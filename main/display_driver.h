/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – MIPI DSI Display Driver
 *
 * Initialises the MIPI DSI bus, selected LCD panel driver,
 * GT911 capacitive touch controller, and LVGL graphics library.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * Display      : 5″ MIPI DSI, 800×480 (driver selected via menuconfig)
 * Touch        : Goodix GT911, I2C
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the MIPI DSI display, touch panel and LVGL.
 *
 * This function performs all hardware initialisation in order:
 *   1. Enable the MIPI DSI PHY internal LDO (2.5 V on LDO_VO3).
 *   2. Create the MIPI DSI bus (2 data lanes, 500 Mbps).
 *   3. Install the selected panel driver (800×480, RGB888).
 *   4. Reset and power-on the panel, enable the backlight.
 *   5. Initialise the I2C bus and GT911 touch controller.
 *   6. Boot LVGL, allocate PSRAM draw buffers, register flush/touch
 *      callbacks, and start the LVGL handler task.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t display_driver_init(void);

/**
 * @brief Acquire the LVGL API lock.
 *
 * LVGL is not thread-safe; call this before any lv_* API call from
 * outside the LVGL task.
 */
void display_lock(void);

/**
 * @brief Release the LVGL API lock.
 */
void display_unlock(void);

/**
 * @brief Get the primary LVGL display handle.
 *
 * @return lv_display_t* or NULL if display_driver_init() has not been
 *         called (or failed).
 */
lv_display_t *display_get_lvgl_display(void);

#ifdef __cplusplus
}
#endif
