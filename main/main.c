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
#include <inttypes.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif_sntp.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "temperature_sensor.h"
#include "temperature_history.h"
#include "telegram_notify.h"
#include "relay_controller.h"
#include "duckdns.h"
#include "auto_heater.h"
#include "co2_controller.h"
#include "timezone_manager.h"
#include "display_ui.h"
#include "feeding_mode.h"
#include "event_log.h"
#include "relay_automation.h"
#include "aquarium_profiles.h"

static const char *TAG = "aquarium";
static const uint32_t DISPLAY_INIT_TASK_STACK_SIZE = 12 * 1024;
static const UBaseType_t DISPLAY_INIT_TASK_PRIORITY = 4; /* above idle, below system-critical tasks */
static const BaseType_t DISPLAY_INIT_TASK_CORE = tskNO_AFFINITY;

/* ── Restart counter (NVS-persisted) ────────────────────────────── */
#define RESTART_NVS_NS  "sys_stats"
#define RESTART_NVS_KEY "boot_count"

static uint32_t s_boot_count = 0;

static void restart_counter_load_and_increment(void)
{
    nvs_handle_t h;
    if (nvs_open(RESTART_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, RESTART_NVS_KEY, &s_boot_count);
        s_boot_count++;
        nvs_set_u32(h, RESTART_NVS_KEY, s_boot_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint32_t app_main_get_boot_count(void) { return s_boot_count; }

/* ── Relay change callback → Telegram + event log ──────────────── */
static void on_relay_change(int index, bool on, const char *source)
{
    char name[RELAY_NAME_MAX];
    relay_controller_get_name(index, name, sizeof(name));
    telegram_notify_relay_change(index, on, name, source);

    char evt[EVENT_MSG_MAX];
    snprintf(evt, sizeof(evt), "Relay %d (%s) %s [%s]",
             index + 1, name, on ? "ON" : "OFF", source);
    event_log_add(EVT_RELAY_CHANGE, evt);
}

static void display_init_task(void *arg)
{
    (void)arg;
    esp_err_t ret = display_ui_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Touch display disabled in Kconfig – skipping");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display UI init failed (0x%x) – continuing without display", ret);
    } else {
        ESP_LOGI(TAG, "Touch display UI ready");
    }
    vTaskDelete(NULL);
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

    /* ── 1a. Restart counter and event log ────────────────────── */
    restart_counter_load_and_increment();
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Boot #%" PRIu32 "  Reset reason: %d", s_boot_count, (int)reset_reason);

    event_log_init();

    {
        static const char * const reason_str[] = {
            "unknown", "power-on", "ext-reset", "sw-reset",
            "exception/panic", "int-watchdog", "task-watchdog", "other-watchdog",
            "deepsleep-wakeup", "brownout", "sdio",
        };
        int ri = (int)reset_reason;
        char boot_msg[64];
        snprintf(boot_msg, sizeof(boot_msg), "Boot #%" PRIu32 " reason: %s",
                 s_boot_count,
                 (ri >= 0 && ri < (int)(sizeof(reason_str)/sizeof(reason_str[0])))
                     ? reason_str[ri] : "unknown");
        event_log_add(EVT_SYSTEM, boot_msg);
    }

    /* ── 1b. Initialise Task Watchdog Timer ───────────────────── */
    {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms    = 45000,  /* 45 s – must exceed WIFI_INIT_TIMEOUT_MS (30 s) */
            .idle_core_mask = 0,     /* do not monitor idle tasks */
            .trigger_panic = true,   /* reboot on WDT timeout */
        };
        ret = esp_task_wdt_reconfigure(&wdt_cfg);
        if (ret != ESP_OK) {
            ret = esp_task_wdt_init(&wdt_cfg);
        }
        if (ret == ESP_OK) {
            esp_task_wdt_add(NULL);  /* subscribe main task */
            ESP_LOGI(TAG, "Task watchdog initialised (45 s timeout)");
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

    /* ── 3. Initialise timezone (load from NVS or use default) ──── */
    timezone_manager_init();
    ESP_LOGI(TAG, "Timezone initialised");

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

    /* ── 5. Initialise DS18B20 water temperature sensor ─────────── */
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

    /* ── 6. Initialise Telegram notification service ────────────── */
    esp_task_wdt_reset();
    ret = telegram_notify_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Telegram init failed (0x%x) – continuing without notifications", ret);
    } else {
        ESP_LOGI(TAG, "Telegram notification service ready");
    }

    /* ── 7. Initialise 4-channel relay controller ────────────────── */
    esp_task_wdt_reset();
    ret = relay_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay controller init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Relay controller ready (4 channels)");
        /* Register Telegram notification callback for relay state changes */
        relay_controller_set_change_cb(on_relay_change);
        /* Run one immediate schedule evaluation so that relays reach their
         * correct schedule state right away rather than waiting up to 60 s
         * for the first periodic tick in the main loop.  The function
         * returns silently if the clock is not yet valid. */
        relay_controller_tick_schedules();
    }

    /* ── 7b. Initialise auto-heater thermostat ───────────────────── */
    esp_task_wdt_reset();
    ret = auto_heater_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Auto-heater init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Auto-heater module ready");
    }

    /* ── 7c. Initialise CO2 solenoid controller ──────────────────── */
    esp_task_wdt_reset();
    ret = co2_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CO2 controller init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "CO2 controller module ready");
    }

    /* ── 7d. Initialise feeding mode module ──────────────────────── */
    esp_task_wdt_reset();
    ret = feeding_mode_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Feeding mode init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Feeding mode module ready");
    }

    /* ── 7e. Initialise relay automation module ───────────────────── */
    esp_task_wdt_reset();
    ret = relay_auto_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay automation init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "Relay automation module ready");
    }

    /* ── 7f. Apply deferred aquarium profile from Android provisioning ── */
    esp_task_wdt_reset();
    {
        nvs_handle_t pr_h;
        if (nvs_open("provisioning", NVS_READWRITE, &pr_h) == ESP_OK) {
            char init_profile[16] = {0};
            size_t plen = sizeof(init_profile);
            if (nvs_get_str(pr_h, "init_profile", init_profile, &plen) == ESP_OK
                    && init_profile[0] != '\0') {
                esp_err_t pret = aquarium_profile_apply(init_profile);
                if (pret == ESP_OK) {
                    ESP_LOGI(TAG, "Applied provisioned profile: %s", init_profile);
                } else {
                    ESP_LOGW(TAG, "Profile apply failed (0x%x)", pret);
                }
                nvs_erase_key(pr_h, "init_profile");
                nvs_commit(pr_h);
            }
            nvs_close(pr_h);
        }
    }

    /* ── 8. Initialise DuckDNS dynamic DNS client ────────────────── */
    esp_task_wdt_reset();
    ret = duckdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DuckDNS init failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "DuckDNS client ready");
    }

    /* ── 9. Start HTTP status server ─────────────────────────────── */
    bool web_server_running = false;
    if (wifi_manager_is_connected()) {
        ret = web_server_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed (0x%x)", ret);
        } else {
            web_server_running = true;
        }
    } else if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "AP mode active – captive portal at http://192.168.4.1");
        ESP_LOGI(TAG, "Connect to '%s' WiFi to configure credentials", "AquariumSetup");
    }

    /* ── 10. Start touch display UI initialisation task ───────────── */
    esp_task_wdt_reset();
    BaseType_t task_create_result = xTaskCreatePinnedToCore(
        display_init_task,
        "display_init",
        DISPLAY_INIT_TASK_STACK_SIZE,
        NULL,
        DISPLAY_INIT_TASK_PRIORITY,
        NULL,
        DISPLAY_INIT_TASK_CORE);
    if (task_create_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to start display init task – continuing without display");
    }

    /* ── 11. Confirm OTA firmware as valid (enables auto-rollback) ── */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA: firmware validated – rollback cancelled");
        }
    }

    /* ── 12. Main application loop (adaptive per-module tick) ────── */
    ESP_LOGI(TAG, "Entering main loop …");

    /* Per-module last-call timestamps (microseconds via esp_timer_get_time) */
    int64_t t_relay_schedule = 0;  /* Relay sched:   60 s */
    int64_t t_heater     = 0;   /* Auto-heater:   30 s */
    int64_t t_relay_automation = 0;   /* Relay auto:    30 s */
    int64_t t_co2        = 0;   /* CO2 valve:     60 s */
    int64_t t_feeding    = 0;   /* Feeding mode:  10 s */

