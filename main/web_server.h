/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Web Status Server
 * Provides a simple HTTP status page to verify WiFi connectivity.
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
 * @brief Start the HTTP status server on port 80.
 *
 * Serves a single-page dashboard at "/" showing:
 *   - WiFi connection status
 *   - IP address, SSID, RSSI
 *   - Free heap memory and uptime
 *
 * Also exposes a JSON endpoint at "/api/status" for programmatic access.
 *
 * @return
 *   - ESP_OK   on success.
 *   - ESP_FAIL if the server could not be started.
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the HTTP status server.
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif
