/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Aquarium Profiles
 * Preset helpers for common aquarium configurations.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t aquarium_profile_apply(const char *type);

#ifdef __cplusplus
}
#endif
