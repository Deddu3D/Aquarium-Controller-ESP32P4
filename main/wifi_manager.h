/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - WiFi Manager
 * Manages WiFi STA connection via ESP32-C6 coprocessor (SDIO).
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
 * @brief Maximum number of reconnection attempts before giving up.
 *        Set to 0 for infinite retries.
 */
#define WIFI_MAXIMUM_RETRY 5

/**
 * @brief Initialise the WiFi subsystem in STA mode and attempt to connect.
 *
 * This function:
 *   1. Initialises the TCP/IP stack and default event loop.
 *   2. Creates the default WiFi STA network interface.
 *   3. Configures and starts WiFi using the SSID / password defined
 *      in Kconfig (menuconfig) or the compile-time defaults.
 *   4. Blocks until either a connection is established (IP obtained)
 *      or the maximum retry count is exhausted.
 *
 * @return
 *   - ESP_OK            on successful connection.
 *   - ESP_FAIL          if the connection could not be established.
 *   - ESP_ERR_*         on internal error during initialisation.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Return true if WiFi is currently connected (has an IP address).
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Copy the current IP address into @p buf as a dotted-decimal string.
 *
 * If the station is not connected the buffer is set to "0.0.0.0".
 *
 * @param buf   Destination buffer (at least 16 bytes recommended).
 * @param len   Size of @p buf.
 */
void wifi_manager_get_ip_str(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
