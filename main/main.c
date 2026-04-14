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
#include "esp_task_wdt.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "led_controller.h"
#include "led_schedule.h"
#include "led_scenes.h"
#include "temperature_sensor.h"
#include "temperature_history.h"
#include "telegram_notify.h"
#include "relay_controller.h"
#include "duckdns.h"
#include "auto_heater.h"
#include "display_driver.h"
#include "display_ui.h"

static const char *TAG = "aquarium";

/* ── Background task: initialise MIPI DSI display + LVGL + touch ── */
/* Runs outside app_main so that a disconnected panel cannot block
 * WiFi, web server, or any other service from starting.             */
static void display_init_task(void *arg)
{
    esp_err_t ret = display_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed (0x%x) – continuing without display", ret);
    } else {
        ESP_LOGI(TAG, "MIPI DSI display ready (800x480)");
        ret = display_ui_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Display UI init failed (0x%x)", ret);
        }
    }
    vTaskDelete(NULL);   /* one-shot task */
}

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

    /* ── 1b. Initialise Task Watchdog Timer ───────────────────── */
    {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms    = 30000,  /* 30 s watchdog timeout */
            .idle_core_mask = 0,     /* do not monitor idle tasks */
            .trigger_panic = true,   /* reboot on WDT timeout */
        };
        ret = esp_task_wdt_reconfigure(&wdt_cfg);
        if (ret != ESP_OK) {
            ret = esp_task_wdt_init(&wdt_cfg);
        }
        if (ret == ESP_OK) {
            esp_task_wdt_add(NULL);  /* subscribe main task */
            ESP_LOGI(TAG, "Task watchdog initialised (30 s timeout)");
        } else {
            ESP_LOGW(TAG, "Task WDT init failed (0x%x) – continuing", ret);
        }
    }

    /* ── 2. Bring up WiFi via C6 coprocessor ──────────────────── */
    esp_task_wdt_reset();
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed (0x%x) – continuing without network", ret);
    }

    /* ── 3. Set POSIX timezone for Cagliari, Italy (CET/CEST) ── */
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set: CET-1CEST (Cagliari, Italy)");

    /* ── 4. Start SNTP time synchronisation ─────────────────── */
    esp_task_wdt_reset();
    if (wifi_manager_is_connected()) {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        ESP_LOGI(TAG, "SNTP time sync started – waiting for clock …");

        /* Block until the system clock is synchronised (max 15 s).
         * Without a valid wall-clock time, TLS certificate validation
         * will always fail (the cert appears expired / not-yet-valid). */
        ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
        if (ret == ESP_OK) {
            time_t now = time(NULL);
            struct tm ti;
            localtime_r(&now, &ti);
            ESP_LOGI(TAG, "SNTP synchronised – %04d-%02d-%02d %02d:%02d:%02d",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            ESP_LOGW(TAG, "SNTP sync timed out – HTTPS connections may fail");
        }
    }

    /* ── 5. Initialise LED strip ────────────────────────────────── */
    esp_task_wdt_reset();
    ret = led_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "LED strip ready");

        /* Initialise LED schedule module */
        ret = led_schedule_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LED schedule init failed (0x%x)", ret);
        }

        /* Initialise LED scene engine */
        ret = led_scenes_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LED scenes init failed (0x%x)", ret);
        }
    }

    /* ── 6. Initialise DS18B20 water temperature sensor ─────────── */
    esp_task_wdt_reset();
    ret = temperature_sensor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 init failed (0x%x) – continuing without temperature", ret);
    } else {
        ESP_LOGI(TAG, "DS18B20 temperature sensor ready");

        /* Start temperature history recording for daily chart */
        ret = temperature_history_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Temperature history init failed (0x%x)", ret);
        }
    }

    /* ── 7. Initialise Telegram notification service ────────────── */
    esp_task_wdt_reset();
    ret = telegram_notify_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Telegram init failed (0x%x) – continuing without notifications", ret);
    } else {
        ESP_LOGI(TAG, "Telegram notification service ready");
    }

    /* ── 8. Initialise 4-channel relay controller ────────────────── */
    esp_task_wdt_reset();
    ret = relay_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay controller init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Relay controller ready (4 channels)");
    }

    /* ── 8b. Initialise auto-heater thermostat ───────────────────── */
    esp_task_wdt_reset();
    ret = auto_heater_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Auto-heater init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Auto-heater module ready");
    }

    /* ── 9. Initialise DuckDNS dynamic DNS client ────────────────── */
    esp_task_wdt_reset();
    ret = duckdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DuckDNS init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "DuckDNS client ready");
    }

    /* ── 9b. Initialise MIPI DSI display + LVGL + touch ───────────── */
    /* Run display init in a background task **pinned to CPU 1** so
     * that the ILI9881C busy-wait DSI read-backs cannot starve the
     * main task on CPU 0 (which feeds the task watchdog).
     * If no display is connected these polls may spin indefinitely –
     * isolating them on the second core keeps every other service
     * (WiFi, web server, sensors, relays) fully responsive.          */
    esp_task_wdt_reset();
    {
        BaseType_t xr = xTaskCreatePinnedToCore(
                            display_init_task, "disp_init",
                            8192, NULL, 5, NULL, 1);  /* pin to CPU 1 */
        if (xr != pdPASS) {
            ESP_LOGE(TAG, "Failed to create display init task");
        }
    }

    /* ── 10. Start HTTP status server ─────────────────────────────── */
    if (wifi_manager_is_connected()) {
        ret = web_server_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed (0x%x)", ret);
        }
    }

    /* ── 11. Main application loop ─────────────────────────────────── */
    ESP_LOGI(TAG, "Entering main loop …");
    while (1) {
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "WiFi: connected");
        } else {
            ESP_LOGW(TAG, "WiFi: not connected");
        }

        /* Placeholder – aquarium control logic goes here */

        /* Evaluate LED time-of-day schedule */
        led_schedule_tick();

        /* Evaluate relay time-of-day schedules */
        relay_controller_tick_schedules();

        /* Evaluate auto-heater thermostat logic */
        auto_heater_tick();

        /* Refresh on-screen LVGL dashboard */
        display_ui_refresh();

        esp_task_wdt_reset();   /* feed the watchdog */
        vTaskDelay(pdMS_TO_TICKS(10000));   /* 10 s heartbeat */
    }
}
