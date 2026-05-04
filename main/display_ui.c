/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Touch Display UI
 * LVGL v9 dashboard for the Waveshare 4-DSI-TOUCH-A
 * (720 × 720 round IPS, HX8394 MIPI-DSI panel, GT911 touch).
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 * LVGL         : v9.x
 */

#include "display_ui.h"

/* ── Skip everything when display is disabled in Kconfig ──────────────── */
#if !CONFIG_DISPLAY_ENABLED

#include "esp_log.h"
static const char *TAG_STUB = "display_ui";
esp_err_t display_ui_init(void)
{
    ESP_LOGD(TAG_STUB, "Display disabled in Kconfig – skipping initialisation");
    return ESP_ERR_NOT_SUPPORTED;
}
void display_ui_show_alarm(const char *msg, const char *detail)
{
    (void)msg; (void)detail;
}

#else  /* CONFIG_DISPLAY_ENABLED */

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  1. Includes / constants / forward declarations
 * ╚══════════════════════════════════════════════════════════════════════ */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

/* LCD + Touch hardware */
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_hx8394.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"

/* LVGL */
#include "lvgl.h"

/* Aquarium controller modules */
#include "temperature_sensor.h"
#include "temperature_history.h"
#include "led_controller.h"
#include "led_schedule.h"
#include "led_scenes.h"
#include "relay_controller.h"
#include "auto_heater.h"
#include "co2_controller.h"
#include "wifi_manager.h"
#include "feeding_mode.h"

static const char *TAG = "display_ui";

/* ── Display geometry ──────────────────────────────────────────────── */
#define LCD_W              720
#define LCD_H              720

/* ── MIPI-DSI timing for HX8394 720 × 720 panel ───────────────────── */
#define DSI_LANE_BITRATE_MBPS  1000
#define DPI_CLK_MHZ            40
#define LCD_HBP                40
#define LCD_HFP                40
#define LCD_HSYNC              8
#define LCD_VBP                40
#define LCD_VFP                40
#define LCD_VSYNC              8

/* ── Touch I2C (GPIO from Kconfig) ─────────────────────────────────── */
#define TOUCH_SCL   CONFIG_TOUCH_I2C_SCL
#define TOUCH_SDA   CONFIG_TOUCH_I2C_SDA

/* ── LVGL task / tick ───────────────────────────────────────────────── */
#define LVGL_TICK_MS        5
#define LVGL_TASK_STACK     (14 * 1024)
#define LVGL_TASK_PRIO      2
#define LVGL_BUF_LINES      40            /* partial-render buffer height */
#define UI_REFRESH_MS       2000          /* data polling interval        */

/* ── Layout constants ───────────────────────────────────────────────── */
#define STATUS_H    48                    /* fixed status bar height       */
#define TABBAR_H    65                    /* bottom tab bar height         */

/* ── Tab indices ────────────────────────────────────────────────────── */
#define TAB_HOME        0
#define TAB_LUCI        1
#define TAB_TEMPERATURA 2
#define TAB_AUTOMAZIONI 3
#define TAB_DATI        4

/* ── Chart ──────────────────────────────────────────────────────────── */
#define CHART_POINTS    48                /* 48 × 30-min slots = 24 h     */

/* ── Colour palette – IoT dashboard dark theme ──────────────────────── */
#define C_BG        0x0B1E2D   /* deep navy background                    */
#define C_CARD      0x102A3D   /* card surface                            */
#define C_BORDER    0x1A3A55   /* subtle divider / border                 */
#define C_INPUT     0x0D2236   /* input / secondary surface               */
#define C_PRIMARY   0x1FA3FF   /* blue  – primary accent                  */
#define C_YELLOW    0xF1C40F   /* yellow – lights / scene                 */
#define C_ORANGE    0xF39C12   /* orange – temperature warning            */
#define C_TEXT      0xECF5FB   /* near-white main text                    */
#define C_MUTED     0x5C7F9A   /* slate-grey muted text                   */
#define C_ON        0x2ECC71   /* green – OK / active                     */
#define C_ON_BG     0x1A4A2A   /* dark green background                   */
#define C_OFF       0x1A2E40   /* off / disabled background               */
#define C_ERR       0xE74C3C   /* red – alarm / error                     */
#define C_ERR_BG    0x4A1218   /* dark red background                     */
#define C_BAR_BG    0x070F1A   /* status bar background                   */

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  2. Static state – hardware handles + widget references
 * ╚══════════════════════════════════════════════════════════════════════ */

static esp_lcd_panel_handle_t  s_panel   = NULL;
static esp_lcd_touch_handle_t  s_touch   = NULL;
static lv_display_t           *s_disp    = NULL;
static SemaphoreHandle_t       s_mutex   = NULL;

/* ─ TabView (for Home card → tab navigation) ──────────────────────── */
static lv_obj_t *s_tv = NULL;

/* ─ Status bar ────────────────────────────────────────────────────── */
static lv_obj_t *s_status_time_lbl = NULL;
static lv_obj_t *s_status_temp_lbl = NULL;
static lv_obj_t *s_status_ok_chip  = NULL;
static lv_obj_t *s_status_ok_lbl   = NULL;

/* ─ Home tab cards ────────────────────────────────────────────────── */
static lv_obj_t *s_home_temp_val   = NULL;   /* "25.3°C"          */
static lv_obj_t *s_home_temp_badge = NULL;   /* "OK" pill         */
static lv_obj_t *s_home_led_val    = NULL;   /* "80%"             */
static lv_obj_t *s_home_led_badge  = NULL;   /* "ON" pill         */
static lv_obj_t *s_home_co2_val    = NULL;   /* "ON" / "OFF"      */
static lv_obj_t *s_home_co2_sub    = NULL;   /* "Terminazione: …" */
static lv_obj_t *s_home_water_val  = NULL;   /* "OK"              */
static lv_obj_t *s_home_water_sub  = NULL;   /* "Stato: Normale"  */

/* ─ Luci tab ──────────────────────────────────────────────────────── */
static lv_obj_t *s_luci_sw           = NULL;
static lv_obj_t *s_luci_br_sl        = NULL;
static lv_obj_t *s_luci_br_lbl       = NULL;
static lv_obj_t *s_luci_scene_btn[4] = { NULL, NULL, NULL, NULL };

/* ─ Temperatura tab ───────────────────────────────────────────────── */
static lv_obj_t *s_temp_arc        = NULL;
static lv_obj_t *s_temp_big_lbl    = NULL;
static lv_obj_t *s_temp_target_sb  = NULL;
static lv_obj_t *s_heater_chip     = NULL;
static lv_obj_t *s_heater_lbl      = NULL;
static lv_obj_t *s_cool_chip       = NULL;
static lv_obj_t *s_cool_lbl        = NULL;

/* ─ Automazioni tab ───────────────────────────────────────────────── */
static lv_obj_t *s_auto_luci_sw  = NULL;
static lv_obj_t *s_auto_luci_inf = NULL;
static lv_obj_t *s_auto_co2_sw   = NULL;
static lv_obj_t *s_auto_co2_inf  = NULL;
static lv_obj_t *s_auto_ht_sw    = NULL;
static lv_obj_t *s_auto_ht_inf   = NULL;
static lv_obj_t *s_auto_feed_btn = NULL;
static lv_obj_t *s_auto_feed_lbl = NULL;
static lv_obj_t *s_auto_rel_sw[RELAY_COUNT]   = { NULL, NULL, NULL, NULL };
static lv_obj_t *s_auto_rel_name[RELAY_COUNT] = { NULL, NULL, NULL, NULL };

/* ─ Dati tab ──────────────────────────────────────────────────────── */
static lv_obj_t           *s_chart      = NULL;
static lv_chart_series_t  *s_chart_ser  = NULL;
static lv_obj_t           *s_chart_sel[3] = { NULL, NULL, NULL };
static int                 s_chart_mode = 0;

