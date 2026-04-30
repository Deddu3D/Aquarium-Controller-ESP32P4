/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Feeding Mode
 * Temporarily pauses the filter/pump relay and optionally dims the LED
 * strip for a configurable number of minutes during feeding.  After the
 * timer expires everything returns to its previous state automatically.
 * A Telegram notification is sent at start and end of the feeding window.
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
 * @brief Feeding mode configuration (NVS-persisted).
 */
typedef struct {
    int     relay_index;      /**< Relay to pause during feeding (-1 = none)  */
    int     duration_min;     /**< Pause duration in minutes (1–60)           */
    bool    dim_lights;       /**< true = reduce LED brightness during feeding */
    uint8_t dim_brightness;   /**< LED brightness while feeding (0–255)       */
} feeding_config_t;

/**
 * @brief Initialise the feeding mode module.
 *
 * Loads configuration from NVS.
 * Must be called after nvs_flash_init(), relay_controller_init(), and
 * led_controller_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t feeding_mode_init(void);

/**
 * @brief Get the current feeding mode configuration.
 */
feeding_config_t feeding_mode_get_config(void);

/**
 * @brief Update and persist the feeding mode configuration.
 *
 * @param cfg  Pointer to the new configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t feeding_mode_set_config(const feeding_config_t *cfg);

/**
 * @brief Start feeding mode.
 *
 * - Turns off (or pauses) the configured relay.
 * - Dims the LED strip if configured.
 * - Starts the countdown timer.
 * - Sends a Telegram notification if the service is configured.
 *
 * Calling this while feeding mode is already active resets the timer.
 *
 * @return ESP_OK on success.
 */
esp_err_t feeding_mode_start(void);

/**
 * @brief Stop feeding mode early (manual cancel).
 *
 * Restores the relay and LED strip state and sends a Telegram notification.
 */
void feeding_mode_stop(void);

/**
 * @brief Return true if feeding mode is currently active.
 */
bool feeding_mode_is_active(void);

/**
 * @brief Return the remaining feeding time in seconds (0 if not active).
 */
int feeding_mode_get_remaining_s(void);

/**
 * @brief Evaluate feeding mode timer.
 *
 * Call periodically from the main loop (every ~10 s).
 * Automatically deactivates feeding mode when the timer expires and
 * restores the previous relay and LED state.
 */
void feeding_mode_tick(void);

#ifdef __cplusplus
}
#endif
