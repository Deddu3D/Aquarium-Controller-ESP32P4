/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Event Log
 * Thread-safe in-RAM ring buffer of the last EVENT_LOG_MAX events.
 * Events are timestamped and categorised; exposed via /api/events.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of events kept in the ring buffer. */
#define EVENT_LOG_MAX 64

/** Maximum length of an event message (including NUL). */
#define EVENT_MSG_MAX 96

/** Event categories. */
typedef enum {
    EVT_RELAY_CHANGE   = 0,  /**< Relay turned ON or OFF                */
    EVT_TEMP_ALARM     = 1,  /**< Temperature out of safe range         */
    EVT_SENSOR_FAULT   = 2,  /**< DS18B20 read failures                 */
    EVT_FEEDING        = 3,  /**< Feeding mode started / stopped        */
    EVT_OTA            = 4,  /**< OTA update started / completed        */
    EVT_HEATER_RUNAWAY = 5,  /**< Heater runaway protection triggered   */
    EVT_SYSTEM         = 6,  /**< Generic system event (boot, reset…)   */
    EVT_CO2            = 7,  /**< CO2 solenoid valve event              */
} event_type_t;

/** Single event record. */
typedef struct {
    time_t       timestamp;            /**< UNIX epoch (0 = slot empty)   */
    event_type_t type;                 /**< Event category                */
    char         message[EVENT_MSG_MAX]; /**< Human-readable description  */
} event_entry_t;

/**
 * @brief Initialise the event log module.
 *
 * Creates the internal mutex.  Must be called once before any other
 * event_log_* function.
 *
 * @return ESP_OK on success.
 */
esp_err_t event_log_init(void);

/**
 * @brief Append a new event to the ring buffer.
 *
 * Thread-safe.  If the buffer is full the oldest entry is silently
 * overwritten.  The timestamp is set to time(NULL).
 *
 * @param type     Event category.
 * @param message  NUL-terminated message string (truncated to EVENT_MSG_MAX-1).
 */
void event_log_add(event_type_t type, const char *message);

/**
 * @brief Copy events from the ring buffer in chronological order.
 *
 * Thread-safe.
 *
 * @param[out] out       Caller-allocated array of at least EVENT_LOG_MAX elements.
 * @param[out] out_count Number of valid entries written.
 */
void event_log_get_all(event_entry_t *out, int *out_count);

#ifdef __cplusplus
}
#endif
