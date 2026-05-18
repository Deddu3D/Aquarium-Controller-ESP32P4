/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Zero-Config MQTT Remote Relay
 *
 * Establishes an outbound TLS MQTT connection to a managed public broker
 * (broker.hivemq.com:8883) so the controller is reachable from anywhere
 * without port-forwarding, DuckDNS, or any user account.
 *
 * Device identity is derived automatically from the MAC address (12 hex
 * chars, e.g. "aabbccddeeff").  The Android app learns this ID during
 * the provisioning wizard and uses it to find the device's topics.
 *
 * Topics:
 *   aquarium/{device_id}/status   – ESP publishes (periodic + on-demand)
 *   aquarium/{device_id}/cmd      – ESP subscribes (JSON commands)
 *   aquarium/{device_id}/response – ESP publishes command responses
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Length of the device-ID string (12 hex chars + NUL). */
#define REMOTE_RELAY_DEVICE_ID_LEN 13

/**
 * @brief Initialise the MQTT remote relay module.
 *
 * Derives the device_id from the base MAC address, creates the MQTT
 * client, and (if enabled in Kconfig) starts the background connection.
 * Safe to call before WiFi is up; the client will auto-reconnect once
 * the STA link is established.
 *
 * @return ESP_OK on success.
 */
esp_err_t remote_relay_init(void);

/**
 * @brief Copy the 12-char hex device-ID into @p buf.
 *
 * The device-ID is derived from the ESP base MAC address and is
 * therefore unique to each board.
 *
 * @param buf     Destination buffer (must be at least REMOTE_RELAY_DEVICE_ID_LEN bytes).
 * @param len     Size of @p buf.
 */
void remote_relay_get_device_id(char *buf, size_t len);

/**
 * @brief Return true if the MQTT broker connection is currently active.
 */
bool remote_relay_is_connected(void);

/**
 * @brief Publish a fresh status JSON payload to the status topic.
 *
 * Called by the main loop after any state change so that connected
 * Android clients see near-real-time updates without waiting for the
 * 30-second periodic publish.
 */
void remote_relay_publish_status(void);

#ifdef __cplusplus
}
#endif
