/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Timezone Manager
 * Stores and applies a POSIX TZ string via NVS so the system
 * timezone is configurable without recompiling the firmware.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of a POSIX TZ string including NUL. */
#define TZ_STRING_MAX 64

/**
 * @brief Initialise the timezone manager.
 *
 * Loads the persisted POSIX TZ string from NVS and applies it via
 * setenv("TZ", …) + tzset().  Falls back to the default TZ defined
 * at compile time (CONFIG_AQUARIUM_DEFAULT_TZ) if nothing is stored.
 *
 * @return ESP_OK on success.
 */
esp_err_t timezone_manager_init(void);

/**
 * @brief Get the currently active POSIX TZ string.
 *
 * @param[out] buf   Destination buffer.
 * @param       len   Size of @p buf (at least TZ_STRING_MAX recommended).
 */
void timezone_manager_get(char *buf, size_t len);

/**
 * @brief Set a new POSIX TZ string, apply it, and persist to NVS.
 *
 * @param tz  NUL-terminated POSIX TZ string (e.g. "CET-1CEST,M3.5.0/2,M10.5.0/3").
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t timezone_manager_set(const char *tz);

#ifdef __cplusplus
}
#endif
