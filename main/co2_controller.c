/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - CO2 Solenoid Controller implementation
 * Opens / closes a relay-driven solenoid valve in sync with the
 * lights relay schedule to avoid CO2 waste and pH swings.
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

#include "co2_controller.h"
#include "relay_controller.h"

static const char *TAG = "co2";

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE      "co2"
#define NVS_KEY_ENABLED    "enabled"
#define NVS_KEY_RELAY      "relay_idx"
#define NVS_KEY_PRE_ON     "pre_on"
#define NVS_KEY_POST_OFF   "post_off"
#define MINUTES_PER_DAY    1440

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex   = NULL;
static co2_config_t      s_config;
static bool              s_valve_on = false;   /* last commanded state */
static const int         s_lights_relay_index = 0;

/* Return true when now_min is inside the half-open interval
 * [start_min, end_min), supporting windows that cross midnight
 * (start_min > end_min). */
static bool minute_in_window(int now_min, int start_min, int end_min)
{
    if (start_min <= end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;
}

/* Check whether any enabled schedule slot is active at now_min after applying
 * pre-on and post-off margins (minutes), with wrap-around at midnight.
 * When out_start_min/out_end_min are provided, the active adjusted window
 * is returned for diagnostics. */
static bool relay_schedule_active_now(const relay_schedule_t relay_schedules[RELAY_SCHEDULE_SLOTS],
                                      int now_min, int pre_on_min, int post_off_min,
                                      int *out_start_min, int *out_end_min)
{
    for (int i = 0; i < RELAY_SCHEDULE_SLOTS; i++) {
        if (!relay_schedules[i].enabled) {
            continue;
        }
        int on_m = ((int)relay_schedules[i].on_min - pre_on_min + MINUTES_PER_DAY) % MINUTES_PER_DAY;
        int off_m = ((int)relay_schedules[i].off_min + post_off_min + MINUTES_PER_DAY) % MINUTES_PER_DAY;
        if (minute_in_window(now_min, on_m, off_m)) {
            if (out_start_min) *out_start_min = on_m;
            if (out_end_min) *out_end_min = off_m;
            return true;
        }
    }
    return false;
}

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Defaults */
    s_config.enabled      = false;
    s_config.relay_index  = 3;   /* relay 4 by default */
    s_config.pre_on_min   = 0;
    s_config.post_off_min = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved CO2 config – using defaults");
        return;
    }

    uint8_t u8;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8) == ESP_OK)
        s_config.enabled = (u8 != 0);
    if (nvs_get_u8(h, NVS_KEY_RELAY, &u8) == ESP_OK)
        s_config.relay_index = (int)u8;

    int32_t i32;
    if (nvs_get_i32(h, NVS_KEY_PRE_ON, &i32) == ESP_OK)
        s_config.pre_on_min = (int)i32;
    if (nvs_get_i32(h, NVS_KEY_POST_OFF, &i32) == ESP_OK)
        s_config.post_off_min = (int)i32;

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: enabled=%d relay=%d pre=%d post=%d",
             s_config.enabled, s_config.relay_index,
             s_config.pre_on_min, s_config.post_off_min);
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
    nvs_set_i32(h, NVS_KEY_PRE_ON, (int32_t)s_config.pre_on_min);
    nvs_set_i32(h, NVS_KEY_POST_OFF, (int32_t)s_config.post_off_min);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t co2_controller_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();

    ESP_LOGI(TAG, "CO2 controller module initialised");
    return ESP_OK;
}

co2_config_t co2_controller_get_config(void)
{
    co2_config_t cfg;
    if (s_mutex == NULL) {
        memset(&cfg, 0, sizeof(cfg));
        return cfg;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t co2_controller_set_config(const co2_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    co2_config_t safe = *cfg;

    /* Clamp values */
    if (safe.relay_index < 0 || safe.relay_index >= RELAY_COUNT)
        safe.relay_index = 3;
    if (safe.pre_on_min < 0)   safe.pre_on_min = 0;
    if (safe.pre_on_min > 60)  safe.pre_on_min = 60;
    if (safe.post_off_min < 0)  safe.post_off_min = 0;
    if (safe.post_off_min > 60) safe.post_off_min = 60;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;

    /* If disabled, close valve immediately */
    if (!safe.enabled && s_valve_on) {
        s_valve_on = false;
        xSemaphoreGive(s_mutex);
        relay_controller_set(safe.relay_index, false);
    } else {
        xSemaphoreGive(s_mutex);
    }

    esp_err_t err = nvs_save_config();
    ESP_LOGI(TAG, "Config updated: enabled=%d relay=%d pre=%d post=%d",
             safe.enabled, safe.relay_index,
             safe.pre_on_min, safe.post_off_min);
    return err;
}

void co2_controller_tick(void)
{
    if (s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    co2_config_t cfg     = s_config;
    bool currently_on    = s_valve_on;
    xSemaphoreGive(s_mutex);

    if (!cfg.enabled) {
        return;
    }

    /* Require a valid system clock */
    time_t now = time(NULL);
    struct tm ti;
    if (localtime_r(&now, &ti) == NULL || ti.tm_year < (2024 - 1900)) {
        return;
    }

    int now_min = ti.tm_hour * 60 + ti.tm_min;

    relay_state_t relays[RELAY_COUNT];
    relay_controller_get_all(relays);

    int active_start_min = -1;
    int active_end_min = -1;
    bool should_be_on = relay_schedule_active_now(relays[s_lights_relay_index].schedules,
                                                  now_min,
                                                  cfg.pre_on_min,
                                                  cfg.post_off_min,
                                                  &active_start_min,
                                                  &active_end_min);
    if (!should_be_on) {
        /* No active lights schedule window – close valve and return. */
        if (currently_on) {
            relay_controller_set(cfg.relay_index, false);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_valve_on = false;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "CO2 OFF (no active lights schedule)");
        }
        return;
    }
    if (should_be_on != currently_on) {
        relay_controller_set(cfg.relay_index, should_be_on);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_valve_on = should_be_on;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "CO2 solenoid %s (relay %d, now=%02d:%02d window=%02d:%02d–%02d:%02d lights_relay=%s)",
                 should_be_on ? "OPEN" : "CLOSED",
                 cfg.relay_index,
                 ti.tm_hour, ti.tm_min,
                 (active_start_min >= 0) ? (active_start_min / 60) : 0,
                 (active_start_min >= 0) ? (active_start_min % 60) : 0,
                 (active_end_min >= 0) ? (active_end_min / 60) : 0,
                 (active_end_min >= 0) ? (active_end_min % 60) : 0,
                 relays[s_lights_relay_index].on ? "ON" : "OFF");
    }
}
