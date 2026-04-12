/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – LVGL Dashboard UI
 *
 * Builds the on-screen aquarium status dashboard shown on the
 * MIPI DSI touch display.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the aquarium dashboard on the LVGL display.
 *
 * Must be called after display_driver_init().  All LVGL objects are
 * created inside the display_lock() / display_unlock() section.
 *
 * @return ESP_OK on success.
 */
esp_err_t display_ui_init(void);

/**
 * @brief Periodic refresh of dynamic UI elements.
 *
 * Call from the main loop (e.g. every 10 s) to update temperature,
 * relay states, LED scene name, WiFi status and clock.
 */
void display_ui_refresh(void);

#ifdef __cplusplus
}
#endif
