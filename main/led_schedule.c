/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Schedule implementation
 * Time-of-day based manual LED control with NVS persistence.
 * Supports colour ramp at turn-on, optional midday pause, and
 * up to LED_PRESET_COUNT named presets stored in NVS.
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

/* ── NVS namespaces and keys ─────────────────────────────────────── */

#define NVS_NS_SCHED      "led_sched"
#define NVS_KEY_ENABLED   "enabled"
#define NVS_KEY_ON_H      "on_h"
#define NVS_KEY_ON_M      "on_m"
#define NVS_KEY_RAMP      "ramp_min"
#define NVS_KEY_PAUSE_EN  "pause_en"
#define NVS_KEY_PS_H      "ps_h"
#define NVS_KEY_PS_M      "ps_m"
#define NVS_KEY_PE_H      "pe_h"
#define NVS_KEY_PE_M      "pe_m"
#define NVS_KEY_P_BR      "p_br"
#define NVS_KEY_P_R       "p_r"
#define NVS_KEY_P_G       "p_g"
#define NVS_KEY_P_B       "p_b"
#define NVS_KEY_OFF_H     "off_h"
#define NVS_KEY_OFF_M     "off_m"
#define NVS_KEY_BRIGHT    "bright"
#define NVS_KEY_RED       "red"
#define NVS_KEY_GREEN     "green"
#define NVS_KEY_BLUE      "blue"

#define NVS_NS_PRESETS    "led_presets"
/* Preset keys: "pN_n" (name string) and "pN_c" (config blob) where N=0..4 */

/* ── Internal phase tracking ─────────────────────────────────────── */

typedef enum {
    SCHED_PHASE_OFF = 0,
    SCHED_PHASE_ON,
    SCHED_PHASE_PAUSE,
} sched_phase_t;

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t     s_mutex      = NULL;
static led_schedule_config_t s_config     = {
    .enabled             = false,
    .on_hour             = 8,
    .on_minute           = 0,
    .ramp_duration_min   = 30,
    .pause_enabled       = false,
    .pause_start_hour    = 12,
    .pause_start_minute  = 0,
    .pause_end_hour      = 14,
    .pause_end_minute    = 0,
    .pause_brightness    = 80,
    .pause_red           = 200,
    .pause_green         = 220,
    .pause_blue          = 255,
    .off_hour            = 22,
    .off_minute          = 0,
    .brightness          = 255,
    .red                 = 200,
    .green               = 220,
    .blue                = 255,
};
static sched_phase_t         s_last_phase = SCHED_PHASE_OFF;
static led_preset_t          s_presets[LED_PRESET_COUNT];

/* ── NVS helpers – schedule ──────────────────────────────────────── */

