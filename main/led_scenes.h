/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine
 * Automatic LED scenes and animations for aquarium lighting.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Available LED scenes for aquarium lighting.
 */
typedef enum {
    LED_SCENE_OFF = 0,          /**< No scene – manual LED control          */
    LED_SCENE_DAYLIGHT,         /**< Static cool-white daylight             */
    LED_SCENE_SUNRISE,          /**< 5-min sunrise animation                */
    LED_SCENE_SUNSET,           /**< 5-min sunset animation                 */
    LED_SCENE_MOONLIGHT,        /**< Static dim blue moonlight              */
    LED_SCENE_CLOUDY,           /**< Slow sinusoidal brightness waves       */
    LED_SCENE_STORM,            /**< Dark base with random lightning flashes */
    LED_SCENE_FULL_DAY_CYCLE,   /**< Automatic 24 h cycle (real-time + geolocation) */
    LED_SCENE_MAX               /**< Sentinel – not a valid scene           */
} led_scene_t;

/**
 * @brief Initialise the LED scene engine.
 *
 * Creates a FreeRTOS task that drives scene animations in the
 * background and a mutex for thread-safe scene switching.
 * Must be called after @ref led_controller_init.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on resource failure.
 */
esp_err_t led_scenes_init(void);

/**
 * @brief Activate a scene.
 *
 * The scene task picks up the change on its next tick.
 * Setting @ref LED_SCENE_OFF returns to manual LED control.
 *
 * @param scene  The scene to activate.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if scene is invalid.
 */
esp_err_t led_scenes_set(led_scene_t scene);

/**
 * @brief Return the currently active scene.
 */
led_scene_t led_scenes_get(void);

/**
 * @brief Return the human-readable name of a scene.
 */
const char *led_scenes_get_name(led_scene_t scene);

/**
 * @brief Look up a scene by its name string.
 *
 * @return The matching scene, or LED_SCENE_OFF if not found.
 */
led_scene_t led_scenes_from_name(const char *name);

/**
 * @brief Convenience alias for led_scenes_set(LED_SCENE_OFF).
 */
void led_scenes_stop(void);

#ifdef __cplusplus
}
#endif
