/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Schedule
 * Time-of-day based manual LED control with NVS persistence.
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
 * @brief LED schedule configuration (NVS-backed).
 *
 * When enabled, the LEDs turn on at on_hour:on_minute and
 * off at off_hour:off_minute every day with the specified
 * brightness and colour.
 */
typedef struct {
    bool    enabled;      /**< Schedule enabled                  */
    uint8_t on_hour;      /**< Turn-on hour   (0-23)             */
    uint8_t on_minute;    /**< Turn-on minute  (0-59)             */
    uint8_t off_hour;     /**< Turn-off hour  (0-23)             */
    uint8_t off_minute;   /**< Turn-off minute (0-59)             */
    uint8_t brightness;   /**< LED brightness  (0-255)            */
    uint8_t red;          /**< LED red   component (0-255)        */
    uint8_t green;        /**< LED green component (0-255)        */
    uint8_t blue;         /**< LED blue  component (0-255)        */
} led_schedule_config_t;

/**
 * @brief Initialise the LED schedule module.
 *
 * Loads persisted configuration from NVS and creates a
 * FreeRTOS task that periodically evaluates the schedule
 * and turns the LEDs on/off accordingly.
 *
 * Must be called after @ref led_controller_init and nvs_flash_init.
 *
 * @return ESP_OK on success.
 */
esp_err_t led_schedule_init(void);

/**
 * @brief Return a copy of the current schedule configuration.
 */
led_schedule_config_t led_schedule_get_config(void);

/**
 * @brief Update schedule configuration and persist to NVS.
 *
 * The new configuration is applied immediately on the next
 * schedule tick.
 *
 * @param cfg  New configuration.
 * @return ESP_OK on success.
 */
esp_err_t led_schedule_set_config(const led_schedule_config_t *cfg);

/**
 * @brief Evaluate the schedule once (called from the main loop).
 *
 * Checks the current time against the schedule window and
 * turns the LEDs on or off with fade ramps as appropriate.
 * Safe to call even when the schedule is disabled.
 */
void led_schedule_tick(void);

#ifdef __cplusplus
}
#endif
