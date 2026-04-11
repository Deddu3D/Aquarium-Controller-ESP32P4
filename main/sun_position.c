/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Sun Position Calculator implementation
 * Simplified NOAA solar equations for sunrise / sunset.
 *
 * Reference: https://gml.noaa.gov/grad/solcalc/solareqns.PDF
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <math.h>
#include "sun_position.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD  (M_PI / 180.0)
#define RAD2DEG  (180.0 / M_PI)

/**
 * @brief Day-of-year (1-based) from calendar date.
 */
static int day_of_year(int year, int month, int day)
{
    static const int cum[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int doy = cum[month - 1] + day;
    /* Leap-year adjustment */
    if (month > 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) {
            doy++;
        }
    }
    return doy;
}

sun_times_t sun_position_calc(double latitude_deg,
                              double longitude_deg,
                              int    utc_offset_min,
                              int    year,
                              int    month,
                              int    day)
{
    sun_times_t result = { .valid = false, .sunrise_min = 0, .sunset_min = 0 };

    int doy = day_of_year(year, month, day);

    /* Fractional year (radians) */
    double gamma = 2.0 * M_PI / 365.0 * (doy - 1);

    /* Equation of time (minutes) */
    double eqtime = 229.18 * (0.000075
                    + 0.001868 * cos(gamma)
                    - 0.032077 * sin(gamma)
                    - 0.014615 * cos(2.0 * gamma)
                    - 0.040849 * sin(2.0 * gamma));

    /* Solar declination (radians) */
    double decl = 0.006918
                - 0.399912 * cos(gamma)
                + 0.070257 * sin(gamma)
                - 0.006758 * cos(2.0 * gamma)
                + 0.000907 * sin(2.0 * gamma)
                - 0.002697 * cos(3.0 * gamma)
                + 0.00148  * sin(3.0 * gamma);

    /* Hour angle for the official zenith (90.833°) */
    double lat_rad = latitude_deg * DEG2RAD;
    double zenith  = 90.833 * DEG2RAD;

    double cos_ha = (cos(zenith) / (cos(lat_rad) * cos(decl)))
                  - tan(lat_rad) * tan(decl);

    /* Polar day/night check */
    if (cos_ha < -1.0 || cos_ha > 1.0) {
        return result;   /* sun never rises or never sets */
    }

    double ha = acos(cos_ha) * RAD2DEG;   /* in degrees */

    /* Sunrise and sunset in minutes from midnight UTC */
    double sunrise_utc = 720.0 - 4.0 * (longitude_deg + ha) - eqtime;
    double sunset_utc  = 720.0 - 4.0 * (longitude_deg - ha) - eqtime;

    /* Convert to local time */
    double sunrise_local = sunrise_utc + (double)utc_offset_min;
    double sunset_local  = sunset_utc  + (double)utc_offset_min;

    /* Wrap to 0 – 1439 */
    while (sunrise_local < 0.0)    sunrise_local += 1440.0;
    while (sunrise_local >= 1440.0) sunrise_local -= 1440.0;
    while (sunset_local < 0.0)     sunset_local  += 1440.0;
    while (sunset_local >= 1440.0)  sunset_local  -= 1440.0;

    result.valid       = true;
    result.sunrise_min = (int)(sunrise_local + 0.5);
    result.sunset_min  = (int)(sunset_local  + 0.5);
    return result;
}
