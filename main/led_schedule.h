/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Schedule
 * Time-of-day based manual LED control with NVS persistence.
 * Supports colour ramp at turn-on, optional midday pause, and
 * up to LED_PRESET_COUNT named presets stored in NVS.
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

/** Maximum number of named presets that can be stored in NVS. */
#define LED_PRESET_COUNT      5
/** 16 usable characters + NUL terminator.  NVS string keys are max 15 chars,
 *  so preset names are stored under separate "p<N>_n" keys. */
#define LED_PRESET_NAME_LEN   17

/**
 * @brief LED schedule configuration (NVS-backed).
 *
 * The daily lighting cycle is:
 *   - OFF before on_hour:on_minute
 *   - Colour ramp from black to the configured colour/brightness
 *     over ramp_duration_min minutes (0 = instant)
 *   - Full brightness from ramp end to pause_start (if pause_enabled)
 *   - Reduced brightness/colour during pause window (if pause_enabled)
 *   - Full brightness from pause_end to off_hour:off_minute
 *   - OFF from off_hour:off_minute onward
 */
typedef struct {
    bool     enabled;              /**< Schedule enabled                         */
    uint8_t  on_hour;              /**< Turn-on hour          (0-23)             */
    uint8_t  on_minute;            /**< Turn-on minute        (0-59)             */
    uint16_t ramp_duration_min;    /**< Colour-ramp duration  (0-120 min)        */
    bool     pause_enabled;        /**< Midday pause enabled                     */
    uint8_t  pause_start_hour;     /**< Pause start hour      (0-23)             */
    uint8_t  pause_start_minute;   /**< Pause start minute    (0-59)             */
    uint8_t  pause_end_hour;       /**< Pause end hour        (0-23)             */
    uint8_t  pause_end_minute;     /**< Pause end minute      (0-59)             */
    uint8_t  pause_brightness;     /**< Brightness during pause (0-255)          */
    uint8_t  pause_red;            /**< Red   component during pause (0-255)     */
    uint8_t  pause_green;          /**< Green component during pause (0-255)     */
    uint8_t  pause_blue;           /**< Blue  component during pause (0-255)     */
    uint8_t  off_hour;             /**< Turn-off hour         (0-23)             */
    uint8_t  off_minute;           /**< Turn-off minute       (0-59)             */
    uint8_t  brightness;           /**< Daytime brightness    (0-255)            */
    uint8_t  red;                  /**< Daytime red   component (0-255)          */
    uint8_t  green;                /**< Daytime green component (0-255)          */
    uint8_t  blue;                 /**< Daytime blue  component (0-255)          */
} led_schedule_config_t;

/**
 * @brief A named preset combining a label and a full schedule config.
 */
typedef struct {
    char                  name[LED_PRESET_NAME_LEN]; /**< Human-readable name   */
    led_schedule_config_t config;                    /**< Associated schedule   */
} led_preset_t;

/**
 * @brief Initialise the LED schedule module.
 *
 * Loads persisted configuration and presets from NVS.
 * Must be called after led_controller_init() and nvs_flash_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t led_schedule_init(void);

/**
 * @brief Return a copy of the current schedule configuration.
 */
led_schedule_config_t led_schedule_get_config(void);

/**
 * @brief Update schedule configuration and persist to NVS.
 *
 * @param cfg  New configuration (values are clamped to valid ranges).
 * @return ESP_OK on success.
 */
esp_err_t led_schedule_set_config(const led_schedule_config_t *cfg);

/**
 * @brief Evaluate the schedule once (called from the main loop every ~10 s).
 *
 * Computes the current phase (OFF / ON / PAUSE) from the wall clock and
 * applies LED transitions only on phase changes.  Safe to call when the
 * schedule is disabled.
 */
void led_schedule_tick(void);

/* ── Preset API ──────────────────────────────────────────────────── */

/**
 * @brief Read a preset from the in-memory cache.
 *
 * @param slot  Preset index (0 .. LED_PRESET_COUNT-1).
 * @param out   Filled with the preset data on success.
 * @return true if @p slot is valid, false otherwise.
 */
bool led_preset_get(int slot, led_preset_t *out);

/**
 * @brief Save a named preset to NVS and the in-memory cache.
 *
 * @param slot  Preset index (0 .. LED_PRESET_COUNT-1).
 * @param name  Human-readable name (up to LED_PRESET_NAME_LEN-1 chars).
 * @param cfg   Schedule configuration to store.
 * @return ESP_OK on success.
 */
esp_err_t led_preset_save(int slot, const char *name,
                           const led_schedule_config_t *cfg);

/**
 * @brief Load a preset and apply it as the active schedule.
 *
 * Equivalent to led_schedule_set_config(&preset[slot].config).
 *
 * @param slot  Preset index (0 .. LED_PRESET_COUNT-1).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot is out of range.
 */
esp_err_t led_preset_load(int slot);

#ifdef __cplusplus
}
#endif
