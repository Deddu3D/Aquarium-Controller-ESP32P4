/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine implementation
 * Drives automatic LED animations via a dedicated FreeRTOS task.
 *
 * Features:
 *   - NVS persistence of active scene and configuration
 *   - Configurable sunrise/sunset transition durations
 *   - Lunar-phase modulated moonlight
 *   - Midday siesta (anti-algae dimming) in Full Day Cycle
 *   - Configurable daylight colour temperature (6500–20000 K)
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "led_controller.h"
#include "led_scenes.h"
#include "geolocation.h"
#include "sun_position.h"

static const char *TAG = "led_scene";

/* ── Kconfig defaults ────────────────────────────────────────────── */

#ifndef CONFIG_LED_SUNRISE_DURATION_MIN
#define CONFIG_LED_SUNRISE_DURATION_MIN 30
#endif

#ifndef CONFIG_LED_SUNSET_DURATION_MIN
#define CONFIG_LED_SUNSET_DURATION_MIN 30
#endif

#ifndef CONFIG_LED_DEFAULT_COLOR_TEMP_K
#define CONFIG_LED_DEFAULT_COLOR_TEMP_K 10000
#endif

/* ── Constants ───────────────────────────────────────────────────── */

#define SCENE_TICK_MS          30
#define CLOUDY_PERIOD_FRAMES   267                /* ~8 s at 30 ms/tick */
#define MAX_STORM_FLASHES      3
#define SCENE_TASK_STACK       4096
#define PI_F                   3.14159265f

/* Siesta smooth-transition duration in minutes */
#define SIESTA_RAMP_MIN        5

/* Lunar synodic month (days) and reference new-moon Julian Day */
#define SYNODIC_MONTH          29.530588853
#define NEW_MOON_JD            2451550.1   /* Jan 6, 2000 */

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE   "led_scenes"
#define NVS_KEY_SCENE   "active"
#define NVS_KEY_SR_DUR  "sr_dur"
#define NVS_KEY_SS_DUR  "ss_dur"
#define NVS_KEY_FD_TR   "fd_trans"
#define NVS_KEY_SIEN    "siesta_en"
#define NVS_KEY_SIST    "siesta_s"
#define NVS_KEY_SIED    "siesta_e"
#define NVS_KEY_SIPCT   "siesta_pct"
#define NVS_KEY_COLK    "color_k"
#define NVS_KEY_LUNAR   "lunar_en"

/* ── Scene name table ────────────────────────────────────────────── */

static const char *const s_scene_names[LED_SCENE_MAX] = {
    [LED_SCENE_OFF]            = "off",
    [LED_SCENE_DAYLIGHT]       = "daylight",
    [LED_SCENE_SUNRISE]        = "sunrise",
    [LED_SCENE_SUNSET]         = "sunset",
    [LED_SCENE_MOONLIGHT]      = "moonlight",
    [LED_SCENE_CLOUDY]         = "cloudy",
    [LED_SCENE_STORM]          = "storm",
    [LED_SCENE_FULL_DAY_CYCLE] = "full_day_cycle",
};

/* ── Colour keyframes ────────────────────────────────────────────── */

typedef struct {
    float   pos;     /* 0.0 – 1.0 */
    uint8_t r, g, b;
} keyframe_t;

/*
 * The final keyframe in sunrise and first keyframe in sunset use the
 * configurable daylight colour; these are overwritten at scene-enter
 * time by apply_daylight_color_to_keyframes().
 */
static keyframe_t sunrise_kf[] = {
    { 0.00f,   0,   0,   0 },   /* black             */
    { 0.15f,  30,   0,   5 },   /* dark red          */
    { 0.30f, 180,  50,   5 },   /* deep orange       */
    { 0.50f, 255, 140,  30 },   /* warm orange       */
    { 0.70f, 255, 200, 100 },   /* warm white        */
    { 1.00f, 200, 220, 255 },   /* daylight (patched)*/
};
#define SUNRISE_KF_COUNT  (sizeof(sunrise_kf) / sizeof(sunrise_kf[0]))

static keyframe_t sunset_kf[] = {
    { 0.00f, 200, 220, 255 },   /* daylight (patched)*/
    { 0.25f, 255, 160,  50 },   /* warm orange       */
    { 0.45f, 200,  60,  10 },   /* deep orange/red   */
    { 0.65f,  60,  10,  20 },   /* dark red          */
    { 0.85f,  10,   5,  30 },   /* dark blue         */
    { 1.00f,   5,   8,  40 },   /* moonlight         */
};
#define SUNSET_KF_COUNT  (sizeof(sunset_kf) / sizeof(sunset_kf[0]))

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t  s_mutex        = NULL;
static TaskHandle_t       s_task         = NULL;
static led_scene_t        s_active_scene = LED_SCENE_OFF;
static led_scene_config_t s_config;

