/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine
 * Dynamic lighting effects for a natural aquarium experience:
 *   SUNRISE   – warm amber/orange dawn ramp transitioning to daylight white
 *   SUNSET    – daylight white fading to warm amber/orange, then off
 *   MOONLIGHT – dim blue night light for nocturnal fish
 *   STORM     – random brightness flicker with occasional lightning
 *   CLOUDS    – slow sinusoidal brightness variation simulating overcast sky
 *
 * The scene engine runs a lightweight FreeRTOS task that wakes every 500 ms
 * to update the LED strip.  Only one scene can be active at a time.
 * Starting a new scene automatically stops the previous one.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Scene identifiers ──────────────────────────────────────────── */

typedef enum {
    LED_SCENE_NONE      = 0,  /**< No scene active – manual control      */
    LED_SCENE_SUNRISE   = 1,  /**< Dawn simulation (cold → warm → white) */
    LED_SCENE_SUNSET    = 2,  /**< Dusk simulation (white → warm → off)  */
    LED_SCENE_MOONLIGHT = 3,  /**< Dim blue/white moonlight              */
    LED_SCENE_STORM     = 4,  /**< Flickering storm effect               */
    LED_SCENE_CLOUDS    = 5,  /**< Slow sinusoidal brightness variation  */
} led_scene_t;

/* ── Scene configuration ────────────────────────────────────────── */

/**
 * @brief Per-scene tuning parameters.  All values are validated and
 *        clamped in led_scenes_set_config().
 */
typedef struct {
    uint16_t sunrise_duration_min;  /**< Total sunrise ramp (5–120 min)   */
    uint8_t  sunrise_max_brightness;/**< Peak brightness after dawn (0-255)*/
    uint8_t  sunset_duration_min;   /**< Total sunset ramp (5–120 min)    */
    uint8_t  moonlight_brightness;  /**< Moonlight brightness (0–60)      */
    uint8_t  moonlight_r;           /**< Moonlight red   component        */
    uint8_t  moonlight_g;           /**< Moonlight green component        */
    uint8_t  moonlight_b;           /**< Moonlight blue  component        */
    uint8_t  storm_intensity;       /**< Storm flicker depth (0–100 %)    */
    uint8_t  clouds_depth;          /**< Cloud dimming depth  (0–80 %)    */
    uint16_t clouds_period_s;       /**< Cloud cycle period (10–600 s)    */
} led_scenes_config_t;

/* ── Public API ─────────────────────────────────────────────────── */

/**
 * @brief Initialise the scene engine and load configuration from NVS.
 *
 * Must be called after led_controller_init().
 * Does not start any scene; call led_scenes_start() for that.
 *
 * @return ESP_OK on success.
 */
esp_err_t led_scenes_init(void);

/**
 * @brief Get the current scene configuration.
 */
led_scenes_config_t led_scenes_get_config(void);

/**
 * @brief Update scene configuration and persist to NVS.
 *
 * @param cfg  Pointer to the new configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t led_scenes_set_config(const led_scenes_config_t *cfg);

/**
 * @brief Start (or switch to) a scene.
 *
 * Stops the currently running scene (if any), then begins the new one.
 * Passing LED_SCENE_NONE is equivalent to led_scenes_stop().
 *
 * @param scene  Scene to activate.
 * @return ESP_OK on success.
 */
esp_err_t led_scenes_start(led_scene_t scene);

/**
 * @brief Stop the active scene and return LED control to the caller.
 *
 * The LEDs are left in whatever state the scene last set them.
 * Call led_controller_off() afterwards if a dark state is desired.
 */
void led_scenes_stop(void);

/**
 * @brief Return the currently active scene (LED_SCENE_NONE if stopped).
 */
led_scene_t led_scenes_get_active(void);

/**
 * @brief Return true if a scene is currently running.
 */
bool led_scenes_is_running(void);

#ifdef __cplusplus
}
#endif
