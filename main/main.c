/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Application Entry Point
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 *   - Main MCU  : ESP32-P4  (application processor, no WiFi)
 *   - Co-proc   : ESP32-C6  (WiFi 6 / BLE 5, connected via SDIO)
 *
 * Toolchain    : ESP-IDF v6.0.0 + VS Code
 *
 * Description:
 *   Initialises NVS, brings up the WiFi STA link through the C6
 *   coprocessor (transparently via esp_wifi_remote + esp_hosted),
 *   and enters the main application loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "led_controller.h"
#include "led_scenes.h"
#include "geolocation.h"
#include "temperature_sensor.h"

static const char *TAG = "aquarium";

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Aquarium Controller – ESP32-P4");
    ESP_LOGI(TAG, " Board: Waveshare ESP32-P4-WiFi6 v1.3");
    ESP_LOGI(TAG, "========================================");

    /* ── 1. Initialise NVS (required by WiFi driver) ──────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated – erasing …");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialised");

    /* ── 2. Bring up WiFi via C6 coprocessor ──────────────────── */
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed (0x%x) – continuing without network", ret);
    }

    /* ── 3. Initialise geolocation (NVS-backed lat/lng/UTC) ──── */
    ret = geolocation_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Geolocation init failed (0x%x)", ret);
    }

    /* Set POSIX timezone from geolocation UTC offset */
    {
        geolocation_config_t geo = geolocation_get();
        int abs_off = geo.utc_offset_min < 0 ? -geo.utc_offset_min : geo.utc_offset_min;
        char tz[16];
        /* POSIX TZ sign is inverted: a geolocation offset of +60 min
         * (1 h east of Greenwich) is expressed as UTC-1.              */
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d",
                 geo.utc_offset_min >= 0 ? '-' : '+',
                 abs_off / 60, abs_off % 60);
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone set: %s", tz);
    }

    /* ── 4. Start SNTP time synchronisation ─────────────────── */
    if (wifi_manager_is_connected()) {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        ESP_LOGI(TAG, "SNTP time sync started (pool.ntp.org)");
    }

    /* ── 5. Initialise LED strip ────────────────────────────────── */
    ret = led_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "LED strip ready");

        /* Initialise LED scene engine */
        ret = led_scenes_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LED scene engine init failed (0x%x)", ret);
        }
    }

    /* ── 6. Initialise DS18B20 water temperature sensor ─────────── */
    ret = temperature_sensor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 init failed (0x%x) – continuing without temperature", ret);
    } else {
        ESP_LOGI(TAG, "DS18B20 temperature sensor ready");
    }

    /* ── 7. Start HTTP status server ─────────────────────────────── */
    if (wifi_manager_is_connected()) {
        ret = web_server_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed (0x%x)", ret);
        }
    }

    /* ── 8. Main application loop ─────────────────────────────────── */
    ESP_LOGI(TAG, "Entering main loop …");
    while (1) {
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "WiFi: connected");
        } else {
            ESP_LOGW(TAG, "WiFi: not connected");
        }

        /* Placeholder – aquarium control logic goes here */

        vTaskDelay(pdMS_TO_TICKS(10000));   /* 10 s heartbeat */
    }
}
