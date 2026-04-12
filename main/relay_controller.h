/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - 4-Channel Relay Controller
 * Controls four 3 V relays for external equipment such as pumps,
 * heaters, air stones, and filters.  States and custom names are
 * persisted in NVS.
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

/** Number of relay channels */
#define RELAY_COUNT 4

/** Maximum length of a relay custom name (including NUL) */
#define RELAY_NAME_MAX 32

/**
 * @brief Relay schedule entry – time-of-day on/off automation.
 */
typedef struct {
    bool     enabled;       /**< Schedule active                   */
    uint16_t on_min;        /**< Turn-on time (minutes from midnight, 0-1439) */
    uint16_t off_min;       /**< Turn-off time (minutes from midnight, 0-1439) */
} relay_schedule_t;

/**
 * @brief State of a single relay channel.
 */
typedef struct {
    bool on;                        /**< true = energised / closed   */
    char name[RELAY_NAME_MAX];      /**< user-assigned display name  */
    relay_schedule_t schedule;      /**< time-of-day schedule        */
} relay_state_t;

/**
 * @brief Initialise the relay controller.
 *
 * Configures the four GPIO pins (from Kconfig) as outputs and
 * restores the last-known states and names from NVS.
 *
 * @return
 *   - ESP_OK   on success.
 *   - ESP_FAIL if GPIO or NVS initialisation fails.
 */
esp_err_t relay_controller_init(void);

/**
 * @brief Set a relay on or off.
 *
 * @param index  Relay index (0 – RELAY_COUNT-1).
 * @param on     true to energise, false to de-energise.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t relay_controller_set(int index, bool on);

/**
 * @brief Get the current state of a relay.
 *
 * @param index Relay index (0 – RELAY_COUNT-1).
 * @return true if the relay is energised, false otherwise.
 */
bool relay_controller_get(int index);

/**
 * @brief Set a custom display name for a relay channel.
 *
 * @param index Relay index (0 – RELAY_COUNT-1).
 * @param name  NUL-terminated string (truncated to RELAY_NAME_MAX-1).
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t relay_controller_set_name(int index, const char *name);

/**
 * @brief Get the display name of a relay channel.
 *
 * @param index Relay index (0 – RELAY_COUNT-1).
 * @param[out] name  Buffer to receive the name.
 * @param len   Size of the output buffer.
 */
void relay_controller_get_name(int index, char *name, size_t len);

/**
 * @brief Get the full state snapshot for all relay channels.
 *
 * @param[out] out  Array of RELAY_COUNT elements to fill.
 */
void relay_controller_get_all(relay_state_t out[RELAY_COUNT]);

/**
 * @brief Set the time-of-day schedule for a relay channel.
 *
 * @param index    Relay index (0 – RELAY_COUNT-1).
 * @param schedule Pointer to the schedule configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t relay_controller_set_schedule(int index,
                                        const relay_schedule_t *schedule);

/**
 * @brief Evaluate relay schedules against the current time.
 *
 * Call this periodically (e.g. every 60 s from the main loop) to
 * turn relays on/off according to their configured schedules.
 */
void relay_controller_tick_schedules(void);

#ifdef __cplusplus
}
#endif