#define TICK_INTERVAL_US(sec)  ((int64_t)(sec) * 1000000LL)
#define SINCE(t)               (esp_timer_get_time() - (t))

    while (1) {
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();

        /* ── Lazy web-server start after captive-portal reconnect ── */
        if (!web_server_running && wifi_manager_is_connected()) {
            ret = web_server_start();
            if (ret == ESP_OK) {
                web_server_running = true;
                ESP_LOGI(TAG, "Web server started after WiFi reconnection");
            }
        }

        /* Feeding mode: 10 s – fast poll for timer expiry */
        if (SINCE(t_feeding) >= TICK_INTERVAL_US(10)) {
            feeding_mode_tick();
            t_feeding = now;
        }

        /* Auto-heater: 30 s */
        if (SINCE(t_heater) >= TICK_INTERVAL_US(30)) {
            auto_heater_tick();
            t_heater = now;
        }

        /* Relay automation: 30 s */
        if (SINCE(t_relay_automation) >= TICK_INTERVAL_US(30)) {
            relay_auto_tick();
            t_relay_automation = now;
        }

        /* Relay schedule: 60 s */
        if (SINCE(t_relay_schedule) >= TICK_INTERVAL_US(60)) {
            relay_controller_tick_schedules();
            t_relay_schedule = now;
        }

        /* CO2 valve: 60 s */
        if (SINCE(t_co2) >= TICK_INTERVAL_US(60)) {
            co2_controller_tick();
            t_co2 = now;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5000));   /* 5 s base sleep */
    }

#undef TICK_INTERVAL_US
#undef SINCE
}
