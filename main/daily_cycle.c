/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Daily Lighting Cycle implementation
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "daily_cycle.h"
#include "led_controller.h"
#include "led_scenes.h"
#include "sun_position.h"

static const char *TAG = "daily_cycle";

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NS           "daily_cycle"
#define NVS_KEY_ENABLED  "enabled"
#define NVS_KEY_LAT      "lat"   /* stored as int32_t scaled by 10000 */
#define NVS_KEY_LON      "lon"   /* stored as int32_t scaled by 10000 */

/* ── Kconfig defaults ────────────────────────────────────────────── */

#ifndef CONFIG_DAILY_CYCLE_LATITUDE_E4
#define CONFIG_DAILY_CYCLE_LATITUDE_E4   454600   /* 45.46 °N – Milan, Italy */
#endif
#ifndef CONFIG_DAILY_CYCLE_LONGITUDE_E4
#define CONFIG_DAILY_CYCLE_LONGITUDE_E4   91900   /*  9.19 °E – Milan, Italy */
#endif

/* ── Private state ───────────────────────────────────────────────── */

static daily_cycle_config_t s_config = {
    .enabled   = false,
    .latitude  = (float)CONFIG_DAILY_CYCLE_LATITUDE_E4  / 10000.0f,
    .longitude = (float)CONFIG_DAILY_CYCLE_LONGITUDE_E4 / 10000.0f,
};

/* Sentinel value that guarantees the first tick re-evaluates the phase */
#define PHASE_UNINIT  ((daily_cycle_phase_t)(-1))

static daily_cycle_phase_t s_phase    = PHASE_UNINIT;
static int                 s_last_day = -1;
static sun_times_t         s_sun      = { .valid = false };

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8;
    int32_t i32;
    if (nvs_get_u8 (h, NVS_KEY_ENABLED, &u8)  == ESP_OK) s_config.enabled   = (bool)u8;
    if (nvs_get_i32(h, NVS_KEY_LAT,     &i32) == ESP_OK) s_config.latitude   = (float)i32 / 10000.0f;
    if (nvs_get_i32(h, NVS_KEY_LON,     &i32) == ESP_OK) s_config.longitude  = (float)i32 / 10000.0f;

    nvs_close(h);
}

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8 (h, NVS_KEY_ENABLED, (uint8_t)s_config.enabled);
    nvs_set_i32(h, NVS_KEY_LAT,     (int32_t)(s_config.latitude  * 10000.0f));
    nvs_set_i32(h, NVS_KEY_LON,     (int32_t)(s_config.longitude * 10000.0f));
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Phase computation ───────────────────────────────────────────── */

/**
 * @brief Determine which daily phase corresponds to @p cur (minutes from
 *        local midnight) given the sun times and scene durations.
 *
 * Phase boundaries:
 *   Night      : [0,            sunrise)
 *   Sunrise    : [sunrise,      sunrise + sr_dur)
 *   Morning    : [sunrise+dur,  solar_noon - 60)   – skipped on short days
 *   Noon       : [solar_noon-60, solar_noon + 60)
 *   Afternoon  : [solar_noon+60, sunset)
 *   Sunset     : [sunset,        sunset + ss_dur)
 *   Evening    : [sunset+dur,    sunset + dur + 60)
 *   Night      : [sunset+dur+60, 1440)
 */
static daily_cycle_phase_t compute_phase(int cur,
                                         int sr, int ss,
                                         int sr_dur, int ss_dur)
{
    int noon     = (sr + ss) / 2;
    int morn_end = noon - 60;   /* end of morning = 1 h before solar noon */
    int noon_end = noon + 60;   /* end of noon    = 1 h after  solar noon */

    if (cur < sr)                                               return DAILY_PHASE_NIGHT;
    if (cur < sr + sr_dur)                                      return DAILY_PHASE_SUNRISE;
    /* Show MORNING only if there is a gap between sunrise end and noon window */
    if (morn_end > sr + sr_dur && cur < morn_end)               return DAILY_PHASE_MORNING;
    if (cur < noon_end)                                         return DAILY_PHASE_NOON;
    if (cur < ss)                                               return DAILY_PHASE_AFTERNOON;
    if (cur < ss + ss_dur)                                      return DAILY_PHASE_SUNSET;
    if (cur < ss + ss_dur + 60)                                 return DAILY_PHASE_EVENING;
    return DAILY_PHASE_NIGHT;
}

/* ── Phase application ───────────────────────────────────────────── */

