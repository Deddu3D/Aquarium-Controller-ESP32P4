/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine implementation
 * Drives automatic LED animations via a dedicated FreeRTOS task.
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

#include "led_controller.h"
#include "led_scenes.h"
#include "geolocation.h"
#include "sun_position.h"

static const char *TAG = "led_scene";

/* ── Constants ───────────────────────────────────────────────────── */

#define SCENE_TICK_MS          30
#define SUNRISE_DURATION_MS    (5 * 60 * 1000)   /* 5 minutes */
#define SUNSET_DURATION_MS     (5 * 60 * 1000)   /* 5 minutes */
#define CLOUDY_PERIOD_FRAMES   267                /* ~8 s at 30 ms/tick */
#define MAX_STORM_FLASHES      3
#define SCENE_TASK_STACK       4096
#define PI_F                   3.14159265f

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

static const keyframe_t sunrise_kf[] = {
    { 0.00f,   0,   0,   0 },   /* black             */
    { 0.15f,  30,   0,   5 },   /* dark red          */
    { 0.30f, 180,  50,   5 },   /* deep orange       */
    { 0.50f, 255, 140,  30 },   /* warm orange       */
    { 0.70f, 255, 200, 100 },   /* warm white        */
    { 1.00f, 200, 220, 255 },   /* daylight          */
};
#define SUNRISE_KF_COUNT  (sizeof(sunrise_kf) / sizeof(sunrise_kf[0]))

static const keyframe_t sunset_kf[] = {
    { 0.00f, 200, 220, 255 },   /* daylight          */
    { 0.25f, 255, 160,  50 },   /* warm orange       */
    { 0.45f, 200,  60,  10 },   /* deep orange/red   */
    { 0.65f,  60,  10,  20 },   /* dark red          */
    { 0.85f,  10,   5,  30 },   /* dark blue         */
    { 1.00f,   5,   8,  40 },   /* moonlight         */
};
#define SUNSET_KF_COUNT  (sizeof(sunset_kf) / sizeof(sunset_kf[0]))

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex        = NULL;
static TaskHandle_t      s_task         = NULL;
static led_scene_t       s_active_scene = LED_SCENE_OFF;

/* Storm lightning flash tracking */
static struct {
    uint16_t led;
    uint8_t  frames_left;
    bool     active;
} s_storm_flashes[MAX_STORM_FLASHES];

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

/* ── Render functions ────────────────────────────────────────────── */

/**
 * @brief Render static moonlight (dim blue) across the whole strip.
 */
