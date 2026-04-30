/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Daily Lighting Cycle
 * Automatically simulates a natural full-day lighting cycle using real
 * sunrise / sunset times computed from geographic coordinates:
 *
 *   Night      – LEDs completely off
 *   Sunrise    – LED_SCENE_SUNRISE ramp (warm amber → daylight white)
 *   Morning    – warm daylight white
 *   Noon       – full bright white (around solar noon)
 *   Afternoon  – warm white
 *   Sunset     – LED_SCENE_SUNSET ramp (daylight white → warm amber → off)
 *   Evening    – very dim warm glow (≈60 min twilight after sunset)
 *
 * Sun times are recomputed once per day using the NOAA simplified
 * solar equations (sun_position.h) and the device's current geographic
 * coordinates and system clock.  Configuration is persisted in NVS and
 * is configurable at runtime via the web API (/api/daily_cycle).
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

/* ── Phase identifiers ──────────────────────────────────────────── */

typedef enum {
    DAILY_PHASE_NIGHT     = 0, /**< Before sunrise / after evening: LEDs off   */
    DAILY_PHASE_SUNRISE   = 1, /**< Sunrise colour ramp (LED_SCENE_SUNRISE)    */
    DAILY_PHASE_MORNING   = 2, /**< Post-sunrise warm daylight                 */
    DAILY_PHASE_NOON      = 3, /**< Midday full-brightness white               */
    DAILY_PHASE_AFTERNOON = 4, /**< Pre-sunset warm white                      */
    DAILY_PHASE_SUNSET    = 5, /**< Sunset colour ramp (LED_SCENE_SUNSET)      */
    DAILY_PHASE_EVENING   = 6, /**< Post-sunset dim warm glow                  */
} daily_cycle_phase_t;

/* ── Configuration ──────────────────────────────────────────────── */

/**
 * @brief Daily cycle configuration (NVS-persisted).
 */
typedef struct {
    bool  enabled;    /**< true = module actively controls the LED strip       */
    float latitude;   /**< Geographic latitude  in decimal degrees (N+, S-)   */
    float longitude;  /**< Geographic longitude in decimal degrees (E+, W-)   */
} daily_cycle_config_t;

/* ── Public API ─────────────────────────────────────────────────── */

/**
 * @brief Initialise the daily cycle module and load config from NVS.
 *
 * Must be called after led_controller_init() and led_scenes_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t daily_cycle_init(void);

/**
 * @brief Get a copy of the current daily cycle configuration.
 */
daily_cycle_config_t daily_cycle_get_config(void);

/**
 * @brief Update and persist the daily cycle configuration.
 *
 * If the module is disabled while it was enabled, the LEDs are turned off.
 * Changing latitude / longitude forces a sun-time recalculation on the
 * next tick.
 *
 * @param cfg  Pointer to the new configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t daily_cycle_set_config(const daily_cycle_config_t *cfg);

/**
 * @brief Evaluate the daily cycle once (call from the main loop every ~10 s).
 *
 * Computes today's sunrise / sunset from the configured coordinates and
 * the current system clock, determines the target phase, and applies the
 * appropriate LED state whenever the phase changes.
 * Does nothing if the module is disabled or no valid time is available.
 */
void daily_cycle_tick(void);

/**
 * @brief Return the currently active phase.
 */
daily_cycle_phase_t daily_cycle_get_phase(void);

/**
 * @brief Return today's computed sunrise in minutes from local midnight.
 *
 * Returns -1 if the sun times have not been computed yet (clock not synced
 * or polar day/night condition).
 */
int daily_cycle_get_sunrise_min(void);

/**
 * @brief Return today's computed sunset in minutes from local midnight.
 *
 * Returns -1 if the sun times have not been computed yet.
 */
int daily_cycle_get_sunset_min(void);

#ifdef __cplusplus
}
#endif
