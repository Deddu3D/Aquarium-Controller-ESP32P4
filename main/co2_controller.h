/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - CO2 Solenoid Controller
 * Automates a CO2 solenoid valve (via relay) in sync with the
 * LED lighting schedule.  The valve opens when the lights come
 * on and closes when they go off, with optional pre/post delays
 * to pre-fill the reactor and prevent CO2 waste after lights-out.
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
 * @brief CO2 controller configuration.
 */
typedef struct {
    bool enabled;           /**< CO2 control active                         */
    int  relay_index;       /**< Relay channel (0–3) for the solenoid valve */
    int  pre_on_min;        /**< Open valve N minutes BEFORE lights on (0–60) */
    int  post_off_min;      /**< Keep valve open N minutes AFTER lights off (0–60) */
} co2_config_t;

/**
 * @brief Initialise the CO2 controller module.
 *
 * Loads configuration from NVS.  Does not start a dedicated task;
 * call co2_controller_tick() from the main loop instead.
 *
 * @return ESP_OK on success.
 */
esp_err_t co2_controller_init(void);

/**
 * @brief Get the current CO2 controller configuration.
 */
co2_config_t co2_controller_get_config(void);

/**
 * @brief Update the CO2 controller configuration and persist to NVS.
 *
 * @param cfg  Pointer to the new configuration.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t co2_controller_set_config(const co2_config_t *cfg);

/**
 * @brief Evaluate CO2 valve logic against the current LED schedule.
 *
 * Call periodically (e.g. every 10 s from the main loop).
 * Opens/closes the solenoid relay based on the LED schedule window
 * and the configured pre/post delays.
 */
void co2_controller_tick(void);

#ifdef __cplusplus
}
#endif
