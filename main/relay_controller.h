/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - 4-Channel Relay Controller
 * Controls four 3 V relays for external equipment such as pumps,
 * heaters, air stones, and filters.  States and custom names are
 * persisted in NVS.  Each relay supports up to RELAY_SCHEDULE_SLOTS
 * independent daily time-of-day schedule slots.
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

/** Number of independent schedule slots per relay */
#define RELAY_SCHEDULE_SLOTS 4

/**
 * @brief Relay schedule entry – single time-of-day on/off window.
 */
typedef struct {
    bool     enabled;   /**< This slot is active                          */
    uint16_t on_min;    /**< Turn-on time  (minutes from midnight, 0–1439) */
    uint16_t off_min;   /**< Turn-off time (minutes from midnight, 0–1439) */
} relay_schedule_t;

/**
 * @brief State of a single relay channel.
 */
typedef struct {
    bool on;                                            /**< true = energised */
    char name[RELAY_NAME_MAX];                          /**< user display name */
    relay_schedule_t schedules[RELAY_SCHEDULE_SLOTS];   /**< time slots */
} relay_state_t;

/**
 * @brief Callback invoked when a relay state changes.
 *
 * @param index  Relay index (0–RELAY_COUNT-1).
 * @param on     New state (true = ON).
 * @param source Either "manual" (direct set) or "schedule".
 */
typedef void (*relay_change_cb_t)(int index, bool on, const char *source);

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
 * @brief Set a single schedule slot for a relay channel.
 *
 * @param index     Relay index (0 – RELAY_COUNT-1).
 * @param slot      Schedule slot (0 – RELAY_SCHEDULE_SLOTS-1).
 * @param schedule  Pointer to the schedule configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t relay_controller_set_schedule(int index, int slot,
                                        const relay_schedule_t *schedule);

/**
 * @brief Set all schedule slots for a relay channel at once.
 *
 * Convenience wrapper that replaces the entire slots array.
 *
 * @param index     Relay index (0 – RELAY_COUNT-1).
 * @param schedules Array of RELAY_SCHEDULE_SLOTS entries.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t relay_controller_set_all_schedules(int index,
                                             const relay_schedule_t schedules[RELAY_SCHEDULE_SLOTS]);

/**
 * @brief Evaluate relay schedules against the current time.
 *
 * Call this periodically (e.g. every 60 s from the main loop) to
 * turn relays on/off according to their configured schedules.
 */
void relay_controller_tick_schedules(void);

/**
 * @brief Register a callback for relay state changes.
 *
 * Only one callback is supported; pass NULL to unregister.
 * The callback is invoked from the calling task context.
 *
 * @param cb  Callback function or NULL.
 */
void relay_controller_set_change_cb(relay_change_cb_t cb);

#ifdef __cplusplus
}
#endif