static void nvs_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SCHED, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved schedule config – using defaults");
        return;
    }

    uint8_t  v8;
    uint16_t v16;

    if (nvs_get_u8(h,  NVS_KEY_ENABLED,  &v8)  == ESP_OK) s_config.enabled            = (v8 != 0);
    if (nvs_get_u8(h,  NVS_KEY_ON_H,     &v8)  == ESP_OK) s_config.on_hour            = v8;
    if (nvs_get_u8(h,  NVS_KEY_ON_M,     &v8)  == ESP_OK) s_config.on_minute          = v8;
    if (nvs_get_u16(h, NVS_KEY_RAMP,     &v16) == ESP_OK) s_config.ramp_duration_min  = v16;
    if (nvs_get_u8(h,  NVS_KEY_PAUSE_EN, &v8)  == ESP_OK) s_config.pause_enabled      = (v8 != 0);
    if (nvs_get_u8(h,  NVS_KEY_PS_H,     &v8)  == ESP_OK) s_config.pause_start_hour   = v8;
    if (nvs_get_u8(h,  NVS_KEY_PS_M,     &v8)  == ESP_OK) s_config.pause_start_minute = v8;
    if (nvs_get_u8(h,  NVS_KEY_PE_H,     &v8)  == ESP_OK) s_config.pause_end_hour     = v8;
    if (nvs_get_u8(h,  NVS_KEY_PE_M,     &v8)  == ESP_OK) s_config.pause_end_minute   = v8;
    if (nvs_get_u8(h,  NVS_KEY_P_BR,     &v8)  == ESP_OK) s_config.pause_brightness   = v8;
    if (nvs_get_u8(h,  NVS_KEY_P_R,      &v8)  == ESP_OK) s_config.pause_red          = v8;
    if (nvs_get_u8(h,  NVS_KEY_P_G,      &v8)  == ESP_OK) s_config.pause_green        = v8;
    if (nvs_get_u8(h,  NVS_KEY_P_B,      &v8)  == ESP_OK) s_config.pause_blue         = v8;
    if (nvs_get_u8(h,  NVS_KEY_OFF_H,    &v8)  == ESP_OK) s_config.off_hour           = v8;
    if (nvs_get_u8(h,  NVS_KEY_OFF_M,    &v8)  == ESP_OK) s_config.off_minute         = v8;
    if (nvs_get_u8(h,  NVS_KEY_BRIGHT,   &v8)  == ESP_OK) s_config.brightness         = v8;
    if (nvs_get_u8(h,  NVS_KEY_RED,      &v8)  == ESP_OK) s_config.red               = v8;
    if (nvs_get_u8(h,  NVS_KEY_GREEN,    &v8)  == ESP_OK) s_config.green             = v8;
    if (nvs_get_u8(h,  NVS_KEY_BLUE,     &v8)  == ESP_OK) s_config.blue              = v8;

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded: en=%d on=%02d:%02d ramp=%dmin pause=%d off=%02d:%02d "
             "br=%d RGB=(%d,%d,%d)",
             s_config.enabled, s_config.on_hour, s_config.on_minute,
             s_config.ramp_duration_min, s_config.pause_enabled,
             s_config.off_hour, s_config.off_minute,
             s_config.brightness, s_config.red, s_config.green, s_config.blue);
}

static void nvs_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SCHED, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return;
    }

    nvs_set_u8(h,  NVS_KEY_ENABLED,  s_config.enabled ? 1 : 0);
    nvs_set_u8(h,  NVS_KEY_ON_H,     s_config.on_hour);
    nvs_set_u8(h,  NVS_KEY_ON_M,     s_config.on_minute);
    nvs_set_u16(h, NVS_KEY_RAMP,     s_config.ramp_duration_min);
    nvs_set_u8(h,  NVS_KEY_PAUSE_EN, s_config.pause_enabled ? 1 : 0);
    nvs_set_u8(h,  NVS_KEY_PS_H,     s_config.pause_start_hour);
    nvs_set_u8(h,  NVS_KEY_PS_M,     s_config.pause_start_minute);
    nvs_set_u8(h,  NVS_KEY_PE_H,     s_config.pause_end_hour);
    nvs_set_u8(h,  NVS_KEY_PE_M,     s_config.pause_end_minute);
    nvs_set_u8(h,  NVS_KEY_P_BR,     s_config.pause_brightness);
    nvs_set_u8(h,  NVS_KEY_P_R,      s_config.pause_red);
    nvs_set_u8(h,  NVS_KEY_P_G,      s_config.pause_green);
    nvs_set_u8(h,  NVS_KEY_P_B,      s_config.pause_blue);
    nvs_set_u8(h,  NVS_KEY_OFF_H,    s_config.off_hour);
    nvs_set_u8(h,  NVS_KEY_OFF_M,    s_config.off_minute);
    nvs_set_u8(h,  NVS_KEY_BRIGHT,   s_config.brightness);
    nvs_set_u8(h,  NVS_KEY_RED,      s_config.red);
    nvs_set_u8(h,  NVS_KEY_GREEN,    s_config.green);
    nvs_set_u8(h,  NVS_KEY_BLUE,     s_config.blue);

    nvs_commit(h);
    nvs_close(h);
}

/* ── NVS helpers – presets ───────────────────────────────────────── */

