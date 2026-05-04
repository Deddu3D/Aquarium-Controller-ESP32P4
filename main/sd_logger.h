/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - SD Data Logger
 * Writes time-stamped records to per-day CSV files on the SD card.
 * All functions are no-ops when the SD card is not mounted.
 *
 * File layout:
 *   /sdcard/logs/temp_YYYYMMDD.csv    – temperature samples
 *   /sdcard/logs/events_YYYYMMDD.csv  – relay, feeding, CO2 events
 *   /sdcard/logs/telegram_YYYYMMDD.log – Telegram notification history
 *   /sdcard/logs/diag_YYYYMMDD.log    – diagnostic messages (WARN+)
 *
 * CSV column formats:
 *   temp:    timestamp_iso, temperature_c
 *   events:  timestamp_iso, type, detail
 *   telegram:timestamp_iso, message
 *   diag:    timestamp_iso, level, tag, message
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the SD logger module.
 *
 * Must be called after sd_card_init().  No-op (returns ESP_OK) if the
 * SD card is not mounted; logging functions will silently skip writes
 * until a card is available.
 *
 * @return ESP_OK always (non-fatal).
 */
esp_err_t sd_logger_init(void);

/**
 * @brief Log a temperature sample to the daily temperature CSV.
 *
 * Appends a line:  iso_timestamp,temperature_c
 *
 * @param ts      UNIX epoch timestamp of the reading.
 * @param temp_c  Temperature in degrees Celsius.
 */
void sd_logger_log_temperature(time_t ts, float temp_c);

/**
 * @brief Log a generic aquarium event to the daily events CSV.
 *
 * Appends a line:  iso_timestamp,type,detail
 *
 * Suggested @p type values:
 *   "relay_on", "relay_off", "feeding_start", "feeding_stop",
 *   "co2_open", "co2_close", "heater_on", "heater_off",
 *   "scene_start", "scene_stop", "daily_cycle_phase"
 *
 * @param ts     UNIX epoch timestamp of the event.
 * @param type   Short event category string (no commas).
 * @param detail Free-text detail string (no commas; may be empty or NULL).
 */
void sd_logger_log_event(time_t ts, const char *type, const char *detail);

/**
 * @brief Log an outgoing Telegram notification to the daily Telegram log.
 *
 * Appends a line:  iso_timestamp,message
 *
 * @param ts      UNIX epoch timestamp.
 * @param message Notification text (newlines replaced by space).
 */
void sd_logger_log_telegram(time_t ts, const char *message);

/**
 * @brief Log a diagnostic message to the daily diagnostic log.
 *
 * Intended to capture ESP_LOGW / ESP_LOGE level messages for
 * post-mortem analysis.
 *
 * @param ts      UNIX epoch timestamp.
 * @param level   "W" or "E" (warning / error).
 * @param tag     Module tag string.
 * @param message Message body.
 */
void sd_logger_log_diagnostic(time_t ts, const char *level,
                               const char *tag, const char *message);

#ifdef __cplusplus
}
#endif
