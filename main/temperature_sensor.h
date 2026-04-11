/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - DS18B20 Water Temperature Sensor
 * Reads a DS18B20 1-Wire digital temperature sensor and exposes
 * the latest value through a thread-safe getter.
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
 * @brief Initialise the DS18B20 temperature sensor.
 *
 * Sets up the 1-Wire bus on the GPIO configured in Kconfig
 * (menuconfig → Aquarium Temperature Sensor Settings), enumerates
 * attached DS18B20 devices, and starts a FreeRTOS task that
 * periodically reads the temperature.
 *
 * @return
 *   - ESP_OK   on success (at least one DS18B20 found).
 *   - ESP_FAIL if the bus could not be created or no sensor found.
 */
esp_err_t temperature_sensor_init(void);

/**
 * @brief Get the last measured water temperature in °C.
 *
 * Thread-safe – may be called from any task or ISR context.
 *
 * @param[out] temp_c  Pointer to receive the temperature value.
 * @return
 *   - true  if a valid reading is available.
 *   - false if the sensor has not been read yet or the last read failed.
 */
bool temperature_sensor_get(float *temp_c);

#ifdef __cplusplus
}
#endif