static void nvs_load_presets(void)
{
    nvs_handle_t h;
    bool ns_ok = (nvs_open(NVS_NS_PRESETS, NVS_READONLY, &h) == ESP_OK);

    for (int i = 0; i < LED_PRESET_COUNT; i++) {
        /* Default name */
        snprintf(s_presets[i].name, LED_PRESET_NAME_LEN, "Preset %d", i + 1);
        s_presets[i].config = s_config;

        if (!ns_ok) {
            continue;
        }

        /* Name */
        char name_key[8];
        snprintf(name_key, sizeof(name_key), "p%d_n", i);
        size_t sz = LED_PRESET_NAME_LEN;
        nvs_get_str(h, name_key, s_presets[i].name, &sz);
        s_presets[i].name[LED_PRESET_NAME_LEN - 1] = '\0';

        /* Config blob */
        char cfg_key[8];
        snprintf(cfg_key, sizeof(cfg_key), "p%d_c", i);
        led_schedule_config_t cfg;
        sz = sizeof(cfg);
        if (nvs_get_blob(h, cfg_key, &cfg, &sz) == ESP_OK &&
            sz == sizeof(led_schedule_config_t)) {
            s_presets[i].config = cfg;
        }
    }

    if (ns_ok) {
        nvs_close(h);
    }
}

static esp_err_t nvs_save_preset(int slot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_PRESETS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open preset NVS");
        return err;
    }

    char name_key[8];
    snprintf(name_key, sizeof(name_key), "p%d_n", slot);
    nvs_set_str(h, name_key, s_presets[slot].name);

    char cfg_key[8];
    snprintf(cfg_key, sizeof(cfg_key), "p%d_c", slot);
    nvs_set_blob(h, cfg_key,
                 &s_presets[slot].config,
                 sizeof(led_schedule_config_t));

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Phase computation ───────────────────────────────────────────── */

/**
 * @brief Determine the lighting phase for the given minute-of-day.
 *
 * Returns SCHED_PHASE_OFF when outside the on/off window,
 * SCHED_PHASE_PAUSE inside the pause sub-window, and
 * SCHED_PHASE_ON otherwise.
 */
static sched_phase_t compute_phase(int now_min,
                                   const led_schedule_config_t *cfg)
{
    if (!cfg->enabled) {
        return SCHED_PHASE_OFF;
    }

    int on_min  = (int)cfg->on_hour  * 60 + (int)cfg->on_minute;
    int off_min = (int)cfg->off_hour * 60 + (int)cfg->off_minute;

    /* Is current time inside the on/off window? */
    bool in_window;
    if (on_min <= off_min) {
        in_window = (now_min >= on_min && now_min < off_min);
    } else {
        /* Midnight-crossing window */
        in_window = (now_min >= on_min || now_min < off_min);
    }

    if (!in_window) {
        return SCHED_PHASE_OFF;
    }

    /* Check pause sub-window (assumed within the same day, no crossing) */
    if (cfg->pause_enabled) {
        int ps = (int)cfg->pause_start_hour * 60 + (int)cfg->pause_start_minute;
        int pe = (int)cfg->pause_end_hour   * 60 + (int)cfg->pause_end_minute;
        if (ps < pe && now_min >= ps && now_min < pe) {
            return SCHED_PHASE_PAUSE;
        }
    }

    return SCHED_PHASE_ON;
}

/* ── Phase transition helpers ────────────────────────────────────── */

static void enter_phase_on(sched_phase_t prev,
                           const led_schedule_config_t *cfg)
{
    led_controller_cancel_fade();
    led_controller_set_color(cfg->red, cfg->green, cfg->blue);
    led_controller_set_brightness(cfg->brightness);

    if (prev == SCHED_PHASE_OFF) {
        if (cfg->ramp_duration_min == 0) {
            /* Instant-on: no fade ramp */
            led_controller_on();
        } else {
            /* Morning ramp from darkness to full brightness */
            uint32_t ramp_ms = (uint32_t)cfg->ramp_duration_min * 60u * 1000u;
            led_controller_fade_on(ramp_ms);
        }
        ESP_LOGI(TAG, "Phase ON (dawn ramp %u min)", cfg->ramp_duration_min);
    } else {
        /* Returning from pause: strip already on, color+brightness updated */
        if (!led_controller_is_on()) {
            led_controller_on();
        }
        ESP_LOGI(TAG, "Phase ON (from pause)");
    }
}

static void enter_phase_pause(const led_schedule_config_t *cfg)
{
    led_controller_cancel_fade();
    led_controller_set_color(cfg->pause_red,
                             cfg->pause_green,
                             cfg->pause_blue);
    led_controller_set_brightness(cfg->pause_brightness);
    if (!led_controller_is_on()) {
        led_controller_on();
    }
    ESP_LOGI(TAG, "Phase PAUSE br=%d RGB=(%d,%d,%d)",
             cfg->pause_brightness,
             cfg->pause_red, cfg->pause_green, cfg->pause_blue);
}

