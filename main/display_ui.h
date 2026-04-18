/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Touch Display UI
 * LVGL v9 dashboard for the Waveshare 4-DSI-TOUCH-A
 * (720 × 720 round IPS, HX8394 panel driver, GT911 touch controller).
 *
 * Tabs:
 *  0 – Home   : temperature, LED quick control, relay toggle buttons
 *  1 – LED    : manual control, schedule, presets
 *  2 – Relè   : 4-channel relay names + manual on/off
 *  3 – Config : auto-heater, CO2 controller, timezone
 *  4 – Info   : WiFi status, IP, heap, uptime, date/time
 *
 * Excluded intentionally (web-UI-only): Telegram, OTA, DuckDNS.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 * LVGL         : v9.x
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the display hardware and build the LVGL touch UI.
 *
 * Initialises MIPI-DSI bus, HX8394 panel, GT911 touch controller, and LVGL,
 * then builds the five-tab dashboard and starts the LVGL timer-handler task.
 *
 * Must be called after all aquarium controller modules are initialised so
 * that the UI can immediately display live values.
 *
 * @return
 *   - ESP_OK               on success.
 *   - ESP_ERR_NOT_SUPPORTED if `CONFIG_DISPLAY_ENABLED` is disabled in Kconfig.
 *   - ESP_FAIL             if display or touch hardware initialisation fails; the firmware
 *                          continues without display in that case.
 */
esp_err_t display_ui_init(void);

#ifdef __cplusplus
}
#endif
