/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - OTA Firmware Update
 * Supports firmware updates via HTTP URL download.
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
 * @brief OTA update status codes.
 */
typedef enum {
    OTA_STATUS_IDLE = 0,       /**< No update in progress         */
    OTA_STATUS_DOWNLOADING,    /**< Downloading firmware image     */
    OTA_STATUS_FLASHING,       /**< Writing to flash               */
    OTA_STATUS_DONE,           /**< Update complete, pending reboot*/
    OTA_STATUS_ERROR,          /**< Update failed                  */
} ota_status_t;

/**
 * @brief Current OTA progress information.
 */
typedef struct {
    ota_status_t status;       /**< Current OTA status             */
    int          progress_pct; /**< Download progress 0-100        */
    char         error_msg[64];/**< Error description if failed    */
} ota_progress_t;

/**
 * @brief Start an OTA firmware update from the given URL.
 *
 * Launches a background FreeRTOS task that downloads the firmware
 * binary and writes it to the next OTA partition.  Non-blocking.
 *
 * @param url  HTTPS or HTTP URL to the firmware binary.
 * @return ESP_OK if the update task was launched successfully.
 */
esp_err_t ota_update_start(const char *url);

/**
 * @brief Get the current OTA update progress.
 */
ota_progress_t ota_update_get_progress(void);

/**
 * @brief Return true if an OTA update is currently in progress.
 */
bool ota_update_in_progress(void);

#ifdef __cplusplus
}
#endif