/* Storm lightning flash tracking */
static struct {
    uint16_t led;
    uint8_t  frames_left;
    bool     active;
} s_storm_flashes[MAX_STORM_FLASHES];

/* ── Daylight colour temperature ─────────────────────────────────── */

/* Pre-defined colour-temperature presets for WS2812B aquarium LEDs.
 * Values are subjective and tuned for visual effect, not CIE accuracy. */
typedef struct {
    uint16_t kelvin;
    uint8_t  r, g, b;
} color_temp_preset_t;

static const color_temp_preset_t CT_PRESETS[] = {
    {  6500, 255, 244, 230 },   /* warm white – planted tanks  */
    {  8000, 230, 232, 255 },   /* neutral white               */
    { 10000, 200, 220, 255 },   /* cool daylight (old default) */
    { 14000, 170, 200, 255 },   /* marine reef                 */
    { 20000, 140, 170, 255 },   /* actinic blue                */
};
#define CT_PRESET_COUNT  (sizeof(CT_PRESETS) / sizeof(CT_PRESETS[0]))

/* Current daylight RGB (derived from s_config.color_temp_kelvin) */
static uint8_t s_day_r = 200, s_day_g = 220, s_day_b = 255;

/**
 * @brief Linearly interpolate the colour-temperature presets to get
 *        an RGB triplet for the requested Kelvin value.
 */
static void kelvin_to_rgb(uint16_t kelvin, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (kelvin <= CT_PRESETS[0].kelvin) {
        *r = CT_PRESETS[0].r;
        *g = CT_PRESETS[0].g;
        *b = CT_PRESETS[0].b;
        return;
    }
    if (kelvin >= CT_PRESETS[CT_PRESET_COUNT - 1].kelvin) {
        *r = CT_PRESETS[CT_PRESET_COUNT - 1].r;
        *g = CT_PRESETS[CT_PRESET_COUNT - 1].g;
        *b = CT_PRESETS[CT_PRESET_COUNT - 1].b;
        return;
    }
    for (size_t i = 0; i < CT_PRESET_COUNT - 1; i++) {
        if (kelvin >= CT_PRESETS[i].kelvin &&
            kelvin <= CT_PRESETS[i + 1].kelvin)
        {
            float t = (float)(kelvin - CT_PRESETS[i].kelvin) /
                      (float)(CT_PRESETS[i + 1].kelvin - CT_PRESETS[i].kelvin);
            *r = (uint8_t)((float)CT_PRESETS[i].r +
                           ((float)CT_PRESETS[i + 1].r - (float)CT_PRESETS[i].r) * t);
            *g = (uint8_t)((float)CT_PRESETS[i].g +
                           ((float)CT_PRESETS[i + 1].g - (float)CT_PRESETS[i].g) * t);
            *b = (uint8_t)((float)CT_PRESETS[i].b +
                           ((float)CT_PRESETS[i + 1].b - (float)CT_PRESETS[i].b) * t);
            return;
        }
    }
    /* Fallback – should not be reached */
    *r = 200; *g = 220; *b = 255;
}

/**
 * @brief Update the daylight RGB from the current colour-temperature
 *        setting and patch the sunrise/sunset keyframes.
 */
static void update_daylight_color(void)
{
    kelvin_to_rgb(s_config.color_temp_kelvin,
                  &s_day_r, &s_day_g, &s_day_b);

    /* Patch sunrise endpoint and sunset start-point */
    sunrise_kf[SUNRISE_KF_COUNT - 1].r = s_day_r;
    sunrise_kf[SUNRISE_KF_COUNT - 1].g = s_day_g;
    sunrise_kf[SUNRISE_KF_COUNT - 1].b = s_day_b;

    sunset_kf[0].r = s_day_r;
    sunset_kf[0].g = s_day_g;
    sunset_kf[0].b = s_day_b;

    ESP_LOGI(TAG, "Daylight color: %d K → R=%d G=%d B=%d",
             s_config.color_temp_kelvin, s_day_r, s_day_g, s_day_b);
}

/* ── Lunar phase ─────────────────────────────────────────────────── */

