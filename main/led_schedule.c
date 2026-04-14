/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Schedule implementation
 * Time-of-day based manual LED control with NVS persistence.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "led_controller.h"
#include "led_schedule.h"

static const char *TAG = "led_sched";

/* ── Kconfig fallback ────────────────────────────────────────────── */

#ifndef CONFIG_LED_RAMP_DURATION_SEC
#define CONFIG_LED_RAMP_DURATION_SEC 30
#endif

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE   "led_sched"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_ON_H    "on_h"
#define NVS_KEY_ON_M    "on_m"
#define NVS_KEY_OFF_H   "off_h"
#define NVS_KEY_OFF_M   "off_m"
#define NVS_KEY_BRIGHT  "bright"
#define NVS_KEY_RED     "red"
#define NVS_KEY_GREEN   "green"
#define NVS_KEY_BLUE    "blue"

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t   s_mutex  = NULL;
static led_schedule_config_t s_config = {
    .enabled    = false,
    .on_hour    = 8,
    .on_minute  = 0,
    .off_hour   = 22,
    .off_minute = 0,
    .brightness = 128,
    .red        = 255,
    .green      = 255,
    .blue       = 255,
};

/**
 * @brief Track whether the schedule has already turned LEDs on/off
 *        so we don't repeat fade ramps every tick.
 */
static bool s_schedule_active = false;

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved schedule config – using defaults");
        return;
    }

    uint8_t v8;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &v8) == ESP_OK) s_config.enabled    = (v8 != 0);
    if (nvs_get_u8(h, NVS_KEY_ON_H,    &v8) == ESP_OK) s_config.on_hour    = v8;
    if (nvs_get_u8(h, NVS_KEY_ON_M,    &v8) == ESP_OK) s_config.on_minute  = v8;
    if (nvs_get_u8(h, NVS_KEY_OFF_H,   &v8) == ESP_OK) s_config.off_hour   = v8;
    if (nvs_get_u8(h, NVS_KEY_OFF_M,   &v8) == ESP_OK) s_config.off_minute = v8;
    if (nvs_get_u8(h, NVS_KEY_BRIGHT,  &v8) == ESP_OK) s_config.brightness = v8;
    if (nvs_get_u8(h, NVS_KEY_RED,     &v8) == ESP_OK) s_config.red        = v8;
    if (nvs_get_u8(h, NVS_KEY_GREEN,   &v8) == ESP_OK) s_config.green      = v8;
    if (nvs_get_u8(h, NVS_KEY_BLUE,    &v8) == ESP_OK) s_config.blue       = v8;

    nvs_close(h);

    ESP_LOGI(TAG, "Loaded schedule: enabled=%d  on=%02d:%02d  off=%02d:%02d  "
             "bright=%d  RGB=(%d,%d,%d)",
             s_config.enabled,
             s_config.on_hour, s_config.on_minute,
             s_config.off_hour, s_config.off_minute,
             s_config.brightness,
             s_config.red, s_config.green, s_config.blue);
}

static void nvs_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return;
    }

    nvs_set_u8(h, NVS_KEY_ENABLED, s_config.enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_ON_H,    s_config.on_hour);
    nvs_set_u8(h, NVS_KEY_ON_M,    s_config.on_minute);
    nvs_set_u8(h, NVS_KEY_OFF_H,   s_config.off_hour);
    nvs_set_u8(h, NVS_KEY_OFF_M,   s_config.off_minute);
    nvs_set_u8(h, NVS_KEY_BRIGHT,  s_config.brightness);
    nvs_set_u8(h, NVS_KEY_RED,     s_config.red);
    nvs_set_u8(h, NVS_KEY_GREEN,   s_config.green);
    nvs_set_u8(h, NVS_KEY_BLUE,    s_config.blue);

    nvs_commit(h);
    nvs_close(h);
}

/* ── Schedule evaluation ─────────────────────────────────────────── */

/**
 * @brief Check if the current time falls inside the on-window.
 *
 * Supports windows that wrap past midnight
 * (e.g. on=22:00, off=06:00).
 */
static bool is_in_on_window(int now_min, int on_min, int off_min)
{
    if (on_min <= off_min) {
        /* Normal window: e.g. 08:00 – 22:00 */
        return (now_min >= on_min && now_min < off_min);
    }
    /* Midnight-crossing window: e.g. 22:00 – 06:00 */
    return (now_min >= on_min || now_min < off_min);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t led_schedule_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create schedule mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();

    ESP_LOGI(TAG, "LED schedule module initialised");
    return ESP_OK;
}

led_schedule_config_t led_schedule_get_config(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_schedule_config_t copy = s_config;
    xSemaphoreGive(s_mutex);
    return copy;
}

esp_err_t led_schedule_set_config(const led_schedule_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_config = *cfg;

    /* Clamp values */
    if (s_config.on_hour  > 23) s_config.on_hour  = 23;
    if (s_config.on_minute > 59) s_config.on_minute = 59;
    if (s_config.off_hour > 23) s_config.off_hour = 23;
    if (s_config.off_minute > 59) s_config.off_minute = 59;

    nvs_save_config();

    ESP_LOGI(TAG, "Schedule updated: enabled=%d  on=%02d:%02d  off=%02d:%02d  "
             "bright=%d  RGB=(%d,%d,%d)",
             s_config.enabled,
             s_config.on_hour, s_config.on_minute,
             s_config.off_hour, s_config.off_minute,
             s_config.brightness,
             s_config.red, s_config.green, s_config.blue);

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

void led_schedule_tick(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_config.enabled) {
        /* If the schedule was just disabled while active, turn off */
        if (s_schedule_active) {
            s_schedule_active = false;
            xSemaphoreGive(s_mutex);
            uint32_t ramp_ms = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
            led_controller_fade_off(ramp_ms);
            return;
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    /* Get current local time */
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    /* Don't evaluate if time hasn't been set (year < 2024) */
    if (ti.tm_year < (2024 - 1900)) {
        xSemaphoreGive(s_mutex);
        return;
    }

    int now_min = ti.tm_hour * 60 + ti.tm_min;
    int on_min  = s_config.on_hour  * 60 + s_config.on_minute;
    int off_min = s_config.off_hour * 60 + s_config.off_minute;

    /* Capture config for use after releasing mutex */
    uint8_t brightness = s_config.brightness;
    uint8_t red   = s_config.red;
    uint8_t green = s_config.green;
    uint8_t blue  = s_config.blue;

    bool should_be_on = is_in_on_window(now_min, on_min, off_min);

    if (should_be_on && !s_schedule_active) {
        /* Turn on */
        s_schedule_active = true;
        xSemaphoreGive(s_mutex);

        led_controller_set_color(red, green, blue);
        led_controller_set_brightness(brightness);
        uint32_t ramp_ms = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
        led_controller_fade_on(ramp_ms);

        ESP_LOGI(TAG, "Schedule ON – brightness=%d RGB=(%d,%d,%d)",
                 brightness, red, green, blue);
        return;
    }

    if (!should_be_on && s_schedule_active) {
        /* Turn off */
        s_schedule_active = false;
        xSemaphoreGive(s_mutex);

        uint32_t ramp_ms = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
        led_controller_fade_off(ramp_ms);

        ESP_LOGI(TAG, "Schedule OFF");
        return;
    }

    xSemaphoreGive(s_mutex);
}