static void render_moonlight(uint16_t num_leds)
{
    for (uint16_t i = 0; i < num_leds; i++) {
        led_controller_set_pixel(i, 5, 8, 40);
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
 */
static void render_cloudy(uint32_t frame, uint16_t num_leds)
{
    const float base_r = 200.0f, base_g = 220.0f, base_b = 255.0f;

    for (uint16_t i = 0; i < num_leds; i++) {
        float phase = (float)i / (float)num_leds * 2.0f * PI_F;
        float wave  = sinf(2.0f * PI_F * (float)frame
                           / (float)CLOUDY_PERIOD_FRAMES + phase);
        float factor = 1.0f + wave * 0.15f;

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

/* ── Scene enter (one-time setup on scene change) ────────────────── */

static void scene_enter(led_scene_t scene)
{
    ESP_LOGI(TAG, "Entering scene: %s", s_scene_names[scene]);

    /* Reset storm flash state for all scenes */
    memset(s_storm_flashes, 0, sizeof(s_storm_flashes));

    switch (scene) {
    case LED_SCENE_DAYLIGHT:
        led_controller_set_brightness(255);
        led_controller_set_color(200, 220, 255);
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
    case LED_SCENE_FULL_DAY_CYCLE:
        led_controller_set_brightness(255);
        led_controller_set_color(0, 0, 0);
        led_controller_on();
        break;

    case LED_SCENE_OFF:
    default:
        /* Manual control – leave LEDs as they are */
        break;
    }
}

/* ── Scene task ──────────────────────────────────────────────────── */

static void led_scene_task(void *arg)
{
    (void)arg;
    uint32_t    frame      = 0;
    led_scene_t last_scene = LED_SCENE_OFF;

    while (1) {
        /* Read current scene under mutex */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_scene_t scene = s_active_scene;
        xSemaphoreGive(s_mutex);

        /* Detect scene change */
        if (scene != last_scene) {
            frame = 0;
            last_scene = scene;
            scene_enter(scene);
        }

        uint16_t num_leds = led_controller_get_num_leds();

        switch (scene) {
        /* ── Static scenes: idle after initial setup ────────────── */
        case LED_SCENE_OFF:
        case LED_SCENE_DAYLIGHT:
        case LED_SCENE_MOONLIGHT:
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;   /* skip frame++ and normal tick delay */

        /* ── Sunrise animation ──────────────────────────────────── */
        case LED_SCENE_SUNRISE: {
            uint32_t total = SUNRISE_DURATION_MS / SCENE_TICK_MS;
            float progress = (float)frame / (float)total;
            if (progress >= 1.0f) {
                /* Auto-transition to daylight */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active_scene = LED_SCENE_DAYLIGHT;
                xSemaphoreGive(s_mutex);
            } else {
                render_sunrise(progress, num_leds);
            }
            break;
        }

        /* ── Sunset animation ───────────────────────────────────── */
        case LED_SCENE_SUNSET: {
            uint32_t total = SUNSET_DURATION_MS / SCENE_TICK_MS;
            float progress = (float)frame / (float)total;
            if (progress >= 1.0f) {
                /* Auto-transition to moonlight */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active_scene = LED_SCENE_MOONLIGHT;
                xSemaphoreGive(s_mutex);
            } else {
                render_sunset(progress, num_leds);
            }
            break;
        }

        /* ── Cloudy (continuous) ────────────────────────────────── */
        case LED_SCENE_CLOUDY:
            render_cloudy(frame, num_leds);
            break;

        /* ── Storm (continuous) ─────────────────────────────────── */
        case LED_SCENE_STORM:
            render_storm(num_leds);
            break;

        /* ── Full 24 h day cycle (real-time + geolocation) ────────── */
        case LED_SCENE_FULL_DAY_CYCLE: {
            /* Get current wall-clock time */
            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);

            /* If system time is not set (year < 2024) fall back to
             * uptime so the scene still works without NTP.          */
            int now_min;    /* minutes since local midnight */
            int cur_year, cur_month, cur_day;
            bool have_time = (timeinfo.tm_year >= (2024 - 1900));

            if (have_time) {
                now_min   = timeinfo.tm_hour * 60 + timeinfo.tm_min;
                cur_year  = timeinfo.tm_year + 1900;
                cur_month = timeinfo.tm_mon + 1;
                cur_day   = timeinfo.tm_mday;
            } else {
                /* Fallback: use uptime mapped to a 24 h day */
                int64_t up_s = esp_timer_get_time() / 1000000;
                int     sec_of_day = (int)(up_s % 86400);
                now_min   = sec_of_day / 60;
                cur_year  = 2026;
                cur_month = 6;
                cur_day   = 21;   /* summer solstice as default */
            }

            /* Compute sunrise / sunset from geolocation */
            geolocation_config_t geo = geolocation_get();
            sun_times_t st = sun_position_calc(
                geo.latitude, geo.longitude, geo.utc_offset_min,
                cur_year, cur_month, cur_day);

            int sr_start, sr_end, ss_start, ss_end;

            if (st.valid) {
                /* 30-min sunrise transition centred on calculated time */
                sr_start = st.sunrise_min - 15;
                sr_end   = st.sunrise_min + 15;
                /* 30-min sunset transition centred on calculated time */
                ss_start = st.sunset_min - 15;
                ss_end   = st.sunset_min + 15;
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

            if (now_min >= sr_start && now_min < sr_end) {
                /* Sunrise transition */
                float p = (float)(now_min - sr_start) /
                          (float)(sr_end - sr_start);
                render_sunrise(p, num_leds);
            } else if (now_min >= sr_end && now_min < ss_start) {
                /* Daytime – cloudy variation */
                render_cloudy(frame, num_leds);
            } else if (now_min >= ss_start && now_min < ss_end) {
                /* Sunset transition */
                float p = (float)(now_min - ss_start) /
                          (float)(ss_end - ss_start);
                render_sunset(p, num_leds);
            } else {
                /* Night – moonlight */
                render_moonlight(num_leds);
            }
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active_scene = scene;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Scene set: %s", s_scene_names[scene]);
    return ESP_OK;
}

led_scene_t led_scenes_get(void)
{
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