/**
 * @brief Compute Julian Day Number from calendar date.
 *
 * Uses the standard Gregorian-to-Julian Day conversion formula.
 * Reference: Meeus, J. "Astronomical Algorithms", 2nd ed., ch. 7.
 */
static int julian_day(int year, int month, int day)
{
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

/**
 * @brief Return the moon illumination fraction (0.0–1.0) for a date.
 *
 * 0.0 = new moon (dark), 1.0 = full moon (bright).
 */
static float moon_illumination(int year, int month, int day)
{
    int jdn = julian_day(year, month, day);
    double days_since = (double)jdn - NEW_MOON_JD;
    double phase = fmod(days_since, SYNODIC_MONTH) / SYNODIC_MONTH;
    if (phase < 0.0) {
        phase += 1.0;
    }
    /* 0.0 = new moon, 0.5 = full moon, 1.0 = new moon again
     * Illumination follows a cosine curve centred on 0.5            */
    return 0.5f * (1.0f - cosf(2.0f * PI_F * (float)phase));
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static inline uint8_t clamp_u8(float v)
{
    if (v < 0.0f)   return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t)
{
    return clamp_u8((float)a + ((float)b - (float)a) * t);
}

/**
 * @brief Interpolate an RGB colour along a keyframe sequence.
 */
static void interp_kf(const keyframe_t *kf, int count, float t,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (t <= kf[0].pos) {
        *r = kf[0].r; *g = kf[0].g; *b = kf[0].b;
        return;
    }
    if (t >= kf[count - 1].pos) {
        *r = kf[count - 1].r; *g = kf[count - 1].g; *b = kf[count - 1].b;
        return;
    }
    for (int i = 0; i < count - 1; i++) {
        if (t >= kf[i].pos && t <= kf[i + 1].pos) {
            float lt = (t - kf[i].pos) / (kf[i + 1].pos - kf[i].pos);
            *r = lerp_u8(kf[i].r, kf[i + 1].r, lt);
            *g = lerp_u8(kf[i].g, kf[i + 1].g, lt);
            *b = lerp_u8(kf[i].b, kf[i + 1].b, lt);
            return;
        }
    }
    /* Fallback – should not be reached */
    *r = kf[count - 1].r;
    *g = kf[count - 1].g;
    *b = kf[count - 1].b;
}

/* ── NVS persistence ─────────────────────────────────────────────── */

/**
 * @brief Load scene configuration from NVS.
 */
static void nvs_load_config(void)
{
    /* Defaults */
    s_config.sunrise_duration_min    = CONFIG_LED_SUNRISE_DURATION_MIN;
    s_config.sunset_duration_min     = CONFIG_LED_SUNSET_DURATION_MIN;
    s_config.transition_duration_min = CONFIG_LED_SUNRISE_DURATION_MIN;
    s_config.siesta_enabled          = false;
    s_config.siesta_start_min        = 12 * 60;    /* noon    */
    s_config.siesta_end_min          = 14 * 60;    /* 14:00   */
    s_config.siesta_intensity_pct    = 40;
    s_config.color_temp_kelvin       = CONFIG_LED_DEFAULT_COLOR_TEMP_K;
    s_config.lunar_moonlight         = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved scene config – using defaults");
        return;
    }

    /* Active scene */
    uint8_t scn = 0;
    if (nvs_get_u8(h, NVS_KEY_SCENE, &scn) == ESP_OK && scn < LED_SCENE_MAX) {
        s_active_scene = (led_scene_t)scn;
        ESP_LOGI(TAG, "Restored scene: %s", s_scene_names[s_active_scene]);
    }

    uint16_t u16;
    uint8_t  u8;

    if (nvs_get_u16(h, NVS_KEY_SR_DUR, &u16) == ESP_OK)
        s_config.sunrise_duration_min = u16;
    if (nvs_get_u16(h, NVS_KEY_SS_DUR, &u16) == ESP_OK)
        s_config.sunset_duration_min = u16;
    if (nvs_get_u16(h, NVS_KEY_FD_TR, &u16) == ESP_OK)
        s_config.transition_duration_min = u16;
    if (nvs_get_u8(h, NVS_KEY_SIEN, &u8) == ESP_OK)
        s_config.siesta_enabled = (u8 != 0);
    if (nvs_get_u16(h, NVS_KEY_SIST, &u16) == ESP_OK)
        s_config.siesta_start_min = u16;
    if (nvs_get_u16(h, NVS_KEY_SIED, &u16) == ESP_OK)
        s_config.siesta_end_min = u16;
    if (nvs_get_u8(h, NVS_KEY_SIPCT, &u8) == ESP_OK)
        s_config.siesta_intensity_pct = u8;
    if (nvs_get_u16(h, NVS_KEY_COLK, &u16) == ESP_OK)
        s_config.color_temp_kelvin = u16;
    if (nvs_get_u8(h, NVS_KEY_LUNAR, &u8) == ESP_OK)
        s_config.lunar_moonlight = (u8 != 0);

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: sr=%d ss=%d fdtr=%d siesta=%d "
             "color_k=%d lunar=%d",
             s_config.sunrise_duration_min,
             s_config.sunset_duration_min,
             s_config.transition_duration_min,
             s_config.siesta_enabled,
             s_config.color_temp_kelvin,
             s_config.lunar_moonlight);
}

