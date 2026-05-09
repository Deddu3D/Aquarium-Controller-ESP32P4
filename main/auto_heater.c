/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Auto-Heater Controller implementation
 * Simple on/off thermostat with hysteresis using relay + DS18B20.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "auto_heater.h"
#include "temperature_sensor.h"
#include "relay_controller.h"
#include "telegram_notify.h"
#include "event_log.h"

#ifndef RELAY_COUNT
#define RELAY_COUNT 4
#endif

static const char *TAG = "heater";

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE   "auto_heater"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_RELAY   "relay_idx"
#define NVS_KEY_TARGET  "target"
#define NVS_KEY_HYST    "hysteresis"
#define NVS_KEY_RUNAWAY "runaway_en"
#define NVS_KEY_RUN_TMO "runaway_tmo"

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t    s_mutex  = NULL;
static auto_heater_config_t s_config;
static bool                 s_heater_on = false;   /* last relay command */
static time_t               s_heater_on_since = 0; /* time when heater was turned ON */
static bool                 s_runaway_triggered = false; /* guard sent once */

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Defaults */
    s_config.enabled             = false;
    s_config.relay_index         = 0;
    s_config.target_temp_c       = 25.0f;
    s_config.hysteresis_c        = 0.5f;
    s_config.runaway_protection  = true;
    s_config.runaway_timeout_min = 60;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved heater config – using defaults");
        return;
    }

    uint8_t u8;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8) == ESP_OK)
        s_config.enabled = (u8 != 0);
    if (nvs_get_u8(h, NVS_KEY_RELAY, &u8) == ESP_OK)
        s_config.relay_index = (int)u8;
    if (nvs_get_u8(h, NVS_KEY_RUNAWAY, &u8) == ESP_OK)
        s_config.runaway_protection = (u8 != 0);

    int32_t i32;
    if (nvs_get_i32(h, NVS_KEY_TARGET, &i32) == ESP_OK)
        s_config.target_temp_c = (float)i32 / 100.0f;
    if (nvs_get_i32(h, NVS_KEY_HYST, &i32) == ESP_OK)
        s_config.hysteresis_c = (float)i32 / 100.0f;
    if (nvs_get_i32(h, NVS_KEY_RUN_TMO, &i32) == ESP_OK)
        s_config.runaway_timeout_min = (int)i32;

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: enabled=%d relay=%d target=%.1f hyst=%.1f runaway=%d tmo=%d",
             s_config.enabled, s_config.relay_index,
             (double)s_config.target_temp_c, (double)s_config.hysteresis_c,
             s_config.runaway_protection, s_config.runaway_timeout_min);
}

static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, NVS_KEY_ENABLED, s_config.enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_RELAY, (uint8_t)s_config.relay_index);
    nvs_set_u8(h, NVS_KEY_RUNAWAY, s_config.runaway_protection ? 1 : 0);
    nvs_set_i32(h, NVS_KEY_TARGET, (int32_t)(s_config.target_temp_c * 100.0f));
    nvs_set_i32(h, NVS_KEY_HYST, (int32_t)(s_config.hysteresis_c * 100.0f));
    nvs_set_i32(h, NVS_KEY_RUN_TMO, (int32_t)s_config.runaway_timeout_min);
    nvs_commit(h);
    nvs_close(h);

    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t auto_heater_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();

    ESP_LOGI(TAG, "Auto-heater module initialised");
    return ESP_OK;
}

