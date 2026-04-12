/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – MIPI DSI Display Driver implementation
 *
 * Initialises MIPI DSI bus → ILI9881C panel → GT911 touch → LVGL.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * Display      : 5″ MIPI DSI, 800×480, ILI9881C controller
 * Touch        : Goodix GT911 via I2C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_lcd_ili9881c.h"
#include "esp_lcd_touch_gt911.h"
#include "lvgl.h"

#include "display_driver.h"

static const char *TAG = "display";

/* ── Display timing constants (ILI9881C, 800×480 @ ~60 Hz) ───────── */
#define LCD_H_RES              800
#define LCD_V_RES              480
#define LCD_HSYNC_PULSE_WIDTH  10
#define LCD_HBP                80
#define LCD_HFP                160
#define LCD_VSYNC_PULSE_WIDTH  1
#define LCD_VBP                23
#define LCD_VFP                12

/* Pixel clock: (800+10+80+160) × (480+1+23+12) × 60 ≈ 33 MHz */
#define LCD_DPI_CLK_MHZ        33

/* MIPI DSI bus parameters */
#define DSI_LANE_NUM           2
#define DSI_LANE_BITRATE_MBPS  500

/* MIPI DSI PHY power: internal LDO channel 3, 2.5 V */
#define DSI_PHY_LDO_CHAN       3
#define DSI_PHY_LDO_MV        2500

/* ── Board-level GPIOs (Waveshare ESP32-P4-WiFi6 rev 1.3) ────────── */
#ifndef CONFIG_DISPLAY_BK_LIGHT_GPIO
#define CONFIG_DISPLAY_BK_LIGHT_GPIO 26
#endif
#ifndef CONFIG_DISPLAY_RST_GPIO
#define CONFIG_DISPLAY_RST_GPIO      27
#endif
#ifndef CONFIG_DISPLAY_TOUCH_I2C_SDA_GPIO
#define CONFIG_DISPLAY_TOUCH_I2C_SDA_GPIO 7
#endif
#ifndef CONFIG_DISPLAY_TOUCH_I2C_SCL_GPIO
#define CONFIG_DISPLAY_TOUCH_I2C_SCL_GPIO 8
#endif
#ifndef CONFIG_DISPLAY_TOUCH_INT_GPIO
#define CONFIG_DISPLAY_TOUCH_INT_GPIO     -1
#endif
#ifndef CONFIG_DISPLAY_TOUCH_RST_GPIO
#define CONFIG_DISPLAY_TOUCH_RST_GPIO     -1
#endif

#define BK_LIGHT_ON_LEVEL   1
#define BK_LIGHT_OFF_LEVEL  0

/* ── LVGL configuration ──────────────────────────────────────────── */
#define LVGL_DRAW_BUF_LINES   (LCD_V_RES / 10)   /* 48 lines per buffer */
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)

/* ── Module state ────────────────────────────────────────────────── */
static _lock_t              s_lvgl_lock;
static lv_display_t        *s_display     = NULL;
static lv_indev_t          *s_touch_indev = NULL;
static esp_lcd_touch_handle_t s_touch     = NULL;

/* ── Forward declarations ────────────────────────────────────────── */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map);
static void lvgl_tick_cb(void *arg);
static void lvgl_task(void *arg);
static bool dpi_flush_ready_cb(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx);
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* ===================================================================
 *  Public API
 * =================================================================*/

esp_err_t display_driver_init(void)
{
    esp_err_t ret;

    /* ── 1. Power the MIPI DSI PHY via internal LDO ───────────────── */
    ESP_LOGI(TAG, "Enable MIPI DSI PHY LDO (chan %d, %d mV)",
             DSI_PHY_LDO_CHAN, DSI_PHY_LDO_MV);
    esp_ldo_channel_handle_t ldo_mipi = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO acquire failed: 0x%x", ret);
        return ret;
    }

    /* ── 2. Configure backlight GPIO ──────────────────────────────── */
#if CONFIG_DISPLAY_BK_LIGHT_GPIO >= 0
    gpio_config_t bk_cfg = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CONFIG_DISPLAY_BK_LIGHT_GPIO,
    };
    gpio_config(&bk_cfg);
    gpio_set_level(CONFIG_DISPLAY_BK_LIGHT_GPIO, BK_LIGHT_OFF_LEVEL);