/**
 * @brief Save the current configuration to NVS.
 */
static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u16(h, NVS_KEY_SR_DUR, s_config.sunrise_duration_min);
    nvs_set_u16(h, NVS_KEY_SS_DUR, s_config.sunset_duration_min);
    nvs_set_u16(h, NVS_KEY_FD_TR,  s_config.transition_duration_min);
    nvs_set_u8(h,  NVS_KEY_SIEN,   s_config.siesta_enabled ? 1 : 0);
    nvs_set_u16(h, NVS_KEY_SIST,   s_config.siesta_start_min);
    nvs_set_u16(h, NVS_KEY_SIED,   s_config.siesta_end_min);
    nvs_set_u8(h,  NVS_KEY_SIPCT,  s_config.siesta_intensity_pct);
    nvs_set_u16(h, NVS_KEY_COLK,   s_config.color_temp_kelvin);
    nvs_set_u8(h,  NVS_KEY_LUNAR,  s_config.lunar_moonlight ? 1 : 0);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/**
 * @brief Save just the active scene to NVS.
 */
static void nvs_save_scene(led_scene_t scene)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_SCENE, (uint8_t)scene);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── Render functions ────────────────────────────────────────────── */

/**
 * @brief Render moonlight across the whole strip.
 *
 * If lunar_moonlight is enabled, intensity is modulated by the real
 * moon phase.  Otherwise uses a fixed dim blue.
 */
static void render_moonlight(uint16_t num_leds,
                             int year, int month, int day)
{
    /* Base moonlight colour */
    const uint8_t base_r = 5, base_g = 8, base_b = 40;

    float scale = 1.0f;
    if (s_config.lunar_moonlight) {
        float illum = moon_illumination(year, month, day);
        /* Map illumination 0.0–1.0 to intensity 0.05–1.0
         * (never completely dark – fish need some light)    */
        scale = 0.05f + 0.95f * illum;
    }

    for (uint16_t i = 0; i < num_leds; i++) {
        led_controller_set_pixel(i,
            clamp_u8((float)base_r * scale),
            clamp_u8((float)base_g * scale),
            clamp_u8((float)base_b * scale));
    }
    led_controller_refresh();
}

/**
 * @brief Render one frame of the sunrise animation.
 *
 * LEDs light up from the centre outward with colour progressing
 * through warm tones toward daylight.
 *
 * @param progress  0.0 (start) to 1.0 (complete).
 */
static void render_sunrise(float progress, uint16_t num_leds)
{
    uint16_t center   = num_leds / 2;
    float    max_dist = (float)center;
    float    spread   = progress * max_dist;
    float    fade_w   = max_dist * 0.12f;
    if (fade_w < 2.0f) fade_w = 2.0f;

    uint8_t r, g, b;
    interp_kf(sunrise_kf, (int)SUNRISE_KF_COUNT, progress, &r, &g, &b);

    for (uint16_t i = 0; i < num_leds; i++) {
        float dist = (float)(i >= center ? i - center : center - i);
        float intensity;

        if (dist <= spread) {
            intensity = 1.0f;
        } else if (dist <= spread + fade_w) {
            intensity = 1.0f - (dist - spread) / fade_w;
        } else {
            intensity = 0.0f;
        }

        led_controller_set_pixel(i,
            clamp_u8((float)r * intensity),
            clamp_u8((float)g * intensity),
            clamp_u8((float)b * intensity));
    }
    led_controller_refresh();
}

/**
 * @brief Render one frame of the sunset animation.
 *
 * Colour transitions from daylight through warm tones to moonlight,
 * with edges dimming slightly faster than the centre.
 */