auto_heater_config_t auto_heater_get_config(void)
{
    auto_heater_config_t cfg;
    if (s_mutex == NULL) {
        memset(&cfg, 0, sizeof(cfg));
        return cfg;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t auto_heater_set_config(const auto_heater_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    auto_heater_config_t safe = *cfg;

    /* Clamp values */
    if (safe.relay_index < 0 || safe.relay_index >= RELAY_COUNT) {
        safe.relay_index = 0;
    }
    if (safe.target_temp_c < 15.0f) safe.target_temp_c = 15.0f;
    if (safe.target_temp_c > 35.0f) safe.target_temp_c = 35.0f;
    if (safe.hysteresis_c < 0.1f)   safe.hysteresis_c = 0.1f;
    if (safe.hysteresis_c > 3.0f)   safe.hysteresis_c = 3.0f;
    if (safe.runaway_timeout_min < 10)  safe.runaway_timeout_min = 10;
    if (safe.runaway_timeout_min > 480) safe.runaway_timeout_min = 480;

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;

    /* If disabled, turn off heater immediately */
    if (!safe.enabled && s_heater_on) {
        s_heater_on = false;
        xSemaphoreGive(s_mutex);
        relay_controller_set(safe.relay_index, false);
    } else {
        xSemaphoreGive(s_mutex);
    }

    esp_err_t err = nvs_save_config();
    ESP_LOGI(TAG, "Config updated: enabled=%d relay=%d target=%.1f hyst=%.1f",
             safe.enabled, safe.relay_index,
             (double)safe.target_temp_c, (double)safe.hysteresis_c);
    return err;
}

void auto_heater_tick(void)
{
    if (s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auto_heater_config_t cfg        = s_config;
    bool                 currently_on = s_heater_on;
    time_t               on_since     = s_heater_on_since;
    bool                 runaway_done = s_runaway_triggered;
    xSemaphoreGive(s_mutex);

    if (!cfg.enabled) {
        return;
    }

    float temp_c;
    if (!temperature_sensor_get(&temp_c)) {
        /* No valid reading – don't change state */
        return;
    }

    /* ── Runaway protection ────────────────────────────────────────── */
    if (cfg.runaway_protection && currently_on && !runaway_done && on_since > 0) {
        time_t on_seconds = time(NULL) - on_since;
        int    on_minutes = (int)(on_seconds / 60);
        float  high_threshold = cfg.target_temp_c + cfg.hysteresis_c;

        if (on_minutes >= cfg.runaway_timeout_min && temp_c >= high_threshold) {
            /* Heater has been ON beyond the timeout and temperature is still
             * above the upper threshold – the relay appears stuck or the
             * temperature sensor is reading correctly.  Trigger runaway guard. */
            ESP_LOGE(TAG, "RUNAWAY DETECTED: heater ON %d min, temp=%.1f°C (>%.1f°C)",
                     on_minutes, (double)temp_c, (double)high_threshold);

            /* Force relay off and disable auto-heater */
            relay_controller_set(cfg.relay_index, false);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_heater_on         = false;
            s_heater_on_since   = 0;
            s_runaway_triggered = true;
            s_config.enabled    = false;
            xSemaphoreGive(s_mutex);
            nvs_save_config();

            /* Send Telegram alert */
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "\xe2\x9a\xa0\xef\xb8\x8f <b>ALLARME RISCALDATORE RUNAWAY</b>\n"
                     "Relay %d acceso da %d minuti\n"
                     "Temperatura: %.1f\xc2\xb0" "C (soglia: %.1f\xc2\xb0" "C)\n"
                     "Riscaldatore automatico DISABILITATO.",
                     cfg.relay_index + 1, on_minutes,
                     (double)temp_c, (double)high_threshold);
            telegram_notify_send(msg);

            /* Log the event */
            char evt[96];
            snprintf(evt, sizeof(evt),
                     "Runaway relay %d: %dmin ON, %.1f°C",
                     cfg.relay_index + 1, on_minutes, (double)temp_c);
            event_log_add(EVT_HEATER_RUNAWAY, evt);
            return;
        }
    }

    /* ── Normal thermostat logic ───────────────────────────────────── */
    float low_threshold  = cfg.target_temp_c - cfg.hysteresis_c;
    float high_threshold = cfg.target_temp_c + cfg.hysteresis_c;
    bool new_state = currently_on;

    if (temp_c < low_threshold) {
        new_state = true;   /* Too cold – turn heater ON */
    } else if (temp_c > high_threshold) {
        new_state = false;  /* Warm enough – turn heater OFF */
    }
    /* Otherwise keep current state (hysteresis band) */

    if (new_state != currently_on) {
        relay_controller_set(cfg.relay_index, new_state);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_heater_on = new_state;
        if (new_state) {
            s_heater_on_since   = time(NULL);
            s_runaway_triggered = false;  /* reset guard when heater turns on */
        } else {
            s_heater_on_since = 0;
        }
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Heater %s (temp=%.1f target=%.1f±%.1f)",
                 new_state ? "ON" : "OFF",
                 (double)temp_c,
                 (double)cfg.target_temp_c,
                 (double)cfg.hysteresis_c);
    }
}