/* ─ Alarm overlay ─────────────────────────────────────────────────── */
static lv_obj_t *s_alarm_overlay = NULL;

/* ─ Forward declarations ──────────────────────────────────────────── */
static void auto_feed_cb(lv_event_t *e);
static void chart_update(int mode);

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  3. Hardware initialisation
 * ╚══════════════════════════════════════════════════════════════════════ */

/* DPI config stored statically so the panel can reference it after init */
static esp_lcd_dpi_panel_config_t s_dpi_cfg = {
    .virtual_channel    = 0,
    .dpi_clock_freq_mhz = DPI_CLK_MHZ,
    .in_color_format    = LCD_COLOR_FMT_RGB565,
    .num_fbs            = 1,
    .video_timing = {
        .h_size              = LCD_W,
        .v_size              = LCD_H,
        .hsync_back_porch    = LCD_HBP,
        .hsync_pulse_width   = LCD_HSYNC,
        .hsync_front_porch   = LCD_HFP,
        .vsync_back_porch    = LCD_VBP,
        .vsync_pulse_width   = LCD_VSYNC,
        .vsync_front_porch   = LCD_VFP,
    },
};

static esp_err_t lcd_hw_init(void)
{
    /* ── MIPI-DSI bus ─────────────────────────────────────────────── */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id              = 0,
        .num_data_lanes      = 2,
        .phy_clk_src         = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps  = DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus),
                        TAG, "DSI bus init failed");

    /* ── DBI interface (sends panel initialisation commands) ─────── */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io),
                        TAG, "DBI IO init failed");

    /* ── HX8394 panel ────────────────────────────────────────────── */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_hx8394_config_t hx_cfg = {
        .dsi_bus            = dsi_bus,
        .dpi_config         = &s_dpi_cfg,
        .lane_bit_rate_mbps = DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_hx8394(io, &panel_cfg, &hx_cfg, &s_panel),
        TAG, "HX8394 panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel),
                        TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),
                        TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "Panel display-on failed");

    /* ── Optional backlight GPIO ──────────────────────────────────── */
#if CONFIG_DISPLAY_BACKLIGHT_GPIO >= 0
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_DISPLAY_BACKLIGHT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(CONFIG_DISPLAY_BACKLIGHT_GPIO, 1);
    ESP_LOGI(TAG, "Backlight GPIO %d ON", CONFIG_DISPLAY_BACKLIGHT_GPIO);
#endif

    ESP_LOGI(TAG, "HX8394 720×720 panel ready");
    return ESP_OK;
}

static esp_err_t touch_hw_init(void)
{
    /* ── I2C master bus ──────────────────────────────────────────── */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port              = I2C_NUM_0,
        .scl_io_num            = TOUCH_SCL,
        .sda_io_num            = TOUCH_SDA,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus),
                        TAG, "I2C master bus init failed");

    /* ── GT911 touch controller ──────────────────────────────────── */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io),
        TAG, "GT911 panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max         = LCD_W,
        .y_max         = LCD_H,
        .rst_gpio_num  = GPIO_NUM_NC,
        .int_gpio_num  = GPIO_NUM_NC,
        .levels        = { .reset = 0, .interrupt = 0 },
        .flags         = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch),
        TAG, "GT911 touch init failed");

    ESP_LOGI(TAG, "GT911 touch controller ready (I2C SCL=%d SDA=%d)",
             TOUCH_SCL, TOUCH_SDA);
    return ESP_OK;
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  4. LVGL port – flush, touch read, tick, task, mutex
 * ╚══════════════════════════════════════════════════════════════════════ */

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_point_data_t points[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_get_data(s_touch, points, &cnt, 1);
    if (cnt > 0) {
        data->point.x = (int32_t)points[0].x;
        data->point.y = (int32_t)points[0].y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);   /* called from esp_timer task – thread-safe */
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL handler task started");
    while (1) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        uint32_t next_ms = lv_timer_handler();
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(next_ms > 0 && next_ms < 100 ? next_ms : 10));
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  5. Style helpers
 * ╚══════════════════════════════════════════════════════════════════════ */

/* Standard dark card */
static void style_card(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_pad_all(o, 14, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* Transparent container (no bg, no border, no padding) */
static void style_transp(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* Tab page background */
static void style_tab_page(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_set_style_pad_gap(o, 12, 0);
}

/* Bold section title */
static void style_title(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_20, 0);
}

/* Muted subtitle / label */
static void style_muted(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
}

/* Blue primary button */
static void style_btn_primary(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_PRIMARY), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1580CC), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(C_PRIMARY), 0);
    lv_obj_set_style_shadow_width(o, 14, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_30, 0);
}

/* Green button */
static void style_btn_green(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ON_BG), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0E3320), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o, lv_color_hex(C_ON), 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

/* Red / alarm button */
static void style_btn_red(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ERR), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xB03020), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

/* Dark / secondary button */
static void style_btn_dark(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x081525), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

/* ON/OFF switch styling */
static void style_switch(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_OFF),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ON),
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(o, lv_color_hex(C_ON),
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(o, 10,
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(o, LV_OPA_50,
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_TEXT),
                               LV_PART_KNOB | LV_STATE_DEFAULT);
}

/* Yellow/amber brightness slider */
static void style_slider_yellow(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_YELLOW),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_YELLOW),
                               LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(o, lv_color_hex(C_YELLOW),
                                   LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(o, 10, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(o, LV_OPA_30, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_height(o, 8);
    lv_obj_set_style_pad_ver(o, 14, LV_PART_MAIN);
}

/* Spinbox */
static void style_spinbox(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_all(o, 6, 0);
    lv_obj_set_style_border_width(o, 0, LV_PART_CURSOR);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  6. Widget helpers
 * ╚══════════════════════════════════════════════════════════════════════ */

/* Spinbox +/- callbacks */
static void sb_inc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT)
        lv_spinbox_increment((lv_obj_t *)lv_event_get_user_data(e));
}
static void sb_dec_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT)
        lv_spinbox_decrement((lv_obj_t *)lv_event_get_user_data(e));
}

/* [−][spinbox][+] row */
static lv_obj_t *make_sb(lv_obj_t *parent,
                          int32_t  min_v, int32_t  max_v,
                          int32_t  init_v,
                          uint8_t  digits, uint8_t  sep)
{
    lv_obj_t *row = lv_obj_create(parent);
    style_transp(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 4, 0);

    lv_obj_t *sb = lv_spinbox_create(row);
    lv_spinbox_set_range(sb, min_v, max_v);
    lv_spinbox_set_digit_format(sb, digits, sep);
    lv_spinbox_set_value(sb, init_v);
    style_spinbox(sb);

    lv_obj_t *bm = lv_button_create(row);
    lv_obj_set_size(bm, 44, 44);
    lv_obj_set_style_radius(bm, 22, 0);
    style_btn_dark(bm);
    lv_obj_t *lm = lv_label_create(bm);
    lv_label_set_text(lm, LV_SYMBOL_MINUS);
    lv_obj_center(lm);
    lv_obj_add_event_cb(bm, sb_dec_cb, LV_EVENT_ALL, sb);

    lv_obj_t *bp = lv_button_create(row);
    lv_obj_set_size(bp, 44, 44);
    lv_obj_set_style_radius(bp, 22, 0);
    style_btn_dark(bp);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, LV_SYMBOL_PLUS);
    lv_obj_center(lp);
    lv_obj_add_event_cb(bp, sb_inc_cb, LV_EVENT_ALL, sb);

    lv_obj_move_to_index(bm, 0);
    return sb;
}

/* Flex row (space-between, transparent) */
static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    style_transp(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    return r;
}