static void render_sunset(float progress, uint16_t num_leds)
{
    uint16_t center   = num_leds / 2;
    float    max_dist = (float)center;

    uint8_t r, g, b;
    interp_kf(sunset_kf, (int)SUNSET_KF_COUNT, progress, &r, &g, &b);

    for (uint16_t i = 0; i < num_leds; i++) {
        float dist = (float)(i >= center ? i - center : center - i);
        /* Edges dim faster than centre */
        float edge = 1.0f - (dist / max_dist) * progress * 0.3f;
        if (edge < 0.0f) edge = 0.0f;

        led_controller_set_pixel(i,
            clamp_u8((float)r * edge),
            clamp_u8((float)g * edge),
            clamp_u8((float)b * edge));
    }
    led_controller_refresh();
}

/**
 * @brief Render one frame of the cloudy scene.
 *
 * Daylight base with a sinusoidal brightness wave (~±15 %)
 * travelling across the strip (~8 s period).
 *
 * @param dim  Optional dimming factor 0.0–1.0 (1.0 = full intensity).
 */
static void render_cloudy(uint32_t frame, uint16_t num_leds, float dim)
{
    const float base_r = (float)s_day_r;
    const float base_g = (float)s_day_g;
    const float base_b = (float)s_day_b;

    for (uint16_t i = 0; i < num_leds; i++) {
        float phase = (float)i / (float)num_leds * 2.0f * PI_F;
        float wave  = sinf(2.0f * PI_F * (float)frame
                           / (float)CLOUDY_PERIOD_FRAMES + phase);
        float factor = (1.0f + wave * 0.15f) * dim;

        led_controller_set_pixel(i,
            clamp_u8(base_r * factor),
            clamp_u8(base_g * factor),
            clamp_u8(base_b * factor));
    }
    led_controller_refresh();
}

/**
 * @brief Render one frame of the storm scene.
 *
 * Dark blue-grey base with short random white flashes.
 */
static void render_storm(uint16_t num_leds)
{
    /* Base colour: dark blue-grey */
    for (uint16_t i = 0; i < num_leds; i++) {
        led_controller_set_pixel(i, 20, 20, 40);
    }

    /* ~4 % chance per tick to start a new flash */
    if ((esp_random() % 100) < 4) {
        for (int f = 0; f < MAX_STORM_FLASHES; f++) {
            if (!s_storm_flashes[f].active) {
                s_storm_flashes[f].active      = true;
                s_storm_flashes[f].led         = (uint16_t)(esp_random() % num_leds);
                s_storm_flashes[f].frames_left = 2 + (uint8_t)(esp_random() % 3);
                break;
            }
        }
    }

    /* Render active flashes */
    for (int f = 0; f < MAX_STORM_FLASHES; f++) {
        if (s_storm_flashes[f].active) {
            int16_t ctr = (int16_t)s_storm_flashes[f].led;
            int16_t lo  = (int16_t)(ctr - 3);
            int16_t hi  = (int16_t)(ctr + 3);
            if (lo < 0) lo = 0;
            if (hi >= (int16_t)num_leds) hi = (int16_t)(num_leds - 1);

            for (int16_t i = lo; i <= hi; i++) {
                led_controller_set_pixel((uint16_t)i, 255, 255, 255);
            }
            s_storm_flashes[f].frames_left--;
            if (s_storm_flashes[f].frames_left == 0) {
                s_storm_flashes[f].active = false;
            }
        }
    }

    led_controller_refresh();
}

/* ── Siesta helper ───────────────────────────────────────────────── */

/**
 * @brief Compute the daylight dimming factor for siesta.
 *
 * Returns 1.0 during normal daytime, and ramps smoothly down to the
 * siesta intensity percentage during the siesta window.
 *
 * @param now_min  Current time in minutes from midnight.
 * @return Dimming factor 0.0–1.0.
 */
