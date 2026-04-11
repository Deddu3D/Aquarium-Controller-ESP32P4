/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Strip Controller
 * Drives a WS2812B addressable LED strip for aquarium lighting.
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
 * @brief Initialise the WS2812B LED strip via the RMT peripheral.
 *
 * Configures the LED strip using the GPIO pin and LED count defined
 * in Kconfig (menuconfig → Aquarium LED Strip Settings).
 * After initialisation the strip is turned off (all LEDs black).
 *
 * @return
 *   - ESP_OK   on success.
 *   - ESP_FAIL or other error codes on failure.
 */
esp_err_t led_controller_init(void);

/**
 * @brief Turn all LEDs on with the current colour and brightness.
 */
esp_err_t led_controller_on(void);

/**
 * @brief Turn all LEDs off (black).
 */
esp_err_t led_controller_off(void);

/**
 * @brief Set the global brightness level.
 *
 * The brightness value scales the RGB output of every LED.
 *
 * @param brightness  0 (off) – 255 (full brightness).
 * @return ESP_OK on success.
 */
esp_err_t led_controller_set_brightness(uint8_t brightness);

/**
 * @brief Set a uniform colour for the entire strip.
 *
 * The colour is scaled by the current brightness before being
 * written to the strip.
 *
 * @param red    Red   component (0-255).
 * @param green  Green component (0-255).
 * @param blue   Blue  component (0-255).
 * @return ESP_OK on success.
 */
esp_err_t led_controller_set_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Set the colour of a single LED.
 *
 * The colour is scaled by the current brightness. Changes are not
 * visible until @ref led_controller_refresh is called.
 *
 * @param index  LED index (0-based, must be < LED count).
 * @param red    Red   component (0-255).
 * @param green  Green component (0-255).
 * @param blue   Blue  component (0-255).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index is out of range.
 */
esp_err_t led_controller_set_pixel(uint16_t index, uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Flush the pixel buffer to the LED strip.
 *
 * Call this after one or more @ref led_controller_set_pixel calls to
 * make the changes visible.
 *
 * @return ESP_OK on success.
 */
esp_err_t led_controller_refresh(void);

/**
 * @brief Return the current brightness level.
 */
uint8_t led_controller_get_brightness(void);

/**
 * @brief Return the current base colour (before brightness scaling).
 *
 * @param[out] red   Pointer to receive the red   component.
 * @param[out] green Pointer to receive the green component.
 * @param[out] blue  Pointer to receive the blue  component.
 */
void led_controller_get_color(uint8_t *red, uint8_t *green, uint8_t *blue);

/**
 * @brief Return true if the strip is currently turned on.
 */
bool led_controller_is_on(void);

/**
 * @brief Return the number of LEDs in the strip.
 */
uint16_t led_controller_get_num_leds(void);

#ifdef __cplusplus
}
#endif
