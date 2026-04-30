/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine implementation
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "led_scenes.h"
#include "led_controller.h"

static const char *TAG = "led_scenes";

/* ── Scene task parameters ───────────────────────────────────────── */

#define SCENE_TASK_STACK_SIZE  3072
#define SCENE_TASK_PRIORITY    3
#define SCENE_TICK_MS          500    /* update interval */

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NS                 "led_scenes"
#define NVS_KEY_SR_DUR         "sr_dur"
#define NVS_KEY_SR_MAX_BR      "sr_max_br"
#define NVS_KEY_SS_DUR         "ss_dur"
#define NVS_KEY_ML_BR          "ml_br"
#define NVS_KEY_ML_R           "ml_r"
#define NVS_KEY_ML_G           "ml_g"
#define NVS_KEY_ML_B           "ml_b"
#define NVS_KEY_STORM          "storm"
#define NVS_KEY_CLOUD_D        "cloud_d"
#define NVS_KEY_CLOUD_P        "cloud_p"

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t    s_mutex     = NULL;
static TaskHandle_t         s_task      = NULL;
static led_scene_t          s_active    = LED_SCENE_NONE;
static led_scenes_config_t  s_config;

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Defaults */
    s_config.sunrise_duration_min  = 30;
    s_config.sunrise_max_brightness = 255;
    s_config.sunset_duration_min   = 30;
    s_config.moonlight_brightness  = 20;
    s_config.moonlight_r           = 20;
    s_config.moonlight_g           = 40;
    s_config.moonlight_b           = 100;
    s_config.storm_intensity       = 70;
    s_config.clouds_depth          = 40;
    s_config.clouds_period_s       = 120;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8; uint16_t u16;
    if (nvs_get_u16(h, NVS_KEY_SR_DUR,    &u16) == ESP_OK) s_config.sunrise_duration_min   = u16;
    if (nvs_get_u8 (h, NVS_KEY_SR_MAX_BR, &u8)  == ESP_OK) s_config.sunrise_max_brightness = u8;
    if (nvs_get_u8 (h, NVS_KEY_SS_DUR,    &u8)  == ESP_OK) s_config.sunset_duration_min    = u8;
    if (nvs_get_u8 (h, NVS_KEY_ML_BR,     &u8)  == ESP_OK) s_config.moonlight_brightness   = u8;
    if (nvs_get_u8 (h, NVS_KEY_ML_R,      &u8)  == ESP_OK) s_config.moonlight_r            = u8;
    if (nvs_get_u8 (h, NVS_KEY_ML_G,      &u8)  == ESP_OK) s_config.moonlight_g            = u8;
    if (nvs_get_u8 (h, NVS_KEY_ML_B,      &u8)  == ESP_OK) s_config.moonlight_b            = u8;
    if (nvs_get_u8 (h, NVS_KEY_STORM,     &u8)  == ESP_OK) s_config.storm_intensity        = u8;
    if (nvs_get_u8 (h, NVS_KEY_CLOUD_D,   &u8)  == ESP_OK) s_config.clouds_depth           = u8;
    if (nvs_get_u16(h, NVS_KEY_CLOUD_P,   &u16) == ESP_OK) s_config.clouds_period_s        = u16;

    nvs_close(h);
}

static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u16(h, NVS_KEY_SR_DUR,    s_config.sunrise_duration_min);
    nvs_set_u8 (h, NVS_KEY_SR_MAX_BR, s_config.sunrise_max_brightness);
    nvs_set_u8 (h, NVS_KEY_SS_DUR,    s_config.sunset_duration_min);
    nvs_set_u8 (h, NVS_KEY_ML_BR,     s_config.moonlight_brightness);
    nvs_set_u8 (h, NVS_KEY_ML_R,      s_config.moonlight_r);
    nvs_set_u8 (h, NVS_KEY_ML_G,      s_config.moonlight_g);
    nvs_set_u8 (h, NVS_KEY_ML_B,      s_config.moonlight_b);
    nvs_set_u8 (h, NVS_KEY_STORM,     s_config.storm_intensity);
    nvs_set_u8 (h, NVS_KEY_CLOUD_D,   s_config.clouds_depth);
    nvs_set_u16(h, NVS_KEY_CLOUD_P,   s_config.clouds_period_s);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Color helpers ───────────────────────────────────────────────── */