static float siesta_dim_factor(int now_min)
{
    if (!s_config.siesta_enabled) {
        return 1.0f;
    }
    int start = (int)s_config.siesta_start_min;
    int end   = (int)s_config.siesta_end_min;
    if (start >= end) {
        return 1.0f;   /* invalid config – skip */
    }

    float target = (float)s_config.siesta_intensity_pct / 100.0f;

    /* Ramp-in zone: [start - RAMP, start] */
    int ramp_in_start = start - SIESTA_RAMP_MIN;
    if (ramp_in_start < 0) ramp_in_start = 0;

    /* Ramp-out zone: [end, end + RAMP] */
    int ramp_out_end = end + SIESTA_RAMP_MIN;
    if (ramp_out_end > 1439) ramp_out_end = 1439;

    if (now_min >= ramp_in_start && now_min < start) {
        /* Ramping into siesta */
        float t = (float)(now_min - ramp_in_start) /
                  (float)(start - ramp_in_start);
        return 1.0f - (1.0f - target) * t;
    }
    if (now_min >= start && now_min < end) {
        /* Inside siesta */
        return target;
    }
    if (now_min >= end && now_min < ramp_out_end) {
        /* Ramping out of siesta */
        float t = (float)(now_min - end) /
                  (float)(ramp_out_end - end);
        return target + (1.0f - target) * t;
    }

    return 1.0f;   /* outside siesta window */
}

/* ── Scene enter (one-time setup on scene change) ────────────────── */

static void scene_enter(led_scene_t scene)
{
    ESP_LOGI(TAG, "Entering scene: %s", s_scene_names[scene]);

    /* Cancel any running fade ramp so that the ramp timer does not
     * compete with the scene task for RMT channel access.           */
    led_controller_cancel_fade();

    /* Reset storm flash state for all scenes */
    memset(s_storm_flashes, 0, sizeof(s_storm_flashes));

    /* Update daylight colour from config (may have changed) */
    update_daylight_color();

    switch (scene) {
    case LED_SCENE_DAYLIGHT:
        led_controller_set_brightness(255);
        led_controller_set_color(s_day_r, s_day_g, s_day_b);
        led_controller_on();
        break;

    case LED_SCENE_MOONLIGHT:
        led_controller_set_brightness(255);
        led_controller_set_color(5, 8, 40);
        led_controller_on();
        break;

    case LED_SCENE_SUNRISE:
    case LED_SCENE_SUNSET:
    case LED_SCENE_CLOUDY:
    case LED_SCENE_STORM:
        led_controller_set_brightness(255);
        led_controller_set_color(0, 0, 0);
        led_controller_on();
        break;

    case LED_SCENE_FULL_DAY_CYCLE:
        /* Don't set colour to black – the task loop will immediately
         * render the correct phase based on the current time of day. */
        led_controller_set_brightness(255);
        led_controller_on();
        break;

    case LED_SCENE_OFF:
    default:
        /* Manual control – leave LEDs as they are */
        break;
    }
}

/* ── Scene task ──────────────────────────────────────────────────── */

/**
 * @brief Get current date/time info.  Falls back to uptime if NTP
 *        has not synchronised yet.
 */
static void get_current_time(int *out_min, int *out_year,
                             int *out_month, int *out_day,
                             int *out_sec_of_day)
{
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    if (ti.tm_year >= (2024 - 1900)) {
        *out_min   = ti.tm_hour * 60 + ti.tm_min;
        *out_year  = ti.tm_year + 1900;
        *out_month = ti.tm_mon + 1;
        *out_day   = ti.tm_mday;
        if (out_sec_of_day)
            *out_sec_of_day = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
    } else {
        /* Fallback: use uptime mapped to a 24 h day */
        int64_t up_s = esp_timer_get_time() / 1000000;
        int sec_of_day = (int)(up_s % 86400);
        *out_min   = sec_of_day / 60;
        *out_year  = 2026;
        *out_month = 6;
        *out_day   = 21;   /* summer solstice as default */
        if (out_sec_of_day)
            *out_sec_of_day = sec_of_day;
    }
}

