/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - SD Card Manager
 * Mounts a FAT-formatted microSD card via the SDMMC peripheral (slot 1,
 * GPIO matrix, configurable 1-bit/4-bit mode) and exposes helpers for configuration
 * backup/restore.  All log and data files written by other modules live
 * under the /sdcard mount point.
 *
 * SDMMC_HOST_SLOT_1 (GPIO-matrix-routable) is used for the SD card.
 * SDMMC_HOST_SLOT_0 (fixed IOMUX pads) is already occupied by the
 * esp_hosted WiFi coprocessor SDIO transport (GPIO 14-19).
 * Pin mapping for the onboard TF slot:
 *   SDMMC1_CLK = GPIO 43
 *   SDMMC1_CMD = GPIO 44
 *   SDMMC1_D0  = GPIO 39
 *   SDMMC1_D1  = GPIO 40
 *   SDMMC1_D2  = GPIO 41
 *   SDMMC1_D3  = GPIO 42
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 *
 * Directory layout on the SD card:
 *   /sdcard/logs/       – daily CSV data logs and diagnostic logs
 *   /sdcard/config/     – configuration backup files (JSON)
 *   /sdcard/www/        – web dashboard files served over HTTP
 *   /sdcard/firmware.bin – optional firmware image for local OTA
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount point used by the VFS FAT layer. */
#define SD_MOUNT_POINT   "/sdcard"

/** Subdirectory paths */
#define SD_LOGS_DIR      SD_MOUNT_POINT "/logs"
#define SD_CONFIG_DIR    SD_MOUNT_POINT "/config"
#define SD_WWW_DIR       SD_MOUNT_POINT "/www"

/** Default path for configuration backup file. */
#define SD_CONFIG_FILE   SD_CONFIG_DIR "/aquarium_config.json"

/** Path to the web dashboard HTML file served over HTTP. */
#define SD_WWW_INDEX     SD_WWW_DIR "/index.html"

/** Default path for local OTA firmware image. */
#define SD_FIRMWARE_FILE SD_MOUNT_POINT "/firmware.bin"

/**
 * @brief SD card summary reported by sd_card_get_info().
 */
typedef struct {
    bool     mounted;          /**< true if the card is currently mounted  */
    uint64_t total_bytes;      /**< Total card capacity in bytes            */
    uint64_t free_bytes;       /**< Free space on the card in bytes         */
    char     card_name[16];    /**< SD card product name (from CID)         */
    uint32_t card_speed_khz;   /**< Negotiated bus speed in kHz             */
} sd_card_info_t;

/**
 * @brief Initialise the SD card subsystem.
 *
 * Initialises the SDMMC1 host, configures the slot based on Kconfig and mounts
 * the FAT filesystem at /sdcard.  Creates the logs/ and config/
 * subdirectories if they do not exist.
 *
 * This function is intentionally non-fatal: if no card is present or
 * mount fails it returns an error but the rest of the system continues
 * operating normally.
 *
 * @return
 *   - ESP_OK        on success.
 *   - ESP_ERR_NOT_FOUND  if CONFIG_SD_CARD_ENABLED is false (skipped).
 *   - Other error codes from the SDMMC / FAT layer on hardware failure.
 */
esp_err_t sd_card_init(void);

/**
 * @brief Unmount the SD card and release all resources.
 *
 * Safe to call even when the card is not mounted.
 */
void sd_card_deinit(void);

/**
 * @brief Return true if the SD card is currently mounted.
 */
bool sd_card_is_mounted(void);

/**
 * @brief Fill @p info with current SD card statistics.
 *
 * If the card is not mounted, @p info->mounted will be false and
 * all size fields will be zero.
 *
 * @param[out] info  Pointer to the structure to fill.
 */
void sd_card_get_info(sd_card_info_t *info);

/**
 * @brief Export the full system configuration to a JSON file on the SD card.
 *
 * Reads the current settings from every module (LED schedule, relay names
 * and schedules, auto-heater, CO2 controller, feeding mode, Telegram,
 * DuckDNS, daily cycle, timezone) and writes them to @p path as a
 * human-readable JSON file.
 *
 * @param path  Destination file path (e.g. SD_CONFIG_FILE).
 *              Must be under SD_MOUNT_POINT.
 * @return ESP_OK on success, or an error if the SD card is not mounted
 *         or the file cannot be written.
 */
esp_err_t sd_card_config_export(const char *path);

/**
 * @brief Restore system configuration from a JSON file on the SD card.
 *
 * Reads the file at @p path, parses the JSON and applies each section
 * to the corresponding module using its set_config() API.  Missing
 * sections are silently skipped.  The system does NOT reboot after
 * import; a manual reboot is recommended to ensure all modules pick up
 * the new values.
 *
 * @param path  Source file path (e.g. SD_CONFIG_FILE).
 * @return ESP_OK on success, or an error if the file cannot be read or
 *         the JSON is malformed.
 */
esp_err_t sd_card_config_import(const char *path);

#ifdef __cplusplus
}
#endif
