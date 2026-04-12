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

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "auto_heater.h"
#include "temperature_sensor.h"
#include "relay_controller.h"

static const char *TAG = "heater";

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE   "auto_heater"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_RELAY   "relay_idx"
#define NVS_KEY_TARGET  "target"
#define NVS_KEY_HYST    "hysteresis"

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t    s_mutex  = NULL;
static auto_heater_config_t s_config;
static bool                 s_heater_on = false;   /* last relay command */

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Defaults */
    s_config.enabled      = false;
    s_config.relay_index  = 0;
    s_config.target_temp_c = 25.0f;
    s_config.hysteresis_c  = 0.5f;

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

    int32_t i32;
    if (nvs_get_i32(h, NVS_KEY_TARGET, &i32) == ESP_OK)
        s_config.target_temp_c = (float)i32 / 100.0f;
    if (nvs_get_i32(h, NVS_KEY_HYST, &i32) == ESP_OK)
        s_config.hysteresis_c = (float)i32 / 100.0f;

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: enabled=%d relay=%d target=%.1f hyst=%.1f",
             s_config.enabled, s_config.relay_index,
             (double)s_config.target_temp_c, (double)s_config.hysteresis_c);
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
    nvs_set_i32(h, NVS_KEY_TARGET, (int32_t)(s_config.target_temp_c * 100.0f));
    nvs_set_i32(h, NVS_KEY_HYST, (int32_t)(s_config.hysteresis_c * 100.0f));
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
    auto_heater_config_t cfg = s_config;
    bool currently_on = s_heater_on;
    xSemaphoreGive(s_mutex);

    if (!cfg.enabled) {
        return;
    }

    float temp_c;
    if (!temperature_sensor_get(&temp_c)) {
        /* No valid reading – don't change state */
        return;
    }

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
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Heater %s (temp=%.1f target=%.1f±%.1f)",
                 new_state ? "ON" : "OFF",
                 (double)temp_c,
                 (double)cfg.target_temp_c,
                 (double)cfg.hysteresis_c);
    }
}