static void enter_phase_off(void)
{
    led_controller_cancel_fade();
    uint32_t ramp_ms = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000u;
    led_controller_fade_off(ramp_ms);
    ESP_LOGI(TAG, "Phase OFF");
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
    nvs_load_presets();

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
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_config = *cfg;

    /* Clamp values to valid ranges */
    if (s_config.on_hour           > 23)  s_config.on_hour           = 23;
    if (s_config.on_minute         > 59)  s_config.on_minute         = 59;
    if (s_config.ramp_duration_min > 120) s_config.ramp_duration_min = 120;
    if (s_config.pause_start_hour  > 23)  s_config.pause_start_hour   = 23;
    if (s_config.pause_start_minute > 59) s_config.pause_start_minute = 59;
    if (s_config.pause_end_hour    > 23)  s_config.pause_end_hour     = 23;
    if (s_config.pause_end_minute  > 59)  s_config.pause_end_minute   = 59;
    if (s_config.off_hour          > 23)  s_config.off_hour           = 23;
    if (s_config.off_minute        > 59)  s_config.off_minute         = 59;

    /* Force re-evaluation on next tick so new settings take effect promptly */
    s_last_phase = SCHED_PHASE_OFF;

    nvs_save_config();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Schedule updated: en=%d on=%02d:%02d ramp=%dmin "
             "off=%02d:%02d br=%d RGB=(%d,%d,%d)",
             s_config.enabled,
             s_config.on_hour, s_config.on_minute,
             s_config.ramp_duration_min,
             s_config.off_hour, s_config.off_minute,
             s_config.brightness, s_config.red, s_config.green, s_config.blue);

    return ESP_OK;
}

void led_schedule_tick(void)
{
    if (s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_schedule_config_t cfg = s_config;
    xSemaphoreGive(s_mutex);

    /* If disabled, ensure LEDs go off */
    if (!cfg.enabled) {
        if (s_last_phase != SCHED_PHASE_OFF) {
            s_last_phase = SCHED_PHASE_OFF;
            enter_phase_off();
        }
        return;
    }

    /* Require a valid system clock */
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    if (ti.tm_year < (2024 - 1900)) {
        return;
    }

    int now_min = ti.tm_hour * 60 + ti.tm_min;
    sched_phase_t phase = compute_phase(now_min, &cfg);

    /* Only act on phase changes */
    if (phase == s_last_phase) {
        return;
    }

    sched_phase_t prev = s_last_phase;
    s_last_phase = phase;

    switch (phase) {
    case SCHED_PHASE_ON:
        enter_phase_on(prev, &cfg);
        break;
    case SCHED_PHASE_PAUSE:
        enter_phase_pause(&cfg);
        break;
    case SCHED_PHASE_OFF:
        enter_phase_off();
        break;
    default:
        break;
    }
}

/* ── Preset API ──────────────────────────────────────────────────── */

bool led_preset_get(int slot, led_preset_t *out)
{
    if (slot < 0 || slot >= LED_PRESET_COUNT || out == NULL) {
        return false;
    }
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_presets[slot];
    xSemaphoreGive(s_mutex);
    return true;
}

esp_err_t led_preset_save(int slot, const char *name,
                           const led_schedule_config_t *cfg)
{
    if (slot < 0 || slot >= LED_PRESET_COUNT ||
        name == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_presets[slot].name, name, LED_PRESET_NAME_LEN - 1);
    s_presets[slot].name[LED_PRESET_NAME_LEN - 1] = '\0';
    s_presets[slot].config = *cfg;
    xSemaphoreGive(s_mutex);

    esp_err_t err = nvs_save_preset(slot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Preset %d saved: \"%s\"", slot, name);
    }
    return err;
}

esp_err_t led_preset_load(int slot)
{
    if (slot < 0 || slot >= LED_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_schedule_config_t cfg = s_presets[slot].config;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Loading preset %d: \"%s\"",
             slot, s_presets[slot].name);

    return led_schedule_set_config(&cfg);
}

