/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - WiFi Manager implementation
 * Handles WiFi STA connection lifecycle via ESP32-C6 coprocessor.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "wifi_manager.h"

/* ── Configuration ───────────────────────────────────────────────── */

/* SSID and password can be overridden via menuconfig
 * (Component config → Aquarium WiFi Settings) or at compile time.      */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "your_ssid"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "your_password"
#endif

/* ── Private constants ───────────────────────────────────────────── */

static const char *TAG = "wifi_mgr";

/* FreeRTOS event-group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Reconnection back-off parameters */
#define WIFI_BACKOFF_INITIAL_MS   1000   /* first retry after 1 s          */
#define WIFI_BACKOFF_MAX_MS       60000  /* cap at 60 s between retries    */
#define WIFI_INIT_TIMEOUT_MS      30000  /* give up blocking init after 30s*/

/* ── Private state ───────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count;
static bool               s_is_connected;
static esp_netif_t       *s_sta_netif;
static uint32_t           s_backoff_ms;      /* current back-off interval */

/* ── Event handler ───────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started – connecting …");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_is_connected = false;
            s_retry_count++;
            /* Exponential back-off: double the wait each time, capped */
            ESP_LOGW(TAG, "Disconnected – retry %d (backoff %"PRIu32" ms)",
                     s_retry_count, s_backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
            s_backoff_ms *= 2;
            if (s_backoff_ms > WIFI_BACKOFF_MAX_MS) {
                s_backoff_ms = WIFI_BACKOFF_MAX_MS;
            }
            esp_wifi_connect();
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count  = 0;
        s_backoff_ms   = WIFI_BACKOFF_INITIAL_MS;   /* reset back-off */
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count      = 0;
    s_is_connected     = false;
    s_backoff_ms       = WIFI_BACKOFF_INITIAL_MS;

    /* ---- TCP/IP & event loop ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* ---- WiFi driver init ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ---- Register event handlers ---- */
    esp_event_handler_instance_t instance_any_wifi;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &instance_any_wifi));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &instance_got_ip));

    /* ---- WiFi STA configuration ---- */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* Copy SSID and password from Kconfig / compile-time defines */
    strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialisation complete – waiting for connection …");

    /* ---- Block until connected or timeout ---- */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_INIT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to SSID: %s", CONFIG_WIFI_SSID);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Initial connection timed out after %d ms – "
             "retries continue in background", WIFI_INIT_TIMEOUT_MS);
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

void wifi_manager_get_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (s_is_connected && s_sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }

    strncpy(buf, "0.0.0.0", len - 1);
    buf[len - 1] = '\0';
}
