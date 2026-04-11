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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

/* ── Private state ───────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count;
static bool               s_is_connected;

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
            if (WIFI_MAXIMUM_RETRY == 0 || s_retry_count < WIFI_MAXIMUM_RETRY) {
                s_retry_count++;
                ESP_LOGW(TAG, "Disconnected – retry %d/%d",
                         s_retry_count,
                         WIFI_MAXIMUM_RETRY ? WIFI_MAXIMUM_RETRY : -1);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Maximum retries reached – giving up");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count  = 0;
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

    /* ---- TCP/IP & event loop ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

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

    /* ---- Block until connected or failed ---- */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to SSID: %s", CONFIG_WIFI_SSID);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to SSID: %s", CONFIG_WIFI_SSID);
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}