/* Small status badge (rounded pill) – returns the container */
static lv_obj_t *make_badge(lv_obj_t *parent, const char *text,
                             uint32_t bg_col, uint32_t fg_col,
                             lv_obj_t **lbl_out)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 20, 0);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_pad_ver(chip, 4, 0);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg_col), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    if (lbl_out) *lbl_out = lbl;
    return chip;
}

/* Home card: half-width clickable card */
static void home_card_nav_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int t = (int)(uintptr_t)lv_event_get_user_data(e);
    if (s_tv) lv_tabview_set_active(s_tv, (uint32_t)t, LV_ANIM_OFF);
}

static lv_obj_t *make_home_card(lv_obj_t *parent, int nav_tab)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, 140);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (nav_tab >= 0) {
        lv_obj_add_event_cb(card, home_card_nav_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)nav_tab);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x15354F), LV_STATE_PRESSED);
    }
    return card;
}

/* Card header row: icon + LABEL */
static void make_card_header(lv_obj_t *parent, const char *icon,
                              uint32_t icon_col, const char *label)
{
    lv_obj_t *r = lv_obj_create(parent);
    style_transp(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START,
                             LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, 8, 0);

    lv_obj_t *ico = lv_label_create(r);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_color(ico, lv_color_hex(icon_col), 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_16, 0);

    lv_obj_t *lbl = lv_label_create(r);
    lv_label_set_text(lbl, label);
    style_muted(lbl);
}

/* Automation list item row – returns switch handle */
static lv_obj_t *make_auto_item(lv_obj_t *parent,
                                 const char *icon_sym, uint32_t icon_col,
                                 const char *name,
                                 const char *info1, const char *info2,
                                 bool on_state,
                                 lv_obj_t **name_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 12, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 14, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Icon */
    lv_obj_t *ico = lv_label_create(row);
    lv_label_set_text(ico, icon_sym);
    lv_obj_set_style_text_color(ico, lv_color_hex(icon_col), 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_20, 0);
    lv_obj_set_width(ico, 28);

    /* Text column */
    lv_obj_t *mid = lv_obj_create(row);
    style_transp(mid);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(mid, 2, 0);

    lv_obj_t *name_lbl = lv_label_create(mid);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, LV_PCT(100));
    if (name_out) *name_out = name_lbl;

    if (info1 && info1[0]) {
        lv_obj_t *i1 = lv_label_create(mid);
        lv_label_set_text(i1, info1);
        style_muted(i1);
    }
    if (info2 && info2[0]) {
        lv_obj_t *i2 = lv_label_create(mid);
        lv_label_set_text(i2, info2);
        style_muted(i2);
    }

    /* Switch */
    lv_obj_t *sw = lv_switch_create(row);
    style_switch(sw);
    if (on_state) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  7. Home tab – 2×2 dashboard grid
 * ╚══════════════════════════════════════════════════════════════════════ */

static void build_home_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    /* ── Row 1 ─────────────────────────────────────────────────── */
    lv_obj_t *row1 = lv_obj_create(tab);
    style_transp(row1);
    lv_obj_set_width(row1, LV_PCT(100));
    lv_obj_set_height(row1, 140);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row1, 12, 0);

    /* Card 1 – Temperatura */
    lv_obj_t *c1 = make_home_card(row1, TAB_TEMPERATURA);
    make_card_header(c1, LV_SYMBOL_WARNING, C_PRIMARY, "TEMPERATURA");
    s_home_temp_val = lv_label_create(c1);
    lv_label_set_text(s_home_temp_val, "--.-\xc2\xb0""C");
    lv_obj_set_style_text_font(s_home_temp_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_home_temp_val, lv_color_hex(C_TEXT), 0);
    s_home_temp_badge = make_badge(c1, LV_SYMBOL_OK "  OK",
                                   C_ON_BG, C_ON, NULL);

    /* Card 2 – Luci */
    lv_obj_t *c2 = make_home_card(row1, TAB_LUCI);
    make_card_header(c2, LV_SYMBOL_IMAGE, C_YELLOW, "LUCI");
    s_home_led_val = lv_label_create(c2);
    lv_label_set_text(s_home_led_val, "--%");
    lv_obj_set_style_text_font(s_home_led_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_home_led_val, lv_color_hex(C_TEXT), 0);
    s_home_led_badge = make_badge(c2, "OFF", C_OFF, C_MUTED, NULL);

    /* ── Row 2 ─────────────────────────────────────────────────── */
    lv_obj_t *row2 = lv_obj_create(tab);
    style_transp(row2);
    lv_obj_set_width(row2, LV_PCT(100));
    lv_obj_set_height(row2, 140);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row2, 12, 0);

    /* Card 3 – CO2 */
    lv_obj_t *c3 = make_home_card(row2, TAB_AUTOMAZIONI);
    make_card_header(c3, LV_SYMBOL_REFRESH, C_ON, "CO\u2082");
    s_home_co2_val = lv_label_create(c3);
    lv_label_set_text(s_home_co2_val, "OFF");
    lv_obj_set_style_text_font(s_home_co2_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_home_co2_val, lv_color_hex(C_MUTED), 0);
    s_home_co2_sub = lv_label_create(c3);
    lv_label_set_text(s_home_co2_sub, "Terminazione: --:--");
    style_muted(s_home_co2_sub);

    /* Card 4 – Livello Acqua */
    lv_obj_t *c4 = make_home_card(row2, -1);
    make_card_header(c4, LV_SYMBOL_WIFI, C_PRIMARY, "LIVELLO ACQUA");
    s_home_water_val = lv_label_create(c4);
    lv_label_set_text(s_home_water_val, "OK");
    lv_obj_set_style_text_font(s_home_water_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_home_water_val, lv_color_hex(C_ON), 0);
    s_home_water_sub = lv_label_create(c4);
    lv_label_set_text(s_home_water_sub, "Stato: Normale");
    style_muted(s_home_water_sub);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  8. Luci tab – brightness slider + scene buttons
 * ╚══════════════════════════════════════════════════════════════════════ */

static const led_scene_t k_scene_ids[4] = {
    LED_SCENE_SUNRISE, LED_SCENE_NONE, LED_SCENE_SUNSET, LED_SCENE_MOONLIGHT
};
static const uint32_t k_scene_colors[4] = {
    0xF39C12, C_YELLOW, 0xE67E22, C_PRIMARY
};
static const char *k_scene_names[4]  = { "Alba", "Giorno", "Tramonto", "Notte" };
static const char *k_scene_pcts[4]   = { "30%", "100%", "50%", "10%" };
static const char *k_scene_icons[4]  = {
    LV_SYMBOL_PLUS, LV_SYMBOL_IMAGE, LV_SYMBOL_MINUS, LV_SYMBOL_LOOP
};
/* Selected scene background colours */
static const uint32_t k_scene_sel_bg[4] = {
    0x3D2000, 0x3D3000, 0x3D1500, 0x0A1A35
};

static void luci_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint32_t ramp = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED))
        led_controller_fade_on(ramp);
    else
        led_controller_fade_off(ramp);
}

static void luci_br_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int32_t pct = lv_slider_get_value(lv_event_get_target(e));
    led_controller_set_brightness((uint8_t)(pct * 255 / 100));
    if (s_luci_br_lbl)
        lv_label_set_text_fmt(s_luci_br_lbl, "%" PRId32 "%%", pct);
}

