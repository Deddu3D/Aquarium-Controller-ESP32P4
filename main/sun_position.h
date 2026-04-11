/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Sun Position Calculator
 * Computes sunrise / sunset times from geographic coordinates and date
 * using the simplified NOAA solar equations.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a sunrise/sunset calculation.
 *
 * Times are expressed as **minutes from local midnight** (0 – 1439).
 * If the sun never rises or never sets (polar day / night),
 * the @c valid flag is false.
 */
typedef struct {
    bool valid;             /**< true if sunrise/sunset could be computed */
    int  sunrise_min;       /**< Sunrise in minutes from local midnight  */
    int  sunset_min;        /**< Sunset  in minutes from local midnight  */
} sun_times_t;

/**
 * @brief Compute sunrise and sunset for a given location and date.
 *
 * Uses the simplified NOAA solar equations (good to ±1 min at
 * mid-latitudes).
 *
 * @param latitude_deg   Latitude  in decimal degrees (north positive).
 * @param longitude_deg  Longitude in decimal degrees (east positive).
 * @param utc_offset_min UTC offset in minutes (e.g. +60 for CET).
 * @param year           Calendar year   (e.g. 2026).
 * @param month          Calendar month  (1 – 12).
 * @param day            Calendar day    (1 – 31).
 * @return A @ref sun_times_t with the computed times.
 */
sun_times_t sun_position_calc(double latitude_deg,
                              double longitude_deg,
                              int    utc_offset_min,
                              int    year,
                              int    month,
                              int    day);

#ifdef __cplusplus
}
#endif
