/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - WiFi Manager
 * Manages WiFi STA connection via ESP32-C6 coprocessor (SDIO).
 * Falls back to a configuration Access Point when no credentials
 * are stored or the STA connection times out.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum SSID length (including NUL). */
#define WIFI_SSID_MAX      33
/** Maximum password length (including NUL). */
#define WIFI_PASSWORD_MAX  65

/**
 * @brief Initialise the WiFi subsystem.
 *
 * Behaviour:
 *   1. Initialises the TCP/IP stack and default event loop.
 *   2. Loads stored SSID / password from NVS (overrides Kconfig defaults
 *      if a saved credential set exists).
 *   3. Attempts STA connection for up to 30 s.
 *   4. On success returns ESP_OK (STA mode, IP assigned).
 *   5. On timeout or missing credentials, starts a captive-portal AP
 *      (SSID "AquariumSetup", no password) and returns ESP_FAIL.
 *      Connect to the AP and visit http://192.168.4.1 to configure.
 *
 * @return
 *   - ESP_OK   on successful STA connection.
 *   - ESP_FAIL if in AP provisioning mode or STA timed out.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Return true if the STA interface has an IP address.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Return true if the device is currently in AP provisioning mode.
 */
bool wifi_manager_is_ap_mode(void);

/**
 * @brief Copy the current IP address into @p buf as a dotted-decimal string.
 *
 * In AP mode returns the AP gateway IP ("192.168.4.1").
 * If not connected and not in AP mode returns "0.0.0.0".
 *
 * @param buf   Destination buffer (at least 16 bytes recommended).
 * @param len   Size of @p buf.
 */
void wifi_manager_get_ip_str(char *buf, size_t len);

/**
 * @brief Save new WiFi credentials to NVS and restart the STA connection.
 *
 * Called by the provisioning portal after the user submits the form.
 * The device attempts to reconnect in STA mode with the new credentials.
 *
 * @param ssid      NUL-terminated SSID   (max WIFI_SSID_MAX-1 chars).
 * @param password  NUL-terminated password (max WIFI_PASSWORD_MAX-1 chars).
 * @return ESP_OK or error code.
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/**
 * @brief Start the captive-portal configuration HTTP server.
 *
 * Called internally by wifi_manager_init() when AP mode is entered.
 * Can also be called externally to re-open the portal.
 */
esp_err_t wifi_manager_start_portal(void);

#ifdef __cplusplus
}
#endif
