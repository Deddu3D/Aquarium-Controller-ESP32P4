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

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Available LED scenes for aquarium lighting.
 */
typedef enum {
    LED_SCENE_OFF = 0,          /**< No scene – manual LED control          */
    LED_SCENE_DAYLIGHT,         /**< Static daylight (colour-temp based)    */
    LED_SCENE_SUNRISE,          /**< Configurable-duration sunrise          */
    LED_SCENE_SUNSET,           /**< Configurable-duration sunset           */
    LED_SCENE_MOONLIGHT,        /**< Lunar-phase dim blue moonlight         */
    LED_SCENE_CLOUDY,           /**< Slow sinusoidal brightness waves       */
    LED_SCENE_STORM,            /**< Dark base with random lightning flashes */
    LED_SCENE_FULL_DAY_CYCLE,   /**< Automatic 24 h cycle (real-time + geolocation) */
    LED_SCENE_MAX               /**< Sentinel – not a valid scene           */
} led_scene_t;

/**
 * @brief Persistent scene-engine configuration (NVS-backed).
 */
typedef struct {
    uint16_t sunrise_duration_min;    /**< Standalone sunrise (minutes, 1–120) */
    uint16_t sunset_duration_min;     /**< Standalone sunset  (minutes, 1–120) */
    uint16_t transition_duration_min; /**< Full Day Cycle transition (minutes)  */
    bool     siesta_enabled;          /**< Enable midday siesta in Full Day    */
    uint16_t siesta_start_min;        /**< Siesta start (minutes from midnight)*/
    uint16_t siesta_end_min;          /**< Siesta end   (minutes from midnight)*/
    uint8_t  siesta_intensity_pct;    /**< Siesta dimming level 0-100 %        */
    uint16_t color_temp_kelvin;       /**< Daylight colour temperature (K)     */
    bool     lunar_moonlight;         /**< Modulate moonlight by moon phase    */
    uint8_t  fullday_max_brightness_pct; /**< Full Day Cycle max brightness 1-100% */
} led_scene_config_t;

/**
 * @brief Initialise the LED scene engine.
 *
 * Loads persisted configuration and last-active scene from NVS,
 * creates a FreeRTOS task that drives scene animations in the
 * background, and a mutex for thread-safe scene switching.
 * Must be called after @ref led_controller_init and nvs_flash_init.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on resource failure.
 */
esp_err_t led_scenes_init(void);

/**
 * @brief Activate a scene (persisted to NVS).
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

/**
 * @brief Return a copy of the current scene configuration.
 */
led_scene_config_t led_scenes_get_config(void);

/**
 * @brief Update scene configuration and persist to NVS.
 *
 * @param cfg  New configuration (values are clamped to valid ranges).
 * @return ESP_OK on success.
 */
esp_err_t led_scenes_set_config(const led_scene_config_t *cfg);

#ifdef __cplusplus
}
#endif
