/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Auto-Heater Controller
 * Thermostat logic using a relay + DS18B20 temperature sensor.
 *
 * The heater relay is engaged when the water temperature drops below
 * a configurable low threshold and disengaged when it exceeds a high
 * threshold, providing hysteresis to avoid relay chatter.
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

/**
 * @brief Auto-heater configuration.
 */
typedef struct {
    bool  enabled;         /**< Auto-heater control active          */
    int   relay_index;     /**< Relay channel to use (0-3)          */
    float target_temp_c;   /**< Target temperature (°C)             */
    float hysteresis_c;    /**< Hysteresis band (°C), default 0.5   */
} auto_heater_config_t;

/**
 * @brief Initialise the auto-heater module.
 *
 * Loads configuration from NVS.  Does not start a dedicated task;
 * call auto_heater_tick() from the main loop instead.
 *
 * @return ESP_OK on success.
 */
esp_err_t auto_heater_init(void);

/**
 * @brief Get the current auto-heater configuration.
 */
auto_heater_config_t auto_heater_get_config(void);

/**
 * @brief Set the auto-heater configuration.
 *
 * Validates and persists the configuration to NVS.
 *
 * @param cfg  Pointer to the new configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t auto_heater_set_config(const auto_heater_config_t *cfg);

/**
 * @brief Evaluate thermostat logic and control the heater relay.
 *
 * Call this periodically (e.g. every 10 s from the main loop).
 * Reads the current water temperature and turns the heater relay
 * on/off based on the target and hysteresis settings.
 */
void auto_heater_tick(void);

#ifdef __cplusplus
}
#endif
