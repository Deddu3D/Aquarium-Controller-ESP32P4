/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Temperature History (24-hour ring buffer)
 * Periodically samples the DS18B20 water temperature and stores
 * readings in a fixed-size ring buffer for daily charting.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Single temperature sample with wall-clock timestamp. */
typedef struct {
    time_t timestamp;   /**< UNIX epoch seconds (0 = invalid/unused) */
    float  temp_c;      /**< Temperature in degrees Celsius           */
} temp_sample_t;

/**
 * @brief Maximum number of samples kept in the ring buffer.
 *
 * At the default 5-minute sampling interval this gives exactly
 * 24 hours of history (288 × 5 min = 1440 min = 24 h).
 */
#define TEMP_HISTORY_MAX_SAMPLES 288

/**
 * @brief Initialise the temperature history module.
 *
 * Creates a FreeRTOS task that periodically reads the current water
 * temperature (via temperature_sensor_get()) and appends it to the
 * ring buffer.  Samples older than 24 hours are naturally overwritten
 * as the buffer wraps.
 *
 * @return
 *   - ESP_OK       on success.
 *   - ESP_ERR_NO_MEM if the task or mutex could not be created.
 */
esp_err_t temperature_history_init(void);

/**
 * @brief Copy the current history samples into a caller-supplied buffer.
 *
 * Samples are returned in chronological order (oldest first).
 * Only entries with timestamp > 0 are considered valid.
 *
 * @param[out] out       Destination array (must hold at least
 *                       TEMP_HISTORY_MAX_SAMPLES elements).
 * @param[out] out_count Number of valid samples written.
 */
void temperature_history_get(temp_sample_t *out, int *out_count);

#ifdef __cplusplus
}
#endif
