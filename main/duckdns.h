/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - DuckDNS Dynamic DNS Client
 * Periodically updates a DuckDNS domain with the device's public IP,
 * enabling remote access to the web UI from outside the local network.
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

/**
 * @brief DuckDNS configuration stored in NVS.
 */
typedef struct {
    char   domain[64];    /**< DuckDNS sub-domain (e.g. "myaquarium") */
    char   token[48];     /**< DuckDNS API token (UUID format)        */
    bool   enabled;       /**< Whether periodic updates are active    */
} duckdns_config_t;

/**
 * @brief Initialise the DuckDNS module.
 *
 * Loads saved configuration from NVS and, if enabled, starts a
 * background FreeRTOS task that periodically calls the DuckDNS
 * update API.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t duckdns_init(void);

/**
 * @brief Return a copy of the current DuckDNS configuration.
 */
duckdns_config_t duckdns_get_config(void);

/**
 * @brief Apply a new DuckDNS configuration.
 *
 * Saves to NVS and (re)starts or stops the background update task
 * depending on the enabled flag.
 */
esp_err_t duckdns_set_config(const duckdns_config_t *cfg);

/**
 * @brief Trigger a single DuckDNS update right now.
 *
 * Useful for testing the configuration from the web UI.
 *
 * @return ESP_OK if the DuckDNS API responded with "OK", otherwise ESP_FAIL.
 */
esp_err_t duckdns_update_now(void);

/**
 * @brief Copy the last known update status string into a caller buffer.
 *
 * Thread-safe: the copy is made under the module mutex so no race
 * with the background update task that writes the status.
 * Possible values: "OK", "KO", "never", "error: <msg>".
 *
 * @param buf     Destination buffer (must not be NULL).
 * @param buf_len Size of the destination buffer.
 */
void duckdns_get_last_status(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
