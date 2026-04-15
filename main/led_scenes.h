/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Scene Engine (stub)
 * All preset scenes and the sun simulation have been removed.
 * This header is kept so that existing callers (main.c) compile
 * without modification.  led_scenes_init() is a no-op.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief No-op stub – scenes engine has been removed.
 * @return Always ESP_OK.
 */
esp_err_t led_scenes_init(void);

/**
 * @brief No-op stub.
 */
void led_scenes_stop(void);

#ifdef __cplusplus
}
#endif