static void apply_phase(daily_cycle_phase_t phase)
{
    ESP_LOGI(TAG, "Phase → %d", (int)phase);

    switch (phase) {
    case DAILY_PHASE_NIGHT:
        led_scenes_stop();
        led_controller_off();
        break;

    case DAILY_PHASE_SUNRISE:
        /* Start the sunrise scene only if no scene is currently running */
        if (!led_scenes_is_running()) {
            led_scenes_start(LED_SCENE_SUNRISE);
        }
        break;

    case DAILY_PHASE_MORNING:
        /* Warm daylight white – moderate brightness */
        led_scenes_stop();
        led_controller_cancel_fade();
        led_controller_set_color(255, 200, 140);
        led_controller_set_brightness(200);
        if (!led_controller_is_on()) led_controller_on();
        break;

    case DAILY_PHASE_NOON:
        /* Full-intensity cool white – mimics midday sunlight */
        led_scenes_stop();
        led_controller_cancel_fade();
        led_controller_set_color(200, 220, 255);
        led_controller_set_brightness(255);
        if (!led_controller_is_on()) led_controller_on();
        break;

    case DAILY_PHASE_AFTERNOON:
        /* Warm white, slightly dimmer than noon */
        led_scenes_stop();
        led_controller_cancel_fade();
        led_controller_set_color(255, 190, 120);
        led_controller_set_brightness(220);
        if (!led_controller_is_on()) led_controller_on();
        break;

    case DAILY_PHASE_SUNSET:
        /* Start the sunset scene only if no scene is currently running */
        if (!led_scenes_is_running()) {
            led_scenes_start(LED_SCENE_SUNSET);
        }
        break;

    case DAILY_PHASE_EVENING:
        /* Very dim warm orange – natural twilight glow */
        led_scenes_stop();
        led_controller_cancel_fade();
        led_controller_set_color(180, 60, 10);
        led_controller_set_brightness(30);
        if (!led_controller_is_on()) led_controller_on();
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t daily_cycle_init(void)
{
    nvs_load();
    ESP_LOGI(TAG, "Daily cycle initialised (enabled=%d lat=%.4f lon=%.4f)",
             (int)s_config.enabled, s_config.latitude, s_config.longitude);
    return ESP_OK;
}

daily_cycle_config_t daily_cycle_get_config(void)
{
    return s_config;
}

esp_err_t daily_cycle_set_config(const daily_cycle_config_t *cfg)
{
    if (cfg == NULL)                            return ESP_ERR_INVALID_ARG;
    if (cfg->latitude  < -90.0f  || cfg->latitude  > 90.0f)  return ESP_ERR_INVALID_ARG;
    if (cfg->longitude < -180.0f || cfg->longitude > 180.0f) return ESP_ERR_INVALID_ARG;

    bool was_enabled = s_config.enabled;
    s_config = *cfg;

    /* If module was just disabled, stop scenes and turn LEDs off */
    if (was_enabled && !s_config.enabled) {
        led_scenes_stop();
        led_controller_off();
    }

    /* Force sun-time recalculation and phase re-evaluation on next tick */
    s_last_day = -1;
    s_phase    = PHASE_UNINIT;

    return nvs_save();
}

void daily_cycle_tick(void)
{
    if (!s_config.enabled) return;

    /* ── 1. Get current local time ──────────────────────────────── */
    time_t now = time(NULL);
    if (now < 1000000L) {
        /* System clock not yet synchronised – skip */
        return;
    }

    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);

    /* UTC offset in minutes; handles midnight crossing gracefully */
    int local_tod = lt.tm_hour * 60 + lt.tm_min;
    int utc_tod   = gt.tm_hour * 60 + gt.tm_min;
    int utc_off   = local_tod - utc_tod;
    while (utc_off >  720) utc_off -= 1440;
    while (utc_off < -720) utc_off += 1440;

    /* ── 2. Recompute sun times once per calendar day ────────────── */
    if (lt.tm_mday != s_last_day) {
        s_sun = sun_position_calc(
            (double)s_config.latitude,
            (double)s_config.longitude,
            utc_off,
            lt.tm_year + 1900,
            lt.tm_mon  + 1,
            lt.tm_mday);
        s_last_day = lt.tm_mday;

        if (s_sun.valid) {
            ESP_LOGI(TAG, "Sunrise %02d:%02d  Sunset %02d:%02d",
                     s_sun.sunrise_min / 60, s_sun.sunrise_min % 60,
                     s_sun.sunset_min  / 60, s_sun.sunset_min  % 60);
        } else {
            ESP_LOGW(TAG, "Polar day/night – no sun times today");
        }

        /* Force phase re-evaluation with the freshly computed sun times */
        s_phase = PHASE_UNINIT;
    }

    if (!s_sun.valid) return;   /* polar region – nothing to do */

    /* ── 3. Determine current phase ─────────────────────────────── */
    led_scenes_config_t scene_cfg = led_scenes_get_config();
    int cur = lt.tm_hour * 60 + lt.tm_min;

    daily_cycle_phase_t phase = compute_phase(
        cur,
        s_sun.sunrise_min,
        s_sun.sunset_min,
        (int)scene_cfg.sunrise_duration_min,
        (int)scene_cfg.sunset_duration_min);

    /* ── 4. Apply phase on change ────────────────────────────────── */
    if (phase != s_phase) {
        apply_phase(phase);
        s_phase = phase;
    } else if (phase == DAILY_PHASE_SUNRISE || phase == DAILY_PHASE_SUNSET) {
        /* Re-start scene if it finished or was stopped externally */
        if (!led_scenes_is_running()) {
            apply_phase(phase);
        }
    }
}

daily_cycle_phase_t daily_cycle_get_phase(void)
{
    return (s_phase == PHASE_UNINIT) ? DAILY_PHASE_NIGHT : s_phase;
}

int daily_cycle_get_sunrise_min(void)
{
    return s_sun.valid ? s_sun.sunrise_min : -1;
}

int daily_cycle_get_sunset_min(void)
{
    return s_sun.valid ? s_sun.sunset_min : -1;
}
