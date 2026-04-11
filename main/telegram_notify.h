/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Telegram Notification Service
 * Sends notifications via Telegram Bot API for temperature alarms,
 * maintenance reminders and daily summaries.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Telegram notification configuration.
 */
typedef struct {
    char  bot_token[128];        /**< Telegram Bot API token                   */
    char  chat_id[64];           /**< Telegram chat ID to send messages to     */
    bool  enabled;               /**< Master enable for all notifications      */

    /* Temperature alarms */
    bool  temp_alarm_enabled;    /**< Enable temperature threshold alarms      */
    float temp_high_c;           /**< High temperature alarm threshold (°C)    */
    float temp_low_c;            /**< Low temperature alarm threshold (°C)     */

    /* Maintenance reminders */
    bool  water_change_enabled;  /**< Enable water-change reminders            */
    int   water_change_days;     /**< Water-change interval (days)             */
    bool  fertilizer_enabled;    /**< Enable fertilizer reminders              */
    int   fertilizer_days;       /**< Fertilizer interval (days)               */

    /* Daily summary */
    bool  daily_summary_enabled; /**< Enable daily summary notification        */
    int   daily_summary_hour;    /**< Hour (0-23) to send the daily summary    */
} telegram_config_t;

/**
 * @brief Initialise the Telegram notification module.
 *
 * Loads configuration from NVS and starts the background task
 * that monitors alarms and sends reminders.
 * Must be called after nvs_flash_init() and temperature_sensor_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t telegram_notify_init(void);

/**
 * @brief Get the current Telegram configuration.
 *
 * Thread-safe.
 */
telegram_config_t telegram_notify_get_config(void);

/**
 * @brief Update the Telegram configuration and persist to NVS.
 *
 * @param cfg  New configuration.
 * @return ESP_OK on success.
 */
esp_err_t telegram_notify_set_config(const telegram_config_t *cfg);

/**
 * @brief Send a free-text message via the configured Telegram bot.
 *
 * @param message  UTF-8 text to send (Telegram HTML parse mode).
 * @return ESP_OK on success, or an error if sending failed.
 */
esp_err_t telegram_notify_send(const char *message);

/**
 * @brief Record that a water change was performed (resets the timer).
 */
esp_err_t telegram_notify_reset_water_change(void);

/**
 * @brief Record that fertilizer was dosed (resets the timer).
 */
esp_err_t telegram_notify_reset_fertilizer(void);

/**
 * @brief Get the Unix timestamp of the last water change.
 *        Returns 0 if never recorded.
 */
int64_t telegram_notify_get_last_water_change(void);

/**
 * @brief Get the Unix timestamp of the last fertilizer dose.
 *        Returns 0 if never recorded.
 */
int64_t telegram_notify_get_last_fertilizer(void);

#ifdef __cplusplus
}
#endif
