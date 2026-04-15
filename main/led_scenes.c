/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine (stub)
 * All preset scenes and the sun simulation have been removed.
 * The lighting schedule is now fully manual; see led_schedule.c.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include "led_scenes.h"

esp_err_t led_scenes_init(void)
{
    return ESP_OK;
}

void led_scenes_stop(void)
{
}
