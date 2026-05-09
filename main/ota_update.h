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

/** Maximum length of the OTA error message string. */
#define OTA_ERROR_MSG_MAX 192

/**
 * @brief Current OTA progress information.
 */
typedef struct {
    ota_status_t status;       /**< Current OTA status             */
    int          progress_pct; /**< Download progress 0-100        */
    char         error_msg[OTA_ERROR_MSG_MAX];/**< Error description if failed */
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

/* ── Direct-upload OTA API ───────────────────────────────────────── */

/**
 * @brief Begin an OTA firmware upload from raw binary data.
 *
 * Selects the next OTA partition and calls esp_ota_begin().
 * Must be followed by one or more ota_update_write() calls and
 * a final ota_update_finish() (or ota_update_abort_upload() on error).
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t ota_update_begin(void);

/**
 * @brief Write a chunk of firmware binary data to the OTA partition.
 *
 * @param buf  Pointer to binary data buffer.
 * @param len  Number of bytes to write.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t ota_update_write(const void *buf, size_t len);

/**
 * @brief Finalise the OTA upload, validate the image, and set the new
 *        partition as the boot target.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t ota_update_finish(void);

/**
 * @brief Abort an in-progress OTA upload and reset internal state.
 */
void ota_update_abort_upload(void);

/**
 * @brief Update the flash progress percentage displayed by /api/ota_status.
 *
 * @param pct  Progress value 0–100.
 */
void ota_update_set_progress(int pct);

#ifdef __cplusplus
}
#endif