#endif

    /* ── 3. Create MIPI DSI bus ───────────────────────────────────── */
    ESP_LOGI(TAG, "Create MIPI DSI bus (%d lanes, %d Mbps)",
             DSI_LANE_NUM, DSI_LANE_BITRATE_MBPS);
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id            = 0,
        .num_data_lanes    = DSI_LANE_NUM,
        .lane_bit_rate_mbps = DSI_LANE_BITRATE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus create failed: 0x%x", ret);
        return ret;
    }

    /* ── 4. Install DBI command IO ────────────────────────────────── */
    ESP_LOGI(TAG, "Install MIPI DSI DBI IO");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DBI IO create failed: 0x%x", ret);
        return ret;
    }

    /* ── 5. Create DPI panel (video-mode framebuffer) ─────────────── */
    ESP_LOGI(TAG, "Create DPI panel (%dx%d, %d MHz pixel clock)",
             LCD_H_RES, LCD_V_RES, LCD_DPI_CLK_MHZ);
    esp_lcd_panel_handle_t dpi_panel = NULL;
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel   = 0,
        .dpi_clk_src       = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .in_color_format   = LCD_COLOR_FMT_RGB888,
        .video_timing = {
            .h_size             = LCD_H_RES,
            .v_size             = LCD_V_RES,
            .hsync_pulse_width  = LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch   = LCD_HBP,
            .hsync_front_porch  = LCD_HFP,
            .vsync_pulse_width  = LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch   = LCD_VBP,
            .vsync_front_porch  = LCD_VFP,
        },
    };

    ili9881c_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DISPLAY_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config  = &vendor_cfg,
    };
    ret = esp_lcd_new_panel_ili9881c(dbi_io, &panel_cfg, &dpi_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILI9881C panel create failed: 0x%x", ret);
        return ret;
    }

    /* ── 6. Reset, init and turn on the panel ─────────────────────── */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(dpi_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(dpi_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(dpi_panel, true));

    /* Turn on backlight */
#if CONFIG_DISPLAY_BK_LIGHT_GPIO >= 0
    gpio_set_level(CONFIG_DISPLAY_BK_LIGHT_GPIO, BK_LIGHT_ON_LEVEL);
#endif
    ESP_LOGI(TAG, "LCD panel initialised – backlight ON");

    /* ── 7. Initialise I2C bus for GT911 touch controller ─────────── */
    ESP_LOGI(TAG, "Init I2C bus (SDA=%d, SCL=%d) for GT911 touch",
             CONFIG_DISPLAY_TOUCH_I2C_SDA_GPIO,
             CONFIG_DISPLAY_TOUCH_I2C_SCL_GPIO);
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source         = I2C_CLK_SRC_DEFAULT,
        .i2c_port            = I2C_NUM_0,
        .sda_io_num          = CONFIG_DISPLAY_TOUCH_I2C_SDA_GPIO,
        .scl_io_num          = CONFIG_DISPLAY_TOUCH_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&i2c_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed (0x%x) – touch disabled", ret);
    }

    /* ── 8. Install GT911 touch controller ────────────────────────── */
    if (i2c_bus) {
        esp_lcd_panel_io_handle_t touch_io = NULL;
        esp_lcd_panel_io_i2c_config_t touch_io_cfg =
            ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        ret = esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_cfg, &touch_io);
        if (ret == ESP_OK) {
            esp_lcd_touch_config_t tp_cfg = {
                .x_max        = LCD_H_RES,
                .y_max        = LCD_V_RES,
                .rst_gpio_num = CONFIG_DISPLAY_TOUCH_RST_GPIO,
                .int_gpio_num = CONFIG_DISPLAY_TOUCH_INT_GPIO,
                .levels       = { .reset = 0, .interrupt = 0 },
                .flags        = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
            };
            ret = esp_lcd_touch_new_i2c_gt911(touch_io, &tp_cfg, &s_touch);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "GT911 touch controller ready");
            } else {
                ESP_LOGW(TAG, "GT911 init failed (0x%x) – touch disabled", ret);
            }
        } else {
            ESP_LOGW(TAG, "Touch IO create failed (0x%x)", ret);
        }
    }

    /* ── 9. Initialise LVGL ───────────────────────────────────────── */
    ESP_LOGI(TAG, "Initialise LVGL (v%d.%d.%d)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();

    /* Create display */
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(s_display, dpi_panel);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB888);

    /* Allocate double-buffered draw buffers in PSRAM */
    size_t buf_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers (%u bytes)", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_display, buf1, buf2, buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    /* Register DPI panel "color transfer done" callback */
    esp_lcd_dpi_panel_event_callbacks_t panel_cbs = {
        .on_color_trans_done = dpi_flush_ready_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(
        dpi_panel, &panel_cbs, s_display));

    /* Register LVGL touch input device (if touch available) */
    if (s_touch) {
        s_touch_indev = lv_indev_create();
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);
        lv_indev_set_display(s_touch_indev, s_display);
        ESP_LOGI(TAG, "LVGL touch input device registered");
    }

    /* ── 10. Start LVGL tick timer ────────────────────────────────── */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                             LVGL_TICK_PERIOD_MS * 1000));

    /* ── 11. Start LVGL handler task ──────────────────────────────── */
    BaseType_t xr = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE,
                                NULL, LVGL_TASK_PRIORITY, NULL);
    if (xr != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Display driver initialised (%dx%d MIPI DSI + LVGL)",
             LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void display_lock(void)
{
    _lock_acquire(&s_lvgl_lock);
}

void display_unlock(void)
{
    _lock_release(&s_lvgl_lock);
}

lv_display_t *display_get_lvgl_display(void)
{
    return s_display;
}

/* ===================================================================
 *  Private helpers
 * =================================================================*/

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
}

static bool dpi_flush_ready_cb(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    uint32_t delay_ms = 0;
    while (1) {
        _lock_acquire(&s_lvgl_lock);
        delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);
        delay_ms = MAX(delay_ms, (uint32_t)LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, (uint32_t)LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * delay_ms);
    }
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_read_data(s_touch);

    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t  count = 0;

    if (esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &count, 1)
        && count > 0) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