/**
 * @brief Linear interpolation of an 8-bit value.
 * @param a Start value (t=0).
 * @param b End value   (t=1).
 * @param t Fraction [0.0 – 1.0].
 */
static uint8_t lerp_u8(uint8_t a, uint8_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return (uint8_t)((float)a + ((float)b - (float)a) * t);
}

/**
 * @brief Clamp a float to [lo, hi] and convert to uint8_t.
 */
static uint8_t clamp_u8f(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

/* ── Sunrise color model ─────────────────────────────────────────── */
/*
 * Phase 0 (0–20%):  dark red-orange (embers)     → warm amber
 * Phase 1 (20–60%): warm amber                   → warm white
 * Phase 2 (60–100%): warm white                  → full daylight white
 */
static void sunrise_color(float t, uint8_t max_br,
                           uint8_t *r, uint8_t *g, uint8_t *b,
                           uint8_t *br)
{
    /* Brightness: smooth S-curve from 0 to max_br */
    float br_f = (float)max_br * (t * t * (3.0f - 2.0f * t));  /* smoothstep */
    *br = clamp_u8f(br_f);

    if (t < 0.20f) {
        /* Ember: dark red → orange */
        float s = t / 0.20f;
        *r = lerp_u8(180, 255, s);
        *g = lerp_u8(20,  100, s);
        *b = lerp_u8(0,    10, s);
    } else if (t < 0.60f) {
        /* Amber → warm white */
        float s = (t - 0.20f) / 0.40f;
        *r = lerp_u8(255, 255, s);
        *g = lerp_u8(100, 200, s);
        *b = lerp_u8(10,  160, s);
    } else {
        /* Warm white → daylight */
        float s = (t - 0.60f) / 0.40f;
        *r = lerp_u8(255, 200, s);
        *g = lerp_u8(200, 220, s);
        *b = lerp_u8(160, 255, s);
    }
}

/* ── Sunset color model ──────────────────────────────────────────── */
/* Reverse of sunrise: daylight → warm amber → dark.
 * Intentionally reuses sunrise_max_brightness as the peak brightness;
 * the two values are symmetric – sunrise and sunset reach the same
 * maximum LED intensity, only the time direction is reversed. */
static void sunset_color(float t, uint8_t max_br,
                          uint8_t *r, uint8_t *g, uint8_t *b,
                          uint8_t *br)
{
    /* t=0 = full daylight, t=1 = dark */
    sunrise_color(1.0f - t, max_br, r, g, b, br);
}

/* ── Simple PRNG ─────────────────────────────────────────────────── */

static uint32_t s_rng_state = 0x4a3b1c2d;

static uint32_t fast_rand(void)
{
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return s_rng_state;
}

/* ── Scene task ──────────────────────────────────────────────────── */

static void scene_task(void *arg)
{
    (void)arg;

    /* Per-scene runtime state (stack-allocated, reset on scene start) */
    uint32_t tick       = 0;          /* 500 ms ticks since scene started */
    float    t          = 0.0f;       /* scene progress [0..1]            */
    float    t_step     = 0.0f;       /* progress increment per tick      */
    uint8_t  base_br    = 0;
    float    cloud_phase = 0.0f;

    led_scene_t scene;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    scene = s_active;
    led_scenes_config_t cfg = s_config;
    xSemaphoreGive(s_mutex);

    /* Compute t_step based on scene duration */
    uint32_t duration_ticks = 1;
    switch (scene) {
    case LED_SCENE_SUNRISE:
        duration_ticks = (uint32_t)cfg.sunrise_duration_min * 60u *
                         (1000u / SCENE_TICK_MS);
        if (duration_ticks < 1) duration_ticks = 1;
        t_step = 1.0f / (float)duration_ticks;
        break;
    case LED_SCENE_SUNSET:
        duration_ticks = (uint32_t)cfg.sunset_duration_min * 60u *
                         (1000u / SCENE_TICK_MS);
        if (duration_ticks < 1) duration_ticks = 1;
        t_step = 1.0f / (float)duration_ticks;
        break;
    case LED_SCENE_MOONLIGHT:
        /* Static – just set once then idle */
        led_controller_cancel_fade();
        led_controller_set_color(cfg.moonlight_r,
                                 cfg.moonlight_g,
                                 cfg.moonlight_b);
        led_controller_set_brightness(cfg.moonlight_brightness);
        if (!led_controller_is_on()) led_controller_on();
        ESP_LOGI(TAG, "Moonlight active (br=%d RGB=%d,%d,%d)",
                 cfg.moonlight_brightness,
                 cfg.moonlight_r, cfg.moonlight_g, cfg.moonlight_b);
        break;
    case LED_SCENE_CLOUDS: {
        /* Record current brightness as the base */
        base_br = led_controller_get_brightness();
        if (base_br == 0) base_br = 200;
        /* Cloud period in ticks */
        uint32_t period_ticks = (uint32_t)cfg.clouds_period_s *
                                (1000u / SCENE_TICK_MS);
        if (period_ticks < 1) period_ticks = 1;
        t_step = (2.0f * 3.14159f) / (float)period_ticks;
        break;
    }
    case LED_SCENE_STORM:
        base_br = led_controller_get_brightness();
        if (base_br < 100) base_br = 200;
        break;
    default:
        break;
    }

    ESP_LOGI(TAG, "Scene %d task started", (int)scene);

    while (1) {
        /* Check if scene was stopped */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_scene_t cur = s_active;
        cfg = s_config;  /* always pick up latest config */
        xSemaphoreGive(s_mutex);

        if (cur != scene) {
            ESP_LOGI(TAG, "Scene %d task exiting (new scene %d)",
                     (int)scene, (int)cur);
            break;
        }

        switch (scene) {
        case LED_SCENE_SUNRISE: {
            uint8_t r, g, b, br;
            sunrise_color(t, cfg.sunrise_max_brightness, &r, &g, &b, &br);
            led_controller_cancel_fade();
            led_controller_set_brightness_no_refresh(br);
            led_controller_set_color(r, g, b);
            if (!led_controller_is_on()) led_controller_on();
            t += t_step;
            if (t >= 1.0f) {
                t = 1.0f;
                ESP_LOGI(TAG, "Sunrise complete");
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active = LED_SCENE_NONE;
                xSemaphoreGive(s_mutex);
            }
            break;
        }

        case LED_SCENE_SUNSET: {
            uint8_t r, g, b, br;
            sunset_color(t, cfg.sunrise_max_brightness, &r, &g, &b, &br);
            led_controller_cancel_fade();
            led_controller_set_brightness_no_refresh(br);
            led_controller_set_color(r, g, b);
            if (!led_controller_is_on() && br > 0) led_controller_on();
            t += t_step;
            if (t >= 1.0f) {
                t = 1.0f;
                led_controller_off();
                ESP_LOGI(TAG, "Sunset complete – LEDs off");
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_active = LED_SCENE_NONE;
                xSemaphoreGive(s_mutex);
            }
            break;
        }

        case LED_SCENE_MOONLIGHT:
            /* Static – nothing to update every tick */
            break;

        case LED_SCENE_CLOUDS: {
            /* Slow sinusoidal brightness variation */
            float depth = (float)cfg.clouds_depth / 100.0f;  /* 0..0.8 */
            float amp   = (float)base_br * depth;
            float brf   = (float)base_br - amp * 0.5f +
                           amp * 0.5f * cosf(cloud_phase);
            led_controller_set_brightness(clamp_u8f(brf));
            cloud_phase += t_step;
            if (cloud_phase > 2.0f * 3.14159f) cloud_phase -= 2.0f * 3.14159f;
            break;
        }

        case LED_SCENE_STORM: {
            /* Random flicker around base brightness */
            float depth  = (float)cfg.storm_intensity / 100.0f;
            uint32_t rnd = fast_rand();

            /* Occasional lightning flash (probability ~2%) */
            if ((rnd & 0xff) < 5) {
                led_controller_set_brightness(255);
                vTaskDelay(pdMS_TO_TICKS(60));
                led_controller_set_brightness(clamp_u8f((float)base_br * 0.3f));
                vTaskDelay(pdMS_TO_TICKS(80));
                led_controller_set_brightness(255);
                vTaskDelay(pdMS_TO_TICKS(40));
            }

            /* Background flicker */
            float offset = ((float)(int32_t)(rnd >> 8) / (float)0x7fffff) *
                            depth * (float)base_br;
            float brf    = (float)base_br - fabsf(offset);
            led_controller_set_brightness(clamp_u8f(brf));
            break;
        }

        default:
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(SCENE_TICK_MS));
    }

    /* Nullify task handle so led_scenes_stop() knows we exited */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_task = NULL;
    xSemaphoreGive(s_mutex);

    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t led_scenes_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();
    ESP_LOGI(TAG, "Scene engine initialised");
    return ESP_OK;
}

led_scenes_config_t led_scenes_get_config(void)
{
    led_scenes_config_t cfg;
    if (s_mutex == NULL) {
        memset(&cfg, 0, sizeof(cfg));
        return cfg;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t led_scenes_set_config(const led_scenes_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    led_scenes_config_t safe = *cfg;

    /* Clamp */
    if (safe.sunrise_duration_min < 5)   safe.sunrise_duration_min  = 5;
    if (safe.sunrise_duration_min > 120) safe.sunrise_duration_min  = 120;
    if (safe.sunset_duration_min  < 5)   safe.sunset_duration_min   = 5;
    if (safe.sunset_duration_min  > 120) safe.sunset_duration_min   = 120;
    if (safe.moonlight_brightness > 60)  safe.moonlight_brightness  = 60;
    if (safe.storm_intensity      > 100) safe.storm_intensity        = 100;
    if (safe.clouds_depth         > 80)  safe.clouds_depth           = 80;
    if (safe.clouds_period_s      < 10)  safe.clouds_period_s        = 10;
    if (safe.clouds_period_s      > 600) safe.clouds_period_s        = 600;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;
    xSemaphoreGive(s_mutex);

    return nvs_save_config();
}

esp_err_t led_scenes_start(led_scene_t scene)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    /* Stop any running task first */
    led_scenes_stop();

    if (scene == LED_SCENE_NONE) return ESP_OK;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active = scene;
    xSemaphoreGive(s_mutex);

    BaseType_t ret = xTaskCreate(scene_task, "led_scene",
                                 SCENE_TASK_STACK_SIZE,
                                 NULL, SCENE_TASK_PRIORITY, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scene task");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_active = LED_SCENE_NONE;
        s_task   = NULL;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Scene %d started", (int)scene);
    return ESP_OK;
}

void led_scenes_stop(void)
{
    if (s_mutex == NULL) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active = LED_SCENE_NONE;
    TaskHandle_t t = s_task;
    xSemaphoreGive(s_mutex);

    /* The task checks s_active every tick and exits on its own.
     * Give it up to 2 × SCENE_TICK_MS to finish naturally. */
    if (t != NULL) {
        vTaskDelay(pdMS_TO_TICKS(SCENE_TICK_MS * 2 + 100));
        /* If it still hasn't exited (e.g. stuck in a vTaskDelay), delete it */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_task != NULL) {
            vTaskDelete(s_task);
            s_task = NULL;
        }
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Scene stopped");
}

led_scene_t led_scenes_get_active(void)
{
    if (s_mutex == NULL) return LED_SCENE_NONE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_scene_t a = s_active;
    xSemaphoreGive(s_mutex);
    return a;
}

bool led_scenes_is_running(void)
{
    return led_scenes_get_active() != LED_SCENE_NONE;
}