static void scene_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);

    for (int i = 0; i < 4; i++) {
        if (!s_luci_scene_btn[i]) continue;
        bool sel = (i == idx);
        lv_obj_set_style_bg_color(s_luci_scene_btn[i],
            lv_color_hex(sel ? k_scene_sel_bg[i] : C_INPUT), 0);
        lv_obj_set_style_border_width(s_luci_scene_btn[i], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_luci_scene_btn[i],
            lv_color_hex(k_scene_colors[i]), 0);
    }

    if (k_scene_ids[idx] == LED_SCENE_NONE) {
        led_scenes_stop();
        led_controller_set_brightness(255);
        led_controller_fade_on((uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000);
    } else {
        led_scenes_start(k_scene_ids[idx]);
    }
}

static void build_luci_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    /* ── Header card: title + ON/OFF toggle ───────────────────── */
    lv_obj_t *hc = lv_obj_create(tab);
    style_card(hc);
    lv_obj_set_width(hc, LV_PCT(100));
    lv_obj_set_height(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hc, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *htitle = lv_label_create(hc);
    lv_label_set_text(htitle, "LUCI");
    style_title(htitle);

    lv_obj_t *hright = lv_obj_create(hc);
    style_transp(hright);
    lv_obj_set_size(hright, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hright, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hright, LV_FLEX_ALIGN_END,
                                   LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(hright, 10, 0);

    lv_obj_t *hon_lbl = lv_label_create(hright);
    bool led_on = led_controller_is_on();
    lv_label_set_text(hon_lbl, led_on ? "ON" : "OFF");
    lv_obj_set_style_text_color(hon_lbl,
        lv_color_hex(led_on ? C_ON : C_MUTED), 0);
    lv_obj_set_style_text_font(hon_lbl, &lv_font_montserrat_20, 0);

    s_luci_sw = lv_switch_create(hright);
    style_switch(s_luci_sw);
    if (led_on) lv_obj_add_state(s_luci_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_luci_sw, luci_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Brightness slider card ───────────────────────────────── */
    lv_obj_t *bc = lv_obj_create(tab);
    style_card(bc);
    lv_obj_set_width(bc, LV_PCT(100));
    lv_obj_set_height(bc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(bc, 14, 0);

    /* Label row: dim ── 80% ── bright */
    lv_obj_t *br = make_row(bc);
    lv_obj_t *brl = lv_label_create(br);
    lv_label_set_text(brl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(brl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(brl, &lv_font_montserrat_20, 0);

    int cur_pct = led_controller_get_brightness() * 100 / 255;
    s_luci_br_lbl = lv_label_create(br);
    lv_label_set_text_fmt(s_luci_br_lbl, "%d%%", cur_pct);
    lv_obj_set_style_text_color(s_luci_br_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_luci_br_lbl, &lv_font_montserrat_20, 0);

    lv_obj_t *brr = lv_label_create(br);
    lv_label_set_text(brr, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(brr, lv_color_hex(C_YELLOW), 0);
    lv_obj_set_style_text_font(brr, &lv_font_montserrat_20, 0);

    s_luci_br_sl = lv_slider_create(bc);
    lv_obj_set_width(s_luci_br_sl, LV_PCT(100));
    lv_slider_set_range(s_luci_br_sl, 0, 100);
    lv_slider_set_value(s_luci_br_sl, cur_pct, LV_ANIM_OFF);
    style_slider_yellow(s_luci_br_sl);
    lv_obj_add_event_cb(s_luci_br_sl, luci_br_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Scene buttons card ───────────────────────────────────── */
    lv_obj_t *sc = lv_obj_create(tab);
    style_card(sc);
    lv_obj_set_width(sc, LV_PCT(100));
    lv_obj_set_height(sc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(sc, 12, 0);

    lv_obj_t *stitle = lv_label_create(sc);
    lv_label_set_text(stitle, "Scena");
    style_muted(stitle);

    lv_obj_t *srow = lv_obj_create(sc);
    style_transp(srow);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_height(srow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(srow, 8, 0);

    led_scene_t active = led_scenes_get_active();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_obj_create(srow);
        s_luci_scene_btn[i] = btn;
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 105);

        bool is_sel = (k_scene_ids[i] == active && active != LED_SCENE_NONE) ||
                      (i == 1 && active == LED_SCENE_NONE && led_on);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(is_sel ? k_scene_sel_bg[i] : C_INPUT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, is_sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(k_scene_colors[i]), 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                                   LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(btn, 4, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1F28), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, scene_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *ico_lbl = lv_label_create(btn);
        lv_label_set_text(ico_lbl, k_scene_icons[i]);
        lv_obj_set_style_text_color(ico_lbl, lv_color_hex(k_scene_colors[i]), 0);
        lv_obj_set_style_text_font(ico_lbl, &lv_font_montserrat_20, 0);

        lv_obj_t *name_lbl = lv_label_create(btn);
        lv_label_set_text(name_lbl, k_scene_names[i]);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);

        lv_obj_t *pct_lbl = lv_label_create(btn);
        lv_label_set_text(pct_lbl, k_scene_pcts[i]);
        lv_obj_set_style_text_color(pct_lbl, lv_color_hex(k_scene_colors[i]), 0);
        lv_obj_set_style_text_font(pct_lbl, &lv_font_montserrat_14, 0);
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  9. Temperatura tab – arc gauge + target ± + heater/cooling status
 * ╚══════════════════════════════════════════════════════════════════════ */

static void temp_target_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_temp_target_sb) return;
    auto_heater_config_t cfg = auto_heater_get_config();
    cfg.target_temp_c = (float)lv_spinbox_get_value(s_temp_target_sb) / 10.0f;
    auto_heater_set_config(&cfg);
}

static lv_obj_t *make_status_pill(lv_obj_t *parent,
                                   const char *icon_sym, uint32_t icon_col,
                                   const char *label,
                                   lv_obj_t **lbl_out)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_flex_grow(pill, 1);
    lv_obj_set_height(pill, 60);
    lv_obj_set_style_bg_color(pill, lv_color_hex(C_OFF), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_radius(pill, 12, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pill, 8, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ico = lv_label_create(pill);
    lv_label_set_text(ico, icon_sym);
    lv_obj_set_style_text_color(ico, lv_color_hex(icon_col), 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_16, 0);

    lv_obj_t *lbl_name = lv_label_create(pill);
    lv_label_set_text(lbl_name, label);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, 0);

    lv_obj_t *state_lbl = lv_label_create(pill);
    lv_label_set_text(state_lbl, "OFF");
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_16, 0);
    if (lbl_out) *lbl_out = state_lbl;
    return pill;
}

static void build_temperatura_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title_lbl = lv_label_create(tab);
    lv_label_set_text(title_lbl, "TEMPERATURA");
    style_title(title_lbl);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_lbl, LV_PCT(100));

    /* ── Arc + target card ────────────────────────────────────── */
    lv_obj_t *ac = lv_obj_create(tab);
    style_card(ac);
    lv_obj_set_width(ac, LV_PCT(100));
    lv_obj_set_height(ac, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ac, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ac, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Arc side */
    lv_obj_t *arc_cont = lv_obj_create(ac);
    lv_obj_set_style_bg_opa(arc_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc_cont, 0, 0);
    lv_obj_set_style_pad_all(arc_cont, 0, 0);
    lv_obj_set_size(arc_cont, 200, 200);
    lv_obj_remove_flag(arc_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_temp_arc = lv_arc_create(arc_cont);
    lv_obj_set_size(s_temp_arc, 200, 200);
    lv_arc_set_rotation(s_temp_arc, 135);
    lv_arc_set_bg_angles(s_temp_arc, 0, 270);
    lv_arc_set_range(s_temp_arc, 150, 400);
    lv_arc_set_value(s_temp_arc, 250);
    lv_arc_set_mode(s_temp_arc, LV_ARC_MODE_NORMAL);
    lv_obj_remove_flag(s_temp_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_temp_arc, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_temp_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_temp_arc, lv_color_hex(C_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_temp_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_temp_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_temp_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_temp_arc, 0, 0);
    lv_obj_center(s_temp_arc);

    s_temp_big_lbl = lv_label_create(arc_cont);
    lv_label_set_text(s_temp_big_lbl, "--.-\xc2\xb0""C");
    lv_obj_set_style_text_font(s_temp_big_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_temp_big_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(s_temp_big_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_temp_big_lbl);

    lv_obj_t *arc_sub = lv_label_create(arc_cont);
    lv_label_set_text(arc_sub, "ATTUALE");
    lv_obj_set_style_text_font(arc_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(arc_sub, lv_color_hex(C_MUTED), 0);
    lv_obj_align(arc_sub, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* Target side */
    lv_obj_t *tgt = lv_obj_create(ac);
    style_transp(tgt);
    lv_obj_set_size(tgt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tgt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tgt, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(tgt, 10, 0);

    lv_obj_t *tgt_ttl = lv_label_create(tgt);
    lv_label_set_text(tgt_ttl, "TARGET");
    style_muted(tgt_ttl);

    auto_heater_config_t ht = auto_heater_get_config();
    int32_t init_tgt = (int32_t)(ht.target_temp_c * 10.0f + 0.5f);
    s_temp_target_sb = make_sb(tgt, 150, 350, init_tgt, 3, 1);

    lv_obj_t *save_btn = lv_button_create(tgt);
    lv_obj_set_size(save_btn, 120, 48);
    style_btn_primary(save_btn);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Salva");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, temp_target_save_cb, LV_EVENT_CLICKED, NULL);

    /* ── Heater / Cooling status row ────────────────────────────── */
    lv_obj_t *st_row = lv_obj_create(tab);
    style_transp(st_row);
    lv_obj_set_width(st_row, LV_PCT(100));
    lv_obj_set_height(st_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                   LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(st_row, 12, 0);

    s_heater_chip = make_status_pill(st_row,
        LV_SYMBOL_WARNING, C_ERR, "RISCALDATORE", &s_heater_lbl);
    s_cool_chip = make_status_pill(st_row,
        LV_SYMBOL_REFRESH, C_PRIMARY, "RAFFREDDAMENTO", &s_cool_lbl);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  10. Automazioni tab – scrollable list with toggles
 * ╚══════════════════════════════════════════════════════════════════════ */

static void auto_luci_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    led_schedule_config_t cfg = led_schedule_get_config();
    cfg.enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    led_schedule_set_config(&cfg);
}

static void auto_co2_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    co2_config_t cfg = co2_controller_get_config();
    cfg.enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    co2_controller_set_config(&cfg);
}

static void auto_ht_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    auto_heater_config_t cfg = auto_heater_get_config();
    cfg.enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    auto_heater_set_config(&cfg);
}

static void auto_relay_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    relay_controller_set(idx,
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void auto_feed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (feeding_mode_is_active()) {
        feeding_mode_stop();
        if (s_auto_feed_lbl)
            lv_label_set_text(s_auto_feed_lbl, LV_SYMBOL_PAUSE "  Avvia Alimentazione");
        if (s_auto_feed_btn)
            lv_obj_set_style_bg_color(s_auto_feed_btn, lv_color_hex(C_ON_BG), 0);
    } else {
        feeding_mode_start();
        if (s_auto_feed_lbl)
            lv_label_set_text(s_auto_feed_lbl, LV_SYMBOL_STOP "  Ferma Alimentazione");
        if (s_auto_feed_btn)
            lv_obj_set_style_bg_color(s_auto_feed_btn, lv_color_hex(C_ERR_BG), 0);
    }
}

static void build_automazioni_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "AUTOMAZIONI");
    style_title(title);

    led_schedule_config_t sched = led_schedule_get_config();
    co2_config_t          co2   = co2_controller_get_config();
    auto_heater_config_t  ht    = auto_heater_get_config();

    /* Luci schedule time string */
    char luci_time[24];
    snprintf(luci_time, sizeof(luci_time), "%02d:%02d \xe2\x80\x93 %02d:%02d",
             sched.on_hour, sched.on_minute, sched.off_hour, sched.off_minute);

    /* CO2 time derived from LED schedule + offsets */
    int co2_on_min  = sched.on_hour  * 60 + sched.on_minute  - co2.pre_on_min;
    int co2_off_min = sched.off_hour * 60 + sched.off_minute + co2.post_off_min;
    if (co2_on_min  < 0)    co2_on_min  = 0;
    if (co2_off_min > 1439) co2_off_min = 1439;
    char co2_time[24];
    snprintf(co2_time, sizeof(co2_time), "%02d:%02d \xe2\x80\x93 %02d:%02d",
             co2_on_min / 60, co2_on_min % 60,
             co2_off_min / 60, co2_off_min % 60);

    char ht_info[32];
    snprintf(ht_info, sizeof(ht_info), "Target: %.1f\xc2\xb0""C", ht.target_temp_c);

    /* Luci */
    s_auto_luci_sw = make_auto_item(tab,
        LV_SYMBOL_IMAGE, C_YELLOW,
        "Luci", luci_time, "Tutti i giorni",
        sched.enabled, &s_auto_luci_inf);
    lv_obj_add_event_cb(s_auto_luci_sw, auto_luci_sw_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* CO2 */
    s_auto_co2_sw = make_auto_item(tab,
        LV_SYMBOL_REFRESH, C_ON,
        "CO\u2082", co2_time, "Tutti i giorni",
        co2.enabled, &s_auto_co2_inf);
    lv_obj_add_event_cb(s_auto_co2_sw, auto_co2_sw_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Riscaldatore */
    s_auto_ht_sw = make_auto_item(tab,
        LV_SYMBOL_WARNING, C_ERR,
        "Riscaldatore", ht_info, "Termostato auto",
        ht.enabled, &s_auto_ht_inf);
    lv_obj_add_event_cb(s_auto_ht_sw, auto_ht_sw_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Relays */
    relay_state_t rs[RELAY_COUNT];
    relay_controller_get_all(rs);
    static const uint32_t relay_cols[RELAY_COUNT] = {
        C_PRIMARY, 0xF39C12, C_ON, 0xE879F9
    };
    for (int i = 0; i < RELAY_COUNT; i++) {
        char relay_sub[20];
        snprintf(relay_sub, sizeof(relay_sub), "Rel\xc3\xa8 %d", i + 1);
        s_auto_rel_sw[i] = make_auto_item(tab,
            LV_SYMBOL_SETTINGS, relay_cols[i],
            rs[i].name, relay_sub, "",
            rs[i].on, &s_auto_rel_name[i]);
        lv_obj_add_event_cb(s_auto_rel_sw[i], auto_relay_sw_cb,
                            LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)i);
    }

    /* Alimentazione */
    s_auto_feed_btn = lv_button_create(tab);
    lv_obj_set_width(s_auto_feed_btn, LV_PCT(100));
    lv_obj_set_height(s_auto_feed_btn, 60);
    lv_obj_set_style_bg_color(s_auto_feed_btn, lv_color_hex(C_ON_BG), 0);
    lv_obj_set_style_bg_opa(s_auto_feed_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_auto_feed_btn, 0, 0);
    lv_obj_set_style_radius(s_auto_feed_btn, 12, 0);
    s_auto_feed_lbl = lv_label_create(s_auto_feed_btn);
    lv_label_set_text(s_auto_feed_lbl, LV_SYMBOL_PAUSE "  Avvia Alimentazione");
    lv_obj_set_style_text_color(s_auto_feed_lbl, lv_color_hex(C_ON), 0);
    lv_obj_set_style_text_font(s_auto_feed_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_auto_feed_lbl);
    lv_obj_add_event_cb(s_auto_feed_btn, auto_feed_cb, LV_EVENT_CLICKED, NULL);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  11. Dati tab – LVGL chart with Temperatura/Luci/CO₂ selector
 * ╚══════════════════════════════════════════════════════════════════════ */

static const uint32_t k_chart_colors[3] = { C_PRIMARY, C_YELLOW, C_ON };

static void chart_update(int mode)
{
    if (!s_chart || !s_chart_ser) return;
    s_chart_mode = mode;

    /* Update selector button styles */
    for (int i = 0; i < 3; i++) {
        if (!s_chart_sel[i]) continue;
        bool sel = (i == mode);
        lv_obj_set_style_bg_color(s_chart_sel[i],
            lv_color_hex(sel ? k_chart_colors[i] : C_INPUT), 0);
        /* Update label colour inside the button */
        lv_obj_t *lbl = lv_obj_get_child(s_chart_sel[i], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(sel ? C_TEXT : C_MUTED), 0);
    }

    /* Change series colour to match selected channel */
    lv_chart_set_series_color(s_chart, s_chart_ser,
                              lv_color_hex(k_chart_colors[mode]));

    /* Clear series */
    lv_chart_set_all_value(s_chart, s_chart_ser, LV_CHART_POINT_NONE);

    if (mode == 0) {
        /* ── Temperature history (line chart, °C×10) ─────────── */
        lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 180, 320);

        temp_sample_t samples[TEMP_HISTORY_MAX_SAMPLES];
        int count = 0;
        temperature_history_get(samples, &count);

        if (count > 0) {
            int step = (count > CHART_POINTS) ? (count / CHART_POINTS) : 1;
            int n    = count / step;
            if (n > CHART_POINTS) n = CHART_POINTS;
            for (int i = 0; i < n; i++) {
                int src = i * step;
                if (src < count && samples[src].timestamp > 0)
                    lv_chart_set_next_value(s_chart, s_chart_ser,
                        (int32_t)(samples[src].temp_c * 10.0f + 0.5f));
            }
        } else {
            for (int i = 0; i < CHART_POINTS; i++)
                lv_chart_set_next_value(s_chart, s_chart_ser, 250);
        }

    } else if (mode == 1) {
        /* ── Light brightness (bar chart, 0-100%) ────────────── */
        lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

        led_schedule_config_t cfg = led_schedule_get_config();
        int on_slot  = (cfg.on_hour  * 60 + cfg.on_minute)  * CHART_POINTS / 1440;
        int off_slot = (cfg.off_hour * 60 + cfg.off_minute) * CHART_POINTS / 1440;
        int br_pct   = cfg.brightness * 100 / 255;

        for (int i = 0; i < CHART_POINTS; i++) {
            int v = (cfg.enabled && on_slot < off_slot &&
                     i >= on_slot && i < off_slot) ? br_pct : 0;
            lv_chart_set_next_value(s_chart, s_chart_ser, v);
        }

    } else {
        /* ── CO2 active window (bar chart, 0-100%) ───────────── */
        lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

        led_schedule_config_t sched = led_schedule_get_config();
        co2_config_t          co2   = co2_controller_get_config();
        int on_min  = sched.on_hour  * 60 + sched.on_minute  - co2.pre_on_min;
        int off_min = sched.off_hour * 60 + sched.off_minute + co2.post_off_min;
        if (on_min  < 0)    on_min  = 0;
        if (off_min > 1439) off_min = 1439;
        int on_slot  = on_min  * CHART_POINTS / 1440;
        int off_slot = off_min * CHART_POINTS / 1440;

        for (int i = 0; i < CHART_POINTS; i++) {
            int v = (co2.enabled && on_slot < off_slot &&
                     i >= on_slot && i < off_slot) ? 80 : 0;
            lv_chart_set_next_value(s_chart, s_chart_ser, v);
        }
    }

    lv_chart_refresh(s_chart);
}

static void chart_mode_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int mode = (int)(uintptr_t)lv_event_get_user_data(e);
    chart_update(mode);
}

static void build_dati_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "DATI");
    style_title(title);

    /* ── Mode selector bar ────────────────────────────────────── */
    lv_obj_t *sel = lv_obj_create(tab);
    style_transp(sel);
    lv_obj_set_width(sel, LV_PCT(100));
    lv_obj_set_height(sel, 46);
    lv_obj_set_flex_flow(sel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(sel, 8, 0);

    static const char *sel_labels[3] = { "TEMPERATURA", "LUCI", "CO\u2082" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(sel);
        s_chart_sel[i] = btn;
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 46);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(i == 0 ? k_chart_colors[0] : C_INPUT), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, sel_labels[i]);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(i == 0 ? C_TEXT : C_MUTED), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, chart_mode_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    /* ── Chart card ───────────────────────────────────────────── */
    lv_obj_t *cc = lv_obj_create(tab);
    style_card(cc);
    lv_obj_set_width(cc, LV_PCT(100));
    lv_obj_set_flex_grow(cc, 1);
    lv_obj_set_style_min_height(cc, 300, 0);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cc, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cc, 8, 0);

    s_chart = lv_chart_create(cc);
    lv_obj_set_width(s_chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_chart, 1);
    lv_obj_set_style_min_height(s_chart, 260, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 180, 320);
    lv_chart_set_div_line_count(s_chart, 4, 6);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 4, 4, LV_PART_INDICATOR);

    s_chart_ser = lv_chart_add_series(s_chart, lv_color_hex(C_PRIMARY),
                                      LV_CHART_AXIS_PRIMARY_Y);

    /* X-axis time labels */
    lv_obj_t *x_row = lv_obj_create(cc);
    style_transp(x_row);
    lv_obj_set_width(x_row, LV_PCT(100));
    lv_obj_set_height(x_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(x_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(x_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *x_lbl[5] = { "-24h", "-18h", "-12h", "-6h", "0h" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *xl = lv_label_create(x_row);
        lv_label_set_text(xl, x_lbl[i]);
        style_muted(xl);
    }

    /* Initial data population */
    chart_update(0);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  12. Alarm overlay
 * ╚══════════════════════════════════════════════════════════════════════ */

static void alarm_ok_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_alarm_overlay) {
        lv_obj_del(s_alarm_overlay);
        s_alarm_overlay = NULL;
    }
    if (s_status_ok_chip)
        lv_obj_set_style_bg_color(s_status_ok_chip, lv_color_hex(C_ON_BG), 0);
    if (s_status_ok_lbl)
        lv_label_set_text(s_status_ok_lbl, LV_SYMBOL_OK "  OK");
}

static void alarm_dismiss_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_alarm_overlay) {
        lv_obj_del(s_alarm_overlay);
        s_alarm_overlay = NULL;
    }
}

void display_ui_show_alarm(const char *msg, const char *detail)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_alarm_overlay) {
        lv_obj_del(s_alarm_overlay);
        s_alarm_overlay = NULL;
    }

    /* Update status bar chip to ALLARME */
    if (s_status_ok_chip)
        lv_obj_set_style_bg_color(s_status_ok_chip, lv_color_hex(C_ERR_BG), 0);
    if (s_status_ok_lbl)
        lv_label_set_text(s_status_ok_lbl, LV_SYMBOL_WARNING "  ALLARME");

    /* Dim overlay */
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_alarm_overlay = ov;
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    /* Alarm card */
    lv_obj_t *card = lv_obj_create(ov);
    lv_obj_set_size(card, 580, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_ERR_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ERR), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ico = lv_label_create(card);
    lv_label_set_text(ico, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(ico, lv_color_hex(C_ERR), 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_28, 0);

    lv_obj_t *msg_lbl = lv_label_create(card);
    lv_label_set_text(msg_lbl, msg ? msg : "ALLARME");
    lv_obj_set_style_text_color(msg_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg_lbl, LV_PCT(100));
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);

    if (detail && detail[0]) {
        lv_obj_t *det = lv_label_create(card);
        lv_label_set_text(det, detail);
        lv_obj_set_style_text_color(det, lv_color_hex(C_MUTED), 0);
        lv_obj_set_style_text_font(det, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(det, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(det, LV_PCT(100));
    }

    lv_obj_t *brow = lv_obj_create(card);
    style_transp(brow);
    lv_obj_set_width(brow, LV_PCT(100));
    lv_obj_set_height(brow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(brow, 14, 0);

    lv_obj_t *b_dis = lv_button_create(brow);
    lv_obj_set_flex_grow(b_dis, 1);
    lv_obj_set_height(b_dis, 60);
    style_btn_dark(b_dis);
    lv_obj_t *l_dis = lv_label_create(b_dis);
    lv_label_set_text(l_dis, "DISATTIVA ALLARME");
    lv_obj_set_style_text_font(l_dis, &lv_font_montserrat_16, 0);
    lv_obj_center(l_dis);
    lv_obj_add_event_cb(b_dis, alarm_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_ok = lv_button_create(brow);
    lv_obj_set_flex_grow(b_ok, 1);
    lv_obj_set_height(b_ok, 60);
    style_btn_red(b_ok);
    lv_obj_t *l_ok = lv_label_create(b_ok);
    lv_label_set_text(l_ok, "OK");
    lv_obj_set_style_text_font(l_ok, &lv_font_montserrat_20, 0);
    lv_obj_center(l_ok);
    lv_obj_add_event_cb(b_ok, alarm_ok_cb, LV_EVENT_CLICKED, NULL);

    xSemaphoreGive(s_mutex);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  13. Status bar
 * ╚══════════════════════════════════════════════════════════════════════ */

static void build_status_bar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_W, STATUS_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 20, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Time (left) */
    s_status_time_lbl = lv_label_create(bar);
    lv_label_set_text(s_status_time_lbl, "--:--");
    lv_obj_set_style_text_color(s_status_time_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_status_time_lbl, &lv_font_montserrat_20, 0);

    /* Centre group: temp + OK chip */
    lv_obj_t *cg = lv_obj_create(bar);
    style_transp(cg);
    lv_obj_set_size(cg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cg, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cg, 10, 0);

    lv_obj_t *temp_icon = lv_label_create(cg);
    lv_label_set_text(temp_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(temp_icon, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(temp_icon, &lv_font_montserrat_16, 0);

    s_status_temp_lbl = lv_label_create(cg);
    lv_label_set_text(s_status_temp_lbl, "--.-\xc2\xb0""C");
    lv_obj_set_style_text_color(s_status_temp_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_status_temp_lbl, &lv_font_montserrat_16, 0);

    s_status_ok_chip = make_badge(cg, LV_SYMBOL_OK "  OK",
                                  C_ON_BG, C_ON, &s_status_ok_lbl);

    /* WiFi icon (right) */
    lv_obj_t *wifi_lbl = lv_label_create(bar);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_20, 0);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  14. Data-refresh LVGL timer (runs inside LVGL task – no extra lock)
 * ╚══════════════════════════════════════════════════════════════════════ */

static void ui_refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* ── Temperature ─────────────────────────────────────────── */
    float temp_c = 0.0f;
    bool  tok    = temperature_sensor_get(&temp_c);
    bool  tok_ok = tok && (temp_c >= 24.0f && temp_c <= 28.0f);

    /* Status bar */
    if (s_status_temp_lbl) {
        if (tok)
            lv_label_set_text_fmt(s_status_temp_lbl, "%.1f\xc2\xb0""C", temp_c);
        else
            lv_label_set_text(s_status_temp_lbl, "--.-\xc2\xb0""C");
    }

    /* Home card – Temperatura */
    if (s_home_temp_val) {
        if (tok)
            lv_label_set_text_fmt(s_home_temp_val, "%.1f\xc2\xb0""C", temp_c);
        else
            lv_label_set_text(s_home_temp_val, "--.-\xc2\xb0""C");
        lv_obj_set_style_text_color(s_home_temp_val,
            lv_color_hex(tok_ok ? C_ON : (tok ? C_ORANGE : C_MUTED)), 0);
    }
    if (s_home_temp_badge) {
        lv_obj_t *lbl = lv_obj_get_child(s_home_temp_badge, 0);
        if (lbl) {
            if (!tok) {
                lv_label_set_text(lbl, LV_SYMBOL_WARNING "  Errore");
                lv_obj_set_style_text_color(lbl, lv_color_hex(C_ERR), 0);
                lv_obj_set_style_bg_color(s_home_temp_badge, lv_color_hex(C_ERR_BG), 0);
            } else if (tok_ok) {
                lv_label_set_text(lbl, LV_SYMBOL_OK "  OK");
                lv_obj_set_style_text_color(lbl, lv_color_hex(C_ON), 0);
                lv_obj_set_style_bg_color(s_home_temp_badge, lv_color_hex(C_ON_BG), 0);
            } else {
                lv_label_set_text(lbl, LV_SYMBOL_WARNING "  Attenzione");
                lv_obj_set_style_text_color(lbl, lv_color_hex(C_ORANGE), 0);
                lv_obj_set_style_bg_color(s_home_temp_badge, lv_color_hex(0x3D2200), 0);
            }
        }
    }

    /* Temperatura tab arc + big label */
    if (s_temp_arc) {
        int32_t av = tok ? (int32_t)(temp_c * 10.0f + 0.5f) : 250;
        if (av < 150) av = 150;
        if (av > 400) av = 400;
        lv_arc_set_value(s_temp_arc, av);
        lv_obj_set_style_arc_color(s_temp_arc,
            lv_color_hex(tok_ok ? C_ON : (tok ? C_ORANGE : C_PRIMARY)),
            LV_PART_INDICATOR);
    }
    if (s_temp_big_lbl) {
        if (tok)
            lv_label_set_text_fmt(s_temp_big_lbl, "%.1f\xc2\xb0""C", temp_c);
        else
            lv_label_set_text(s_temp_big_lbl, "--.-\xc2\xb0""C");
        lv_obj_set_style_text_color(s_temp_big_lbl,
            lv_color_hex(tok_ok ? C_ON : (tok ? C_ORANGE : C_MUTED)), 0);
    }

    /* Heater status */
    auto_heater_config_t ht = auto_heater_get_config();
    bool heater_on = ht.enabled && relay_controller_get(ht.relay_index);
    if (s_heater_chip && s_heater_lbl) {
        lv_obj_set_style_bg_color(s_heater_chip,
            lv_color_hex(heater_on ? C_ERR_BG : C_OFF), 0);
        lv_label_set_text(s_heater_lbl, heater_on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_heater_lbl,
            lv_color_hex(heater_on ? C_ERR : C_MUTED), 0);
    }
    /* Cooling is not implemented – always OFF */
    if (s_cool_chip && s_cool_lbl) {
        lv_obj_set_style_bg_color(s_cool_chip, lv_color_hex(C_OFF), 0);
        lv_label_set_text(s_cool_lbl, "OFF");
        lv_obj_set_style_text_color(s_cool_lbl, lv_color_hex(C_MUTED), 0);
    }

    /* ── LED (Luci) ──────────────────────────────────────────── */
    bool led_on  = led_controller_is_on();
    int  led_pct = led_on ? (led_controller_get_brightness() * 100 / 255) : 0;

    /* Home card */
    if (s_home_led_val)
        lv_label_set_text_fmt(s_home_led_val, "%d%%", led_pct);
    if (s_home_led_badge) {
        lv_obj_t *lbl = lv_obj_get_child(s_home_led_badge, 0);
        if (lbl) {
            lv_label_set_text(lbl, led_on ? "ON" : "OFF");
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(led_on ? C_ON : C_MUTED), 0);
            lv_obj_set_style_bg_color(s_home_led_badge,
                lv_color_hex(led_on ? C_ON_BG : C_OFF), 0);
        }
    }

    /* Luci tab switch + brightness */
    if (s_luci_sw) {
        if (led_on) lv_obj_add_state(s_luci_sw, LV_STATE_CHECKED);
        else        lv_obj_remove_state(s_luci_sw, LV_STATE_CHECKED);
    }
    if (s_luci_br_sl)
        lv_slider_set_value(s_luci_br_sl, led_pct, LV_ANIM_OFF);
    if (s_luci_br_lbl)
        lv_label_set_text_fmt(s_luci_br_lbl, "%d%%", led_pct);

    /* ── CO2 ─────────────────────────────────────────────────── */
    co2_config_t co2 = co2_controller_get_config();
    bool co2_relay_on = co2.enabled && relay_controller_get(co2.relay_index);

    if (s_home_co2_val) {
        lv_label_set_text(s_home_co2_val, co2_relay_on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_home_co2_val,
            lv_color_hex(co2_relay_on ? C_ON : C_MUTED), 0);
    }
    if (s_home_co2_sub) {
        led_schedule_config_t sc = led_schedule_get_config();
        int off_min = sc.off_hour * 60 + sc.off_minute + co2.post_off_min;
        if (off_min > 1439) off_min = 1439;
        lv_label_set_text_fmt(s_home_co2_sub, "Terminazione: %02d:%02d",
                              off_min / 60, off_min % 60);
    }

    /* Automazioni tab switches */
    led_schedule_config_t sched_now = led_schedule_get_config();
    if (s_auto_luci_sw) {
        if (sched_now.enabled) lv_obj_add_state(s_auto_luci_sw, LV_STATE_CHECKED);
        else                   lv_obj_remove_state(s_auto_luci_sw, LV_STATE_CHECKED);
    }
    co2_config_t co2_now = co2_controller_get_config();
    if (s_auto_co2_sw) {
        if (co2_now.enabled) lv_obj_add_state(s_auto_co2_sw, LV_STATE_CHECKED);
        else                 lv_obj_remove_state(s_auto_co2_sw, LV_STATE_CHECKED);
    }
    auto_heater_config_t ht_now = auto_heater_get_config();
    if (s_auto_ht_sw) {
        if (ht_now.enabled) lv_obj_add_state(s_auto_ht_sw, LV_STATE_CHECKED);
        else                lv_obj_remove_state(s_auto_ht_sw, LV_STATE_CHECKED);
    }

    /* ── Relay states (Automazioni) ─────────────────────────── */
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool on = relay_controller_get(i);
        if (s_auto_rel_sw[i]) {
            if (on) lv_obj_add_state(s_auto_rel_sw[i], LV_STATE_CHECKED);
            else    lv_obj_remove_state(s_auto_rel_sw[i], LV_STATE_CHECKED);
        }
        if (s_auto_rel_name[i]) {
            char name[RELAY_NAME_MAX];
            relay_controller_get_name(i, name, sizeof(name));
            lv_label_set_text(s_auto_rel_name[i], name);
        }
    }

    /* ── Feeding mode ────────────────────────────────────────── */
    if (s_auto_feed_lbl && s_auto_feed_btn) {
        bool feeding = feeding_mode_is_active();
        int  rem_s   = feeding_mode_get_remaining_s();
        if (feeding) {
            int m = rem_s / 60, s = rem_s % 60;
            lv_label_set_text_fmt(s_auto_feed_lbl,
                LV_SYMBOL_STOP "  Ferma (%02d:%02d)", m, s);
            lv_obj_set_style_bg_color(s_auto_feed_btn, lv_color_hex(C_ERR_BG), 0);
        } else {
            lv_label_set_text(s_auto_feed_lbl,
                LV_SYMBOL_PAUSE "  Avvia Alimentazione");
            lv_obj_set_style_bg_color(s_auto_feed_btn, lv_color_hex(C_ON_BG), 0);
        }
    }

    /* ── Status bar: time + wifi ─────────────────────────────── */
    if (s_status_time_lbl) {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        if (ti.tm_year >= (2024 - 1900))
            lv_label_set_text_fmt(s_status_time_lbl, "%02d:%02d",
                                  ti.tm_hour, ti.tm_min);
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  15. Build UI and entry point
 * ╚══════════════════════════════════════════════════════════════════════ */

static void build_ui(void)
{
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    /* Status bar: fixed 48 px strip at the very top */
    build_status_bar();

    /* TabView: fills the rest of the screen below the status bar */
    s_tv = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_position(s_tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tv, TABBAR_H);
    lv_obj_set_pos(s_tv, 0, STATUS_H);
    lv_obj_set_size(s_tv, LCD_W, LCD_H - STATUS_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);

    /* Style the tab button bar */
    lv_obj_t *tb = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_bg_color(tb, lv_color_hex(C_BAR_BG), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(tb, 1, 0);

    /* Default tab: muted */
    lv_obj_set_style_text_color(tb, lv_color_hex(C_MUTED),
                                LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tb, LV_OPA_TRANSP,
                             LV_PART_ITEMS | LV_STATE_DEFAULT);

    /* Active tab: white text + blue top border underline */
    lv_obj_set_style_text_color(tb, lv_color_hex(C_TEXT),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tb, LV_OPA_TRANSP,
                             LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_TOP,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_PRIMARY),
                                   LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tb, 3,
                                   LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tb, &lv_font_montserrat_14, LV_PART_ITEMS);

    /* Build each tab */
    lv_obj_t *tab_home  = lv_tabview_add_tab(s_tv, LV_SYMBOL_HOME   "  Home");
    lv_obj_t *tab_luci  = lv_tabview_add_tab(s_tv, LV_SYMBOL_IMAGE  "  Luci");
    lv_obj_t *tab_temp  = lv_tabview_add_tab(s_tv, LV_SYMBOL_WARNING "  Temp");
    lv_obj_t *tab_auto  = lv_tabview_add_tab(s_tv, LV_SYMBOL_SETTINGS " Auto");
    lv_obj_t *tab_dati  = lv_tabview_add_tab(s_tv, LV_SYMBOL_LIST   "  Dati");

    build_home_tab(tab_home);
    build_luci_tab(tab_luci);
    build_temperatura_tab(tab_temp);
    build_automazioni_tab(tab_auto);
    build_dati_tab(tab_dati);

    /* Periodic data refresh (2 s) */
    lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);

    ESP_LOGI(TAG, "UI built – 5 tabs (Home/Luci/Temp/Auto/Dati)");
}

esp_err_t display_ui_init(void)
{
    ESP_LOGI(TAG, "Initialising display …");

    esp_err_t ret = lcd_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD HW init failed (0x%x) – display disabled", ret);
        return ret;
    }

    ret = touch_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch HW init failed (0x%x) – touch disabled", ret);
        /* non-fatal: display still works without touch */
    }

    lv_init();

    size_t buf_sz = (size_t)LCD_W * LVGL_BUF_LINES * sizeof(lv_color16_t);
    void  *buf1   = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void  *buf2   = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        free(buf1);
        free(buf2);
        return ESP_ERR_NO_MEM;
    }

    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_disp, s_panel);

    if (s_touch) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, lvgl_touch_cb);
        lv_indev_set_user_data(indev, s_touch);
    }

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000ULL));

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    build_ui();
    xSemaphoreGive(s_mutex);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl",
                            LVGL_TASK_STACK, NULL,
                            LVGL_TASK_PRIO, NULL, 1);

    ESP_LOGI(TAG, "Display UI ready – 720×720 MIPI-DSI, new IoT dashboard");
    return ESP_OK;
}

#endif /* CONFIG_DISPLAY_ENABLED */
