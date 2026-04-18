/*
 * SPDX-FileCopyrightText: 2024 Waveshare (adapted for ESP-IDF v6.x)
 * SPDX-License-Identifier: MIT
 *
 * HX8394 MIPI-DSI panel driver – local component override.
 *
 * Differences from the registry waveshare/esp_lcd_hx8394 releases:
 *  • 4-argument esp_lcd_new_panel_hx8394() with dedicated
 *    esp_lcd_panel_hx8394_config_t (dsi_bus / dpi_config / lane_bit_rate_mbps).
 *  • Uses panel_dev_config->rgb_ele_order (not the removed ->color_space).
 *  • No i2c_bus dependency.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Component version */
#define ESP_LCD_HX8394_VER_MAJOR    1
#define ESP_LCD_HX8394_VER_MINOR    0
#define ESP_LCD_HX8394_VER_PATCH    0

/**
 * @brief Custom LCD init command entry.
 */
typedef struct {
    uint8_t        cmd;
    const uint8_t *data;
    size_t         data_bytes;
    unsigned int   delay_ms;
} hx8394_lcd_init_cmd_t;

/**
 * @brief HX8394-specific configuration passed alongside esp_lcd_panel_dev_config_t.
 */
typedef struct {
    esp_lcd_dsi_bus_handle_t          dsi_bus;           /**< DSI bus handle (required) */
    const esp_lcd_dpi_panel_config_t *dpi_config;        /**< DPI panel timing/format (required) */
    uint32_t                          lane_bit_rate_mbps; /**< DSI lane bit rate in Mbps */
    uint8_t                           num_data_lanes;    /**< Number of DSI data lanes (0 = default 2) */
    const hx8394_lcd_init_cmd_t      *init_cmds;         /**< Optional custom init sequence (NULL = use default) */
    uint16_t                          init_cmds_size;    /**< Number of entries in init_cmds */
} esp_lcd_panel_hx8394_config_t;

/**
 * @brief Create a new HX8394 MIPI-DSI panel driver.
 *
 * The @p io handle must be obtained via esp_lcd_new_panel_io_dbi() on the same
 * DSI bus that is also passed in @p hx_config->dsi_bus.  The DBI interface is
 * used for MIPI command-mode initialisation while the DPI interface (configured
 * via @p hx_config->dpi_config) carries the pixel stream.
 *
 * @param[in]  io               DBI panel-IO handle (from esp_lcd_new_panel_io_dbi()).
 * @param[in]  panel_dev_config Generic panel device configuration (reset GPIO, rgb_ele_order, bpp).
 * @param[in]  hx_config        HX8394-specific configuration (DSI bus, DPI config, lane rate).
 * @param[out] ret_panel        Panel handle written on success.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t esp_lcd_new_panel_hx8394(const esp_lcd_panel_io_handle_t          io,
                                    const esp_lcd_panel_dev_config_t         *panel_dev_config,
                                    const esp_lcd_panel_hx8394_config_t      *hx_config,
                                    esp_lcd_panel_handle_t                   *ret_panel);

#ifdef __cplusplus
}
#endif
