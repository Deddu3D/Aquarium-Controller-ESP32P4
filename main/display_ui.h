/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Touch Display UI
 * LVGL v9 dashboard for the Waveshare 4-DSI-TOUCH-A
 * (720 × 720 round IPS, HX8394 panel driver, GT911 touch controller).
 *
 * Tabs:
 *  0 – Home        : 2×2 card grid (Temperatura, Luci, CO₂, Livello Acqua)
 *  1 – Luci        : brightness slider, scene buttons (Alba/Giorno/Tramonto/Notte)
 *  2 – Temperatura : arc gauge, target ±, heater/cooling status
 *  3 – Automazioni : scrollable list with toggles (Luci, CO₂, Riscaldatore, Relè)
 *  4 – Dati        : LVGL line/bar chart with Temperatura/Luci/CO₂ selector
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

/**
 * @brief Show a modal alarm overlay on top of the dashboard.
 *
 * Thread-safe: may be called from any FreeRTOS task.
 * Displays a red modal popup with a warning icon, the given title message,
 * an optional detail line, and two action buttons:
 *  – "DISATTIVA ALLARME" (dismiss/hide)
 *  – "OK" (acknowledge and clear)
 *
 * @param msg    Main alarm message (e.g. "TEMPERATURA TROPPO ALTA!"). Must not be NULL.
 * @param detail Optional detail line (e.g. "Attuale: 28.7°C"). Pass NULL or "" to omit.
 */
void display_ui_show_alarm(const char *msg, const char *detail);

#ifdef __cplusplus
}
#endif
