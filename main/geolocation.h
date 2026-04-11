/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Geolocation Configuration
 * Stores latitude, longitude and UTC offset in NVS and provides
 * thread-safe access for the LED scene engine.
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
 * @brief Geolocation configuration.
 */
typedef struct {
    double latitude;         /**< Decimal degrees, north positive  */
    double longitude;        /**< Decimal degrees, east  positive  */
    int    utc_offset_min;   /**< UTC offset in minutes            */
} geolocation_config_t;

/**
 * @brief Initialise the geolocation module.
 *
 * Loads the saved configuration from NVS.  If no configuration has
 * been saved yet, defaults to Rome, Italy (41.9028°N, 12.4964°E, UTC+60).
 *
 * Must be called after nvs_flash_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t geolocation_init(void);

/**
 * @brief Get the current geolocation configuration.
 *
 * Thread-safe (reads are protected by a mutex).
 */
geolocation_config_t geolocation_get(void);

/**
 * @brief Update the geolocation configuration and persist to NVS.
 *
 * @param cfg  New configuration to store.
 * @return ESP_OK on success.
 */
esp_err_t geolocation_set(const geolocation_config_t *cfg);

#ifdef __cplusplus
}
#endif