static void led_scene_task(void *arg)
{
    (void)arg;
    uint32_t    frame      = 0;
    led_scene_t last_scene = LED_SCENE_OFF;

    /* If a scene was restored from NVS, force an initial enter */
    bool first_tick = true;

    while (1) {
        /* Read current scene and config under mutex */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_scene_t scene = s_active_scene;
        led_scene_config_t cfg = s_config;
        xSemaphoreGive(s_mutex);

        /* Detect scene change */
        if (scene != last_scene || first_tick) {
            frame = 0;
            last_scene = scene;
            first_tick = false;
            scene_enter(scene);
        }

        uint16_t num_leds = led_controller_get_num_leds();

        /* Get current date/time (used by moonlight, full-day, etc.) */
        int now_min, cur_year, cur_month, cur_day, sec_of_day;
        get_current_time(&now_min, &cur_year, &cur_month, &cur_day,
                         &sec_of_day);

        switch (scene) {
        /* ── Static scenes: idle after initial setup ────────────── */
        case LED_SCENE_OFF:
        case LED_SCENE_DAYLIGHT:
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;   /* skip frame++ and normal tick delay */

        /* ── Moonlight (lunar-phase aware) ─────────────────────── */
        case LED_SCENE_MOONLIGHT:
            led_controller_lock();
            render_moonlight(num_leds, cur_year, cur_month, cur_day);
            led_controller_unlock();
            vTaskDelay(pdMS_TO_TICKS(1000));  /* update once per second */
            continue;

        /* ── Sunrise animation ──────────────────────────────────── */
        case LED_SCENE_SUNRISE: {
            uint32_t dur_ms = (uint32_t)cfg.sunrise_duration_min * 60 * 1000;
            uint32_t total  = dur_ms / SCENE_TICK_MS;
            float progress = (float)frame / (float)total;
            if (progress >= 1.0f) {
                /* Auto-transition to daylight */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active_scene = LED_SCENE_DAYLIGHT;
                xSemaphoreGive(s_mutex);
                nvs_save_scene(LED_SCENE_DAYLIGHT);
            } else {
                led_controller_lock();
                render_sunrise(progress, num_leds);
                led_controller_unlock();
            }
            break;
        }

        /* ── Sunset animation ───────────────────────────────────── */
        case LED_SCENE_SUNSET: {
            uint32_t dur_ms = (uint32_t)cfg.sunset_duration_min * 60 * 1000;
            uint32_t total  = dur_ms / SCENE_TICK_MS;
            float progress = (float)frame / (float)total;
            if (progress >= 1.0f) {
                /* Auto-transition to moonlight */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active_scene = LED_SCENE_MOONLIGHT;
                xSemaphoreGive(s_mutex);
                nvs_save_scene(LED_SCENE_MOONLIGHT);
            } else {
                led_controller_lock();
                render_sunset(progress, num_leds);
                led_controller_unlock();
            }
            break;
        }

        /* ── Cloudy (continuous) ────────────────────────────────── */
        case LED_SCENE_CLOUDY:
            led_controller_lock();
            render_cloudy(frame, num_leds, 1.0f);
            led_controller_unlock();
            break;

        /* ── Storm (continuous) ─────────────────────────────────── */
        case LED_SCENE_STORM:
            led_controller_lock();
            render_storm(num_leds);
            led_controller_unlock();
            break;

        /* ── Full 24 h day cycle (real-time + geolocation) ────────── */
        case LED_SCENE_FULL_DAY_CYCLE: {
            /* Compute sunrise / sunset from geolocation */
            geolocation_config_t geo = geolocation_get();
            sun_times_t st = sun_position_calc(
                geo.latitude, geo.longitude, geo.utc_offset_min,
                cur_year, cur_month, cur_day);

            int half_tr = (int)cfg.transition_duration_min / 2;
            int sr_start, sr_end, ss_start, ss_end;

            if (st.valid) {
                sr_start = st.sunrise_min - half_tr;
                sr_end   = st.sunrise_min + half_tr;
                ss_start = st.sunset_min - half_tr;
                ss_end   = st.sunset_min + half_tr;
            } else {
                /* Polar edge-case: default to fixed schedule */
                sr_start = 6 * 60;
                sr_end   = 7 * 60;
                ss_start = 18 * 60;
                ss_end   = 19 * 60;
            }

            /* Clamp to valid range and ensure start < end */
            if (sr_start < 0)     sr_start = 0;
            if (sr_end   > 1439)  sr_end   = 1439;
            if (ss_start < 0)     ss_start = 0;
            if (ss_end   > 1439)  ss_end   = 1439;
            if (sr_start >= sr_end) { sr_start = 6 * 60;  sr_end = 7 * 60;  }
            if (ss_start >= ss_end) { ss_start = 18 * 60; ss_end = 19 * 60; }
            if (sr_end > ss_start)  { sr_end = ss_start; }

            led_controller_lock();
            if (now_min >= sr_start && now_min < sr_end) {
                /* Sunrise transition */
                float p = (float)(now_min - sr_start) /
                          (float)(sr_end - sr_start);
                render_sunrise(p, num_leds);
            } else if (now_min >= sr_end && now_min < ss_start) {
                /* Daytime – cloudy variation with optional siesta.
                 * Use a time-based frame so the animation resumes from
                 * the correct position when the scene is (re-)entered
                 * instead of restarting from frame 0 every time.      */
                float dim = siesta_dim_factor(now_min);
                uint32_t time_frame = (uint32_t)(
                    (uint64_t)sec_of_day * 1000 / SCENE_TICK_MS);
                render_cloudy(time_frame, num_leds, dim);
            } else if (now_min >= ss_start && now_min < ss_end) {
                /* Sunset transition */
                float p = (float)(now_min - ss_start) /
                          (float)(ss_end - ss_start);
                render_sunset(p, num_leds);
            } else {
                /* Night – lunar moonlight */
                render_moonlight(num_leds, cur_year, cur_month, cur_day);
            }
            led_controller_unlock();
            break;
        }

        default:
            break;
        }

        frame++;
        vTaskDelay(pdMS_TO_TICKS(SCENE_TICK_MS));
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t led_scenes_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create scene mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Load persisted config and last-active scene from NVS */
    nvs_load_config();
    update_daylight_color();

    BaseType_t ret = xTaskCreate(led_scene_task, "led_scene",
                                 SCENE_TASK_STACK, NULL,
                                 tskIDLE_PRIORITY + 1, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scene task");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED scene engine initialised");
    return ESP_OK;
}

esp_err_t led_scenes_set(led_scene_t scene)
{
    if (scene >= LED_SCENE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active_scene = scene;
    xSemaphoreGive(s_mutex);

    /* Persist to NVS */
    nvs_save_scene(scene);

    ESP_LOGI(TAG, "Scene set: %s", s_scene_names[scene]);
    return ESP_OK;
}

led_scene_t led_scenes_get(void)
{
    if (s_mutex == NULL) {
        return LED_SCENE_OFF;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_scene_t s = s_active_scene;
    xSemaphoreGive(s_mutex);
    return s;
}

const char *led_scenes_get_name(led_scene_t scene)
{
    if (scene >= LED_SCENE_MAX) {
        return "unknown";
    }
    return s_scene_names[scene];
}

led_scene_t led_scenes_from_name(const char *name)
{
    if (name == NULL) {
        return LED_SCENE_OFF;
    }
    for (int i = 0; i < LED_SCENE_MAX; i++) {
        if (strcmp(name, s_scene_names[i]) == 0) {
            return (led_scene_t)i;
        }
    }
    return LED_SCENE_OFF;
}

void led_scenes_stop(void)
{
    led_scenes_set(LED_SCENE_OFF);
}

led_scene_config_t led_scenes_get_config(void)
{
    led_scene_config_t cfg;
    if (s_mutex == NULL) {
        memset(&cfg, 0, sizeof(cfg));
        return cfg;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t led_scenes_set_config(const led_scene_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Clamp values to valid ranges */
    led_scene_config_t safe = *cfg;
    if (safe.sunrise_duration_min < 1)   safe.sunrise_duration_min = 1;
    if (safe.sunrise_duration_min > 120) safe.sunrise_duration_min = 120;
    if (safe.sunset_duration_min < 1)    safe.sunset_duration_min = 1;
    if (safe.sunset_duration_min > 120)  safe.sunset_duration_min = 120;
    if (safe.transition_duration_min < 1)   safe.transition_duration_min = 1;
    if (safe.transition_duration_min > 120) safe.transition_duration_min = 120;
    if (safe.siesta_start_min > 1439)    safe.siesta_start_min = 1439;
    if (safe.siesta_end_min > 1439)      safe.siesta_end_min = 1439;
    /* Enforce siesta_start < siesta_end; disable if invalid */
    if (safe.siesta_enabled && safe.siesta_start_min >= safe.siesta_end_min) {
        ESP_LOGW(TAG, "Siesta start (%d) >= end (%d) – disabling siesta",
                 safe.siesta_start_min, safe.siesta_end_min);
        safe.siesta_enabled = false;
    }
    if (safe.siesta_intensity_pct > 100) safe.siesta_intensity_pct = 100;
    if (safe.color_temp_kelvin < 6500)   safe.color_temp_kelvin = 6500;
    if (safe.color_temp_kelvin > 20000)  safe.color_temp_kelvin = 20000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;
    xSemaphoreGive(s_mutex);

    update_daylight_color();

    esp_err_t err = nvs_save_config();
    ESP_LOGI(TAG, "Config updated: sr=%d ss=%d fdtr=%d siesta=%d "
             "color_k=%d lunar=%d",
             safe.sunrise_duration_min, safe.sunset_duration_min,
             safe.transition_duration_min, safe.siesta_enabled,
             safe.color_temp_kelvin, safe.lunar_moonlight);
    return err;
}
