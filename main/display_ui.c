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
#include "led_controller.h"
#include "led_schedule.h"
#include "relay_controller.h"
#include "auto_heater.h"
#include "co2_controller.h"
#include "timezone_manager.h"
#include "wifi_manager.h"

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
#define LVGL_TASK_STACK     (12 * 1024)
#define LVGL_TASK_PRIO      2
#define LVGL_BUF_LINES      40            /* partial-render buffer height */
#define UI_REFRESH_MS       2000          /* data polling interval        */

/* ── Colour palette (matches web UI) ───────────────────────────────── */
#define C_BG        0x0b1121
#define C_CARD      0x131c31
#define C_BORDER    0x1a2540
#define C_INPUT     0x1e293b
#define C_ACCENT    0x38bdf8
#define C_TEXT      0xe2e8f0
#define C_MUTED     0x94a3b8
#define C_ON        0x4ade80
#define C_ON_BG     0x166534
#define C_OFF       0x475569
#define C_ERR       0xf87171
#define C_ERR_BG    0x7f1d1d
#define C_WARN      0xfbbf24

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  2. Static state – hardware handles + widget references
 * ╚══════════════════════════════════════════════════════════════════════ */

static esp_lcd_panel_handle_t  s_panel   = NULL;
static esp_lcd_touch_handle_t  s_touch   = NULL;
static lv_display_t           *s_disp    = NULL;
static SemaphoreHandle_t       s_mutex   = NULL;

/* ─ Home tab ──────────────────────────────────────────────────────── */
static lv_obj_t *s_temp_lbl       = NULL;   /* e.g. "24.5°C" */
static lv_obj_t *s_temp_status    = NULL;   /* sensor OK / Errore */
static lv_obj_t *s_led_swatch     = NULL;   /* colour preview square */
static lv_obj_t *s_led_state_lbl  = NULL;   /* "Accese 75%" */
static lv_obj_t *s_home_relay_sw[RELAY_COUNT]; /* quick relay toggles */

/* ─ LED tab ───────────────────────────────────────────────────────── */
static lv_obj_t *s_led_sw         = NULL;
static lv_obj_t *s_led_br_sl      = NULL;
static lv_obj_t *s_led_br_lbl     = NULL;
static lv_obj_t *s_led_r          = NULL;
static lv_obj_t *s_led_g          = NULL;
static lv_obj_t *s_led_b          = NULL;
static lv_obj_t *s_led_preview    = NULL;
static lv_obj_t *s_sched_sw       = NULL;
static lv_obj_t *s_son_h          = NULL;
static lv_obj_t *s_son_m          = NULL;
static lv_obj_t *s_soff_h         = NULL;
static lv_obj_t *s_soff_m         = NULL;
static lv_obj_t *s_sramp          = NULL;
static lv_obj_t *s_sbr            = NULL;
static lv_obj_t *s_sr             = NULL;
static lv_obj_t *s_sg             = NULL;
static lv_obj_t *s_sb             = NULL;
static lv_obj_t *s_pause_sw       = NULL;
static lv_obj_t *s_pstart_h       = NULL;
static lv_obj_t *s_pstart_m       = NULL;
static lv_obj_t *s_pend_h         = NULL;
static lv_obj_t *s_pend_m         = NULL;
static lv_obj_t *s_pbr            = NULL;

/* ─ Relay tab ─────────────────────────────────────────────────────── */
static lv_obj_t *s_rel_sw[RELAY_COUNT];       /* ON/OFF switch per relay */
static lv_obj_t *s_rel_name_lbl[RELAY_COUNT]; /* label showing relay name */

/* ─ Config tab ────────────────────────────────────────────────────── */
static lv_obj_t *s_ht_en      = NULL;
static lv_obj_t *s_ht_relay   = NULL;
static lv_obj_t *s_ht_target  = NULL;
static lv_obj_t *s_ht_hyst    = NULL;
static lv_obj_t *s_co2_en     = NULL;
static lv_obj_t *s_co2_relay  = NULL;
static lv_obj_t *s_co2_pre    = NULL;
static lv_obj_t *s_co2_post   = NULL;
static lv_obj_t *s_tz_dd      = NULL;

/* ─ Info tab ──────────────────────────────────────────────────────── */
static lv_obj_t *s_info_wifi   = NULL;
static lv_obj_t *s_info_ip     = NULL;
static lv_obj_t *s_info_heap   = NULL;
static lv_obj_t *s_info_uptime = NULL;
static lv_obj_t *s_info_time   = NULL;

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  3. Hardware initialisation
 * ╚══════════════════════════════════════════════════════════════════════ */

/* DPI config stored statically so the panel can reference it after init */
static esp_lcd_dpi_panel_config_t s_dpi_cfg = {
    .virtual_channel    = 0,
    .dpi_clock_freq_mhz = DPI_CLK_MHZ,
    .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
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
    esp_lcd_touch_config_t tp_cfg = {
        .x_max         = LCD_W,
        .y_max         = LCD_H,
        .rst_gpio_num  = GPIO_NUM_NC,
        .int_gpio_num  = GPIO_NUM_NC,
        .levels        = { .reset = 0, .interrupt = 0 },
        .flags         = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(i2c_bus, &tp_cfg, &s_touch),
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
    uint16_t x[1], y[1], str[1];
    uint8_t  cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, str, &cnt, 1);
    if (pressed && cnt > 0) {
        data->point.x = (int32_t)x[0];
        data->point.y = (int32_t)y[0];
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

static void style_bg(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_transp(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_section_lbl(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
}

static void style_muted_lbl(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
}

static void style_btn_accent(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0ea5e9), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

static void style_btn_green(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ON_BG), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_ON), 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

static void style_btn_red(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ERR_BG), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_ERR), 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

static void style_btn_dark(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_BORDER), 0);
}

static void style_switch(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_OFF),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ON),
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_TEXT),
                               LV_PART_KNOB | LV_STATE_DEFAULT);
}

static void style_slider(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ACCENT),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_ACCENT),
                               LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_height(o, 8);
    lv_obj_set_style_pad_ver(o, 12, LV_PART_MAIN); /* bigger tap area */
}

static void style_spinbox(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_pad_all(o, 4, 0);
    /* hide the built-in cursor */
    lv_obj_set_style_border_width(o, 0, LV_PART_CURSOR);
}

static void style_tab_page(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_set_style_pad_gap(o, 10, 0);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  6. Widget helpers
 * ╚══════════════════════════════════════════════════════════════════════ */

/* Spinbox +/- callbacks (shared by all spinboxes) */
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

/*
 * Create [−][spinbox][+] in a transparent flex row.
 * Returns the spinbox handle.  The row is a child of @p parent.
 */
static lv_obj_t *make_sb(lv_obj_t *parent,
                          int32_t  min_v,  int32_t  max_v,
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

    /* spinbox first so callbacks can reference it */
    lv_obj_t *sb = lv_spinbox_create(row);
    lv_spinbox_set_range(sb, min_v, max_v);
    lv_spinbox_set_digit_format(sb, digits, sep);
    lv_spinbox_set_value(sb, init_v);
    style_spinbox(sb);

    /* − button */
    lv_obj_t *bm = lv_button_create(row);
    lv_obj_set_size(bm, 40, 40);
    lv_obj_set_style_radius(bm, 20, 0);
    style_btn_dark(bm);
    lv_obj_t *lm = lv_label_create(bm);
    lv_label_set_text(lm, LV_SYMBOL_MINUS);
    lv_obj_center(lm);
    lv_obj_add_event_cb(bm, sb_dec_cb, LV_EVENT_ALL, sb);

    /* + button */
    lv_obj_t *bp = lv_button_create(row);
    lv_obj_set_size(bp, 40, 40);
    lv_obj_set_style_radius(bp, 20, 0);
    style_btn_dark(bp);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, LV_SYMBOL_PLUS);
    lv_obj_center(lp);
    lv_obj_add_event_cb(bp, sb_inc_cb, LV_EVENT_ALL, sb);

    /* reorder: put − before spinbox, + after */
    lv_obj_move_to_index(bm, 0);

    return sb;
}

/*
 * HH:MM time-picker row: [−] HH [+] : [−] MM [+]
 * Stores handles to hour/minute spinboxes in *h_out / *m_out.
 */
static void make_time_row(lv_obj_t *parent,
                          lv_obj_t **h_out, lv_obj_t **m_out,
                          uint8_t init_h, uint8_t init_m)
{
    lv_obj_t *row = lv_obj_create(parent);
    style_transp(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 0, 0);

    *h_out = make_sb(row, 0, 23, init_h, 2, 0);

    lv_obj_t *col = lv_label_create(row);
    lv_label_set_text(col, " : ");
    lv_obj_set_style_text_color(col, lv_color_hex(C_MUTED), 0);

    *m_out = make_sb(row, 0, 59, init_m, 2, 0);
}

/* Row container (flex, no bg) */
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

/* Label + value label side by side */
static lv_obj_t *make_kv_row(lv_obj_t *parent,
                               const char *key, lv_obj_t **val_out)
{
    lv_obj_t *row = make_row(parent);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    style_muted_lbl(k);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);

    if (val_out) *val_out = v;
    return row;
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  7. Home tab
 * ╚══════════════════════════════════════════════════════════════════════ */

/* Quick on/off buttons on Home tab */
static void home_led_on_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    led_controller_fade_on((uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000);
}
static void home_led_off_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    led_controller_fade_off((uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000);
}

/* Home tab relay toggle */
static void home_relay_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    relay_controller_set(idx, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void build_home_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    /* ── Temperature card ─────────────────────────────────────── */
    lv_obj_t *tc = lv_obj_create(tab);
    style_bg(tc);
    lv_obj_set_width(tc, LV_PCT(100));
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tc, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(tc, 6, 0);

    s_temp_lbl = lv_label_create(tc);
    lv_label_set_text(s_temp_lbl, "--.-°C");
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_align(s_temp_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_temp_status = lv_label_create(tc);
    lv_label_set_text(s_temp_status, "Sensore: --");
    style_muted_lbl(s_temp_status);

    /* ── LED status card ──────────────────────────────────────── */
    lv_obj_t *lc = lv_obj_create(tab);
    style_bg(lc);
    lv_obj_set_width(lc, LV_PCT(100));
    lv_obj_set_height(lc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(lc, 8, 0);

    lv_obj_t *lhdr = make_row(lc);

    lv_obj_t *ltitle = lv_label_create(lhdr);
    lv_label_set_text(ltitle, "Illuminazione");
    style_section_lbl(ltitle);

    s_led_state_lbl = lv_label_create(lhdr);
    lv_label_set_text(s_led_state_lbl, "--");
    lv_obj_set_style_text_color(s_led_state_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_led_state_lbl, &lv_font_montserrat_14, 0);

    /* colour swatch */
    s_led_swatch = lv_obj_create(lc);
    lv_obj_set_size(s_led_swatch, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(s_led_swatch, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_bg_opa(s_led_swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_led_swatch, 0, 0);
    lv_obj_set_style_radius(s_led_swatch, 6, 0);
    lv_obj_remove_flag(s_led_swatch, LV_OBJ_FLAG_SCROLLABLE);

    /* On / Off buttons */
    lv_obj_t *lbtns = make_row(lc);
    lv_obj_set_style_pad_gap(lbtns, 10, 0);

    lv_obj_t *bon = lv_button_create(lbtns);
    lv_obj_set_flex_grow(bon, 1);
    lv_obj_set_height(bon, 52);
    style_btn_green(bon);
    lv_obj_t *lon = lv_label_create(bon);
    lv_label_set_text(lon, "Accendi");
    lv_obj_center(lon);
    lv_obj_add_event_cb(bon, home_led_on_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *boff = lv_button_create(lbtns);
    lv_obj_set_flex_grow(boff, 1);
    lv_obj_set_height(boff, 52);
    style_btn_red(boff);
    lv_obj_t *loff = lv_label_create(boff);
    lv_label_set_text(loff, "Spegni");
    lv_obj_center(loff);
    lv_obj_add_event_cb(boff, home_led_off_cb, LV_EVENT_CLICKED, NULL);

    /* ── Relay grid card ──────────────────────────────────────── */
    lv_obj_t *rc = lv_obj_create(tab);
    style_bg(rc);
    lv_obj_set_width(rc, LV_PCT(100));
    lv_obj_set_height(rc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(rc, 8, 0);

    lv_obj_t *rh = lv_label_create(rc);
    lv_label_set_text(rh, "Relè");
    style_section_lbl(rh);

    /* 2×2 grid */
    for (int row = 0; row < 2; row++) {
        lv_obj_t *grid_row = make_row(rc);
        lv_obj_set_style_pad_gap(grid_row, 10, 0);
        for (int col = 0; col < 2; col++) {
            int idx = row * 2 + col;
            char name[RELAY_NAME_MAX];
            relay_controller_get_name(idx, name, sizeof(name));

            lv_obj_t *btn = lv_obj_create(grid_row);
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_height(btn, 64);
            style_bg(btn);
            lv_obj_set_style_pad_all(btn, 8, 0);
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                       LV_FLEX_ALIGN_CENTER,
                                       LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(btn, 4, 0);

            lv_obj_t *nl = lv_label_create(btn);
            lv_label_set_text(nl, name);
            lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(nl, lv_color_hex(C_TEXT), 0);
            lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nl, LV_PCT(100));

            lv_obj_t *sw = lv_switch_create(btn);
            style_switch(sw);
            if (relay_controller_get(idx))
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, home_relay_sw_cb,
                                LV_EVENT_VALUE_CHANGED,
                                (void *)(uintptr_t)idx);
            s_home_relay_sw[idx] = sw;
        }
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  8. LED tab
 * ╚══════════════════════════════════════════════════════════════════════ */

static void update_led_preview(void)
{
    if (!s_led_preview) return;
    uint8_t r = (uint8_t)lv_slider_get_value(s_led_r);
    uint8_t g = (uint8_t)lv_slider_get_value(s_led_g);
    uint8_t b = (uint8_t)lv_slider_get_value(s_led_b);
    lv_obj_set_style_bg_color(s_led_preview, lv_color_make(r, g, b), 0);
}

static void led_br_slider_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (s_led_br_lbl)
        lv_label_set_text_fmt(s_led_br_lbl, "%" PRId32 "%%",
                              v * 100 / 255);
}

static void led_color_slider_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    update_led_preview();
}

static void led_apply_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t r  = (uint8_t)lv_slider_get_value(s_led_r);
    uint8_t g  = (uint8_t)lv_slider_get_value(s_led_g);
    uint8_t b  = (uint8_t)lv_slider_get_value(s_led_b);
    uint8_t br = (uint8_t)lv_slider_get_value(s_led_br_sl);
    led_controller_set_color(r, g, b);
    led_controller_set_brightness(br);
    uint32_t ramp = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
    if (lv_obj_has_state(s_led_sw, LV_STATE_CHECKED))
        led_controller_fade_on(ramp);
    else
        led_controller_fade_off(ramp);
}

static void sched_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    led_schedule_config_t cfg = led_schedule_get_config();
    cfg.enabled          = lv_obj_has_state(s_sched_sw, LV_STATE_CHECKED);
    cfg.on_hour          = (uint8_t)lv_spinbox_get_value(s_son_h);
    cfg.on_minute        = (uint8_t)lv_spinbox_get_value(s_son_m);
    cfg.off_hour         = (uint8_t)lv_spinbox_get_value(s_soff_h);
    cfg.off_minute       = (uint8_t)lv_spinbox_get_value(s_soff_m);
    cfg.ramp_duration_min= (uint16_t)lv_spinbox_get_value(s_sramp);
    cfg.brightness       = (uint8_t)lv_slider_get_value(s_sbr);
    cfg.red              = (uint8_t)lv_slider_get_value(s_sr);
    cfg.green            = (uint8_t)lv_slider_get_value(s_sg);
    cfg.blue             = (uint8_t)lv_slider_get_value(s_sb);
    cfg.pause_enabled    = lv_obj_has_state(s_pause_sw, LV_STATE_CHECKED);
    cfg.pause_start_hour = (uint8_t)lv_spinbox_get_value(s_pstart_h);
    cfg.pause_start_minute=(uint8_t)lv_spinbox_get_value(s_pstart_m);
    cfg.pause_end_hour   = (uint8_t)lv_spinbox_get_value(s_pend_h);
    cfg.pause_end_minute = (uint8_t)lv_spinbox_get_value(s_pend_m);
    cfg.pause_brightness = (uint8_t)lv_slider_get_value(s_pbr);
    led_schedule_set_config(&cfg);
}

static void preset_load_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    led_preset_load(slot);
    /* refresh LED tab controls from new schedule */
    led_schedule_config_t cfg = led_schedule_get_config();
    lv_spinbox_set_value(s_son_h,  cfg.on_hour);
    lv_spinbox_set_value(s_son_m,  cfg.on_minute);
    lv_spinbox_set_value(s_soff_h, cfg.off_hour);
    lv_spinbox_set_value(s_soff_m, cfg.off_minute);
    lv_spinbox_set_value(s_sramp,  cfg.ramp_duration_min);
    lv_slider_set_value(s_sbr, cfg.brightness, LV_ANIM_OFF);
    lv_slider_set_value(s_sr,  cfg.red,        LV_ANIM_OFF);
    lv_slider_set_value(s_sg,  cfg.green,      LV_ANIM_OFF);
    lv_slider_set_value(s_sb,  cfg.blue,       LV_ANIM_OFF);
    if (cfg.enabled)
        lv_obj_add_state(s_sched_sw, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(s_sched_sw, LV_STATE_CHECKED);
}

/* Full-width 0-255 colour slider with label */
static lv_obj_t *make_rgb_slider(lv_obj_t *parent, const char *key,
                                  lv_color_t accent, uint8_t init)
{
    lv_obj_t *row = make_row(parent);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, key);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl, 20);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_flex_grow(sl, 1);
    lv_slider_set_range(sl, 0, 255);
    lv_slider_set_value(sl, init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, accent,
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sl, accent,
                               LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_INPUT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_height(sl, 8);
    lv_obj_set_style_pad_ver(sl, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(sl, led_color_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
}

static void build_led_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    /* ── Manual control card ──────────────────────────────────── */
    lv_obj_t *mc = lv_obj_create(tab);
    style_bg(mc);
    lv_obj_set_width(mc, LV_PCT(100));
    lv_obj_set_height(mc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(mc, 10, 0);

    /* Power switch row */
    lv_obj_t *pr = make_row(mc);
    lv_obj_t *ptitle = lv_label_create(pr);
    lv_label_set_text(ptitle, "Controllo Manuale");
    style_section_lbl(ptitle);
    s_led_sw = lv_switch_create(pr);
    style_switch(s_led_sw);
    if (led_controller_is_on())
        lv_obj_add_state(s_led_sw, LV_STATE_CHECKED);

    /* Brightness slider + label */
    lv_obj_t *br_row = make_row(mc);
    lv_obj_t *br_lbl_key = lv_label_create(br_row);
    lv_label_set_text(br_lbl_key, "Lum.");
    style_muted_lbl(br_lbl_key);
    s_led_br_lbl = lv_label_create(br_row);
    lv_obj_set_style_text_color(s_led_br_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_led_br_lbl, &lv_font_montserrat_14, 0);

    s_led_br_sl = lv_slider_create(mc);
    lv_obj_set_width(s_led_br_sl, LV_PCT(100));
    lv_slider_set_range(s_led_br_sl, 0, 255);
    lv_slider_set_value(s_led_br_sl,
                        led_controller_get_brightness(), LV_ANIM_OFF);
    style_slider(s_led_br_sl);
    lv_obj_add_event_cb(s_led_br_sl, led_br_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    /* init brightness label */
    lv_label_set_text_fmt(s_led_br_lbl, "%d%%",
                          led_controller_get_brightness() * 100 / 255);

    /* Colour preview bar */
    s_led_preview = lv_obj_create(mc);
    lv_obj_set_size(s_led_preview, LV_PCT(100), 30);
    lv_obj_set_style_radius(s_led_preview, 8, 0);
    lv_obj_set_style_border_width(s_led_preview, 0, 0);
    lv_obj_remove_flag(s_led_preview, LV_OBJ_FLAG_SCROLLABLE);
    {
        uint8_t r, g, b;
        led_controller_get_color(&r, &g, &b);
        lv_obj_set_style_bg_color(s_led_preview, lv_color_make(r, g, b), 0);
        lv_obj_set_style_bg_opa(s_led_preview, LV_OPA_COVER, 0);
    }

    /* RGB sliders */
    {
        uint8_t r, g, b;
        led_controller_get_color(&r, &g, &b);
        s_led_r = make_rgb_slider(mc, "R", lv_color_hex(0xff4444), r);
        s_led_g = make_rgb_slider(mc, "G", lv_color_hex(0x44ff44), g);
        s_led_b = make_rgb_slider(mc, "B", lv_color_hex(0x4488ff), b);
    }

    /* Apply button */
    lv_obj_t *ab = lv_button_create(mc);
    lv_obj_set_width(ab, LV_PCT(100));
    lv_obj_set_height(ab, 52);
    style_btn_accent(ab);
    lv_obj_t *al = lv_label_create(ab);
    lv_label_set_text(al, "Applica");
    lv_obj_center(al);
    lv_obj_add_event_cb(ab, led_apply_cb, LV_EVENT_CLICKED, NULL);

    /* ── Schedule card ────────────────────────────────────────── */
    lv_obj_t *sc = lv_obj_create(tab);
    style_bg(sc);
    lv_obj_set_width(sc, LV_PCT(100));
    lv_obj_set_height(sc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(sc, 8, 0);

    led_schedule_config_t cfg = led_schedule_get_config();

    /* Enable row */
    lv_obj_t *se = make_row(sc);
    lv_obj_t *set = lv_label_create(se);
    lv_label_set_text(set, "Programmazione");
    style_section_lbl(set);
    s_sched_sw = lv_switch_create(se);
    style_switch(s_sched_sw);
    if (cfg.enabled)
        lv_obj_add_state(s_sched_sw, LV_STATE_CHECKED);

    /* On time */
    lv_obj_t *on_row = make_row(sc);
    lv_obj_t *on_lbl = lv_label_create(on_row);
    lv_label_set_text(on_lbl, "Accensione");
    style_muted_lbl(on_lbl);
    make_time_row(on_row, &s_son_h, &s_son_m,
                  cfg.on_hour, cfg.on_minute);

    /* Off time */
    lv_obj_t *off_row = make_row(sc);
    lv_obj_t *off_lbl = lv_label_create(off_row);
    lv_label_set_text(off_lbl, "Spegnimento");
    style_muted_lbl(off_lbl);
    make_time_row(off_row, &s_soff_h, &s_soff_m,
                  cfg.off_hour, cfg.off_minute);

    /* Ramp duration */
    lv_obj_t *ramp_row = make_row(sc);
    lv_obj_t *ramp_lbl = lv_label_create(ramp_row);
    lv_label_set_text(ramp_lbl, "Ramp (min)");
    style_muted_lbl(ramp_lbl);
    s_sramp = make_sb(ramp_row, 0, 120, cfg.ramp_duration_min, 3, 0);

    /* Day brightness */
    lv_obj_t *dbr_lbl = lv_label_create(sc);
    lv_label_set_text(dbr_lbl, "Luminosità");
    style_muted_lbl(dbr_lbl);
    s_sbr = lv_slider_create(sc);
    lv_obj_set_width(s_sbr, LV_PCT(100));
    lv_slider_set_range(s_sbr, 0, 255);
    lv_slider_set_value(s_sbr, cfg.brightness, LV_ANIM_OFF);
    style_slider(s_sbr);

    /* Day colour sliders */
    lv_obj_t *dcl = lv_label_create(sc);
    lv_label_set_text(dcl, "Colore giorno");
    style_muted_lbl(dcl);
    s_sr = make_rgb_slider(sc, "R", lv_color_hex(0xff4444), cfg.red);
    s_sg = make_rgb_slider(sc, "G", lv_color_hex(0x44ff44), cfg.green);
    s_sb = make_rgb_slider(sc, "B", lv_color_hex(0x4488ff), cfg.blue);

    /* Pause enable row */
    lv_obj_t *pe = make_row(sc);
    lv_obj_t *pet = lv_label_create(pe);
    lv_label_set_text(pet, "Pausa mezzogiorno");
    style_muted_lbl(pet);
    s_pause_sw = lv_switch_create(pe);
    style_switch(s_pause_sw);
    if (cfg.pause_enabled)
        lv_obj_add_state(s_pause_sw, LV_STATE_CHECKED);

    /* Pause start/end */
    lv_obj_t *ps_row = make_row(sc);
    lv_obj_t *ps_lbl = lv_label_create(ps_row);
    lv_label_set_text(ps_lbl, "Inizio pausa");
    style_muted_lbl(ps_lbl);
    make_time_row(ps_row, &s_pstart_h, &s_pstart_m,
                  cfg.pause_start_hour, cfg.pause_start_minute);

    lv_obj_t *pe_row = make_row(sc);
    lv_obj_t *pe_lbl = lv_label_create(pe_row);
    lv_label_set_text(pe_lbl, "Fine pausa");
    style_muted_lbl(pe_lbl);
    make_time_row(pe_row, &s_pend_h, &s_pend_m,
                  cfg.pause_end_hour, cfg.pause_end_minute);

    /* Pause brightness */
    lv_obj_t *pbr_lbl = lv_label_create(sc);
    lv_label_set_text(pbr_lbl, "Lum. pausa");
    style_muted_lbl(pbr_lbl);
    s_pbr = lv_slider_create(sc);
    lv_obj_set_width(s_pbr, LV_PCT(100));
    lv_slider_set_range(s_pbr, 0, 255);
    lv_slider_set_value(s_pbr, cfg.pause_brightness, LV_ANIM_OFF);
    style_slider(s_pbr);

    /* Save schedule button */
    lv_obj_t *ssb = lv_button_create(sc);
    lv_obj_set_width(ssb, LV_PCT(100));
    lv_obj_set_height(ssb, 52);
    style_btn_accent(ssb);
    lv_obj_t *ssl = lv_label_create(ssb);
    lv_label_set_text(ssl, "Salva Programma");
    lv_obj_center(ssl);
    lv_obj_add_event_cb(ssb, sched_save_cb, LV_EVENT_CLICKED, NULL);

    /* ── Presets card ─────────────────────────────────────────── */
    lv_obj_t *pc = lv_obj_create(tab);
    style_bg(pc);
    lv_obj_set_width(pc, LV_PCT(100));
    lv_obj_set_height(pc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(pc, 8, 0);

    lv_obj_t *pt = lv_label_create(pc);
    lv_label_set_text(pt, "Preset");
    style_section_lbl(pt);

    for (int i = 0; i < LED_PRESET_COUNT; i++) {
        led_preset_t pr_data;
        char btn_text[32];
        if (led_preset_get(i, &pr_data) && pr_data.name[0])
            snprintf(btn_text, sizeof(btn_text), "%s", pr_data.name);
        else
            snprintf(btn_text, sizeof(btn_text), "Preset %d", i + 1);

        lv_obj_t *pb = lv_button_create(pc);
        lv_obj_set_width(pb, LV_PCT(100));
        lv_obj_set_height(pb, 48);
        style_btn_dark(pb);
        lv_obj_t *pl = lv_label_create(pb);
        lv_label_set_text(pl, btn_text);
        lv_obj_center(pl);
        lv_obj_add_event_cb(pb, preset_load_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  9. Relay tab
 * ╚══════════════════════════════════════════════════════════════════════ */

static void relay_sw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    relay_controller_set(idx,
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* ── Relay rename modal ─────────────────────────────────────────────── */

typedef struct {
    int        idx;
    lv_obj_t  *ta;
    lv_obj_t  *overlay;
} rename_ctx_t;

static void rename_ok_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    rename_ctx_t *ctx = (rename_ctx_t *)lv_event_get_user_data(e);
    const char *new_name = lv_textarea_get_text(ctx->ta);
    if (new_name && new_name[0]) {
        relay_controller_set_name(ctx->idx, new_name);
        /* update the name label in the relay tab */
        if (s_rel_name_lbl[ctx->idx])
            lv_label_set_text(s_rel_name_lbl[ctx->idx], new_name);
        /* update the home tab name label (child of the relay btn card) */
        if (s_home_relay_sw[ctx->idx]) {
            lv_obj_t *card = lv_obj_get_parent(s_home_relay_sw[ctx->idx]);
            if (card) {
                lv_obj_t *nl = lv_obj_get_child(card, 0);
                if (nl) lv_label_set_text(nl, new_name);
            }
        }
    }
    lv_obj_del(ctx->overlay); /* frees ctx via LV_EVENT_DELETE */
}

static void rename_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_del((lv_obj_t *)lv_event_get_user_data(e));
}

static void rename_cleanup_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE)
        free(lv_event_get_user_data(e));
}

static void relay_name_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);

    rename_ctx_t *ctx = malloc(sizeof(rename_ctx_t));
    if (!ctx) return;
    ctx->idx = idx;

    /* Fullscreen overlay on the top layer */
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    ctx->overlay = ov;

    /* Title panel */
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, 560, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(panel, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 10, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text_fmt(title, "Rinomina Relè %d", idx + 1);
    style_section_lbl(title);

    /* Text area */
    lv_obj_t *ta = lv_textarea_create(panel);
    ctx->ta = ta;
    char cur[RELAY_NAME_MAX];
    relay_controller_get_name(idx, cur, sizeof(cur));
    lv_textarea_set_text(ta, cur);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, RELAY_NAME_MAX - 1);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);

    /* OK / Cancel buttons */
    lv_obj_t *br = lv_obj_create(panel);
    style_transp(br);
    lv_obj_set_width(br, LV_PCT(100));
    lv_obj_set_height(br, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(br, 10, 0);

    lv_obj_t *bok = lv_button_create(br);
    lv_obj_set_flex_grow(bok, 1);
    lv_obj_set_height(bok, 52);
    style_btn_green(bok);
    lv_obj_t *lokl = lv_label_create(bok);
    lv_label_set_text(lokl, "OK");
    lv_obj_center(lokl);
    lv_obj_add_event_cb(bok, rename_ok_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *bcan = lv_button_create(br);
    lv_obj_set_flex_grow(bcan, 1);
    lv_obj_set_height(bcan, 52);
    style_btn_dark(bcan);
    lv_obj_t *lcl = lv_label_create(bcan);
    lv_label_set_text(lcl, "Annulla");
    lv_obj_center(lcl);
    lv_obj_add_event_cb(bcan, rename_cancel_cb, LV_EVENT_CLICKED, ov);

    /* Register cleanup so ctx is freed when overlay is deleted */
    lv_obj_add_event_cb(ov, rename_cleanup_cb, LV_EVENT_DELETE, ctx);

    /* Keyboard below the panel */
    lv_obj_t *kb = lv_keyboard_create(ov);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), 0);
}

static void build_relay_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    relay_state_t rs[RELAY_COUNT];
    relay_controller_get_all(rs);

    for (int i = 0; i < RELAY_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(tab);
        style_bg(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                    LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(card, 10, 0);

        /* Name button (tap to rename) – flex row: [name_lbl | edit icon] */
        lv_obj_t *nbtn = lv_button_create(card);
        lv_obj_set_flex_grow(nbtn, 1);
        lv_obj_set_height(nbtn, 52);
        style_btn_dark(nbtn);
        lv_obj_set_flex_flow(nbtn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(nbtn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                    LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(nbtn, 6, 0);

        s_rel_name_lbl[i] = lv_label_create(nbtn);
        lv_label_set_text(s_rel_name_lbl[i], rs[i].name);
        lv_obj_set_style_text_font(s_rel_name_lbl[i],
                                   &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(s_rel_name_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(s_rel_name_lbl[i], 1);

        lv_obj_t *hint = lv_label_create(nbtn);
        lv_label_set_text(hint, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);

        lv_obj_add_event_cb(nbtn, relay_name_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        /* ON/OFF switch */
        lv_obj_t *sw = lv_switch_create(card);
        style_switch(sw);
        if (rs[i].on)
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, relay_sw_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)i);
        s_rel_sw[i] = sw;
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  10. Config tab
 * ╚══════════════════════════════════════════════════════════════════════ */

static void ht_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto_heater_config_t cfg;
    cfg.enabled      = lv_obj_has_state(s_ht_en, LV_STATE_CHECKED);
    cfg.relay_index  = (int)lv_spinbox_get_value(s_ht_relay);
    cfg.target_temp_c= (float)lv_spinbox_get_value(s_ht_target) / 10.0f;
    cfg.hysteresis_c = (float)lv_spinbox_get_value(s_ht_hyst)   / 10.0f;
    auto_heater_set_config(&cfg);
}

static void co2_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    co2_config_t cfg;
    cfg.enabled      = lv_obj_has_state(s_co2_en, LV_STATE_CHECKED);
    cfg.relay_index  = (int)lv_spinbox_get_value(s_co2_relay);
    cfg.pre_on_min   = (int)lv_spinbox_get_value(s_co2_pre);
    cfg.post_off_min = (int)lv_spinbox_get_value(s_co2_post);
    co2_controller_set_config(&cfg);
}

static void tz_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint16_t sel = lv_dropdown_get_selected(s_tz_dd);
    /* preset TZ strings – same set offered by the web UI */
    static const char * const tz_vals[] = {
        "CET-1CEST,M3.5.0/2,M10.5.0/3",   /* Italia / Europa Centrale  */
        "GMT0BST,M3.5.0/1,M10.5.0",         /* UK                       */
        "EST5EDT,M3.2.0,M11.1.0",           /* US East                  */
        "CST6CDT,M3.2.0,M11.1.0",           /* US Central               */
        "MST7MDT,M3.2.0,M11.1.0",           /* US Mountain              */
        "PST8PDT,M3.2.0,M11.1.0",           /* US Pacific               */
        "JST-9",                             /* Giappone                 */
        "AEST-10AEDT,M10.1.0,M4.1.0/3",     /* Australia Est            */
    };
    if (sel < (sizeof(tz_vals) / sizeof(tz_vals[0])))
        timezone_manager_set(tz_vals[sel]);
}

static void build_config_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    auto_heater_config_t ht  = auto_heater_get_config();
    co2_config_t         co2 = co2_controller_get_config();

    /* ── Auto-heater card ─────────────────────────────────────── */
    lv_obj_t *hc = lv_obj_create(tab);
    style_bg(hc);
    lv_obj_set_width(hc, LV_PCT(100));
    lv_obj_set_height(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(hc, 8, 0);

    /* Title + enable */
    lv_obj_t *hhr = make_row(hc);
    lv_obj_t *hht = lv_label_create(hhr);
    lv_label_set_text(hht, "Riscaldatore Auto");
    style_section_lbl(hht);
    s_ht_en = lv_switch_create(hhr);
    style_switch(s_ht_en);
    if (ht.enabled)
        lv_obj_add_state(s_ht_en, LV_STATE_CHECKED);

    /* Relay channel (0-3) */
    lv_obj_t *h_rel_row = make_row(hc);
    lv_obj_t *h_rel_lbl = lv_label_create(h_rel_row);
    lv_label_set_text(h_rel_lbl, "Relè (1-4)");
    style_muted_lbl(h_rel_lbl);
    s_ht_relay = make_sb(h_rel_row, 0, RELAY_COUNT - 1, ht.relay_index, 1, 0);

    /* Target temperature (150=15.0 .. 350=35.0 °C) */
    lv_obj_t *h_tgt_row = make_row(hc);
    lv_obj_t *h_tgt_lbl = lv_label_create(h_tgt_row);
    lv_label_set_text(h_tgt_lbl, "Target (°C)");
    style_muted_lbl(h_tgt_lbl);
    s_ht_target = make_sb(h_tgt_row, 150, 350,
                           (int32_t)(ht.target_temp_c * 10.0f + 0.5f),
                           3, 1);

    /* Hysteresis (1=0.1 .. 30=3.0 °C) */
    lv_obj_t *h_hy_row = make_row(hc);
    lv_obj_t *h_hy_lbl = lv_label_create(h_hy_row);
    lv_label_set_text(h_hy_lbl, "Isteresi (°C)");
    style_muted_lbl(h_hy_lbl);
    s_ht_hyst = make_sb(h_hy_row, 1, 30,
                         (int32_t)(ht.hysteresis_c * 10.0f + 0.5f),
                         2, 1);

    lv_obj_t *ht_sb = lv_button_create(hc);
    lv_obj_set_width(ht_sb, LV_PCT(100));
    lv_obj_set_height(ht_sb, 52);
    style_btn_accent(ht_sb);
    lv_obj_t *ht_sl = lv_label_create(ht_sb);
    lv_label_set_text(ht_sl, "Salva Riscaldatore");
    lv_obj_center(ht_sl);
    lv_obj_add_event_cb(ht_sb, ht_save_cb, LV_EVENT_CLICKED, NULL);

    /* ── CO2 controller card ──────────────────────────────────── */
    lv_obj_t *cc = lv_obj_create(tab);
    style_bg(cc);
    lv_obj_set_width(cc, LV_PCT(100));
    lv_obj_set_height(cc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(cc, 8, 0);

    lv_obj_t *chr = make_row(cc);
    lv_obj_t *cht = lv_label_create(chr);
    lv_label_set_text(cht, "CO\u2082 Controller");
    style_section_lbl(cht);
    s_co2_en = lv_switch_create(chr);
    style_switch(s_co2_en);
    if (co2.enabled)
        lv_obj_add_state(s_co2_en, LV_STATE_CHECKED);

    lv_obj_t *c_rel_row = make_row(cc);
    lv_obj_t *c_rel_lbl = lv_label_create(c_rel_row);
    lv_label_set_text(c_rel_lbl, "Relè (1-4)");
    style_muted_lbl(c_rel_lbl);
    s_co2_relay = make_sb(c_rel_row, 0, RELAY_COUNT - 1,
                           co2.relay_index, 1, 0);

    lv_obj_t *c_pre_row = make_row(cc);
    lv_obj_t *c_pre_lbl = lv_label_create(c_pre_row);
    lv_label_set_text(c_pre_lbl, "Anticipo ON (min)");
    style_muted_lbl(c_pre_lbl);
    s_co2_pre = make_sb(c_pre_row, 0, 60, co2.pre_on_min, 2, 0);

    lv_obj_t *c_post_row = make_row(cc);
    lv_obj_t *c_post_lbl = lv_label_create(c_post_row);
    lv_label_set_text(c_post_lbl, "Ritardo OFF (min)");
    style_muted_lbl(c_post_lbl);
    s_co2_post = make_sb(c_post_row, 0, 60, co2.post_off_min, 2, 0);

    lv_obj_t *co2_sb = lv_button_create(cc);
    lv_obj_set_width(co2_sb, LV_PCT(100));
    lv_obj_set_height(co2_sb, 52);
    style_btn_accent(co2_sb);
    lv_obj_t *co2_sl = lv_label_create(co2_sb);
    lv_label_set_text(co2_sl, "Salva CO\u2082");
    lv_obj_center(co2_sl);
    lv_obj_add_event_cb(co2_sb, co2_save_cb, LV_EVENT_CLICKED, NULL);

    /* ── Timezone card ────────────────────────────────────────── */
    lv_obj_t *tzc = lv_obj_create(tab);
    style_bg(tzc);
    lv_obj_set_width(tzc, LV_PCT(100));
    lv_obj_set_height(tzc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tzc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(tzc, 8, 0);

    lv_obj_t *tzt = lv_label_create(tzc);
    lv_label_set_text(tzt, "Fuso Orario");
    style_section_lbl(tzt);

    s_tz_dd = lv_dropdown_create(tzc);
    lv_dropdown_set_options(s_tz_dd,
        "Italia / Europa Centrale\n"
        "UK (GMT/BST)\n"
        "US Eastern\n"
        "US Central\n"
        "US Mountain\n"
        "US Pacific\n"
        "Giappone (JST)\n"
        "Australia Est");
    lv_obj_set_width(s_tz_dd, LV_PCT(100));
    lv_obj_set_style_bg_color(s_tz_dd, lv_color_hex(C_INPUT), 0);
    lv_obj_set_style_text_color(s_tz_dd, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(s_tz_dd, lv_color_hex(C_BORDER), 0);

    /* Pre-select current timezone */
    {
        char cur_tz[TZ_STRING_MAX];
        timezone_manager_get(cur_tz, sizeof(cur_tz));
        const char * const tz_strs[] = {
            "CET-1CEST,M3.5.0/2,M10.5.0/3",
            "GMT0BST,M3.5.0/1,M10.5.0",
            "EST5EDT,M3.2.0,M11.1.0",
            "CST6CDT,M3.2.0,M11.1.0",
            "MST7MDT,M3.2.0,M11.1.0",
            "PST8PDT,M3.2.0,M11.1.0",
            "JST-9",
            "AEST-10AEDT,M10.1.0,M4.1.0/3",
        };
        for (int i = 0; i < (int)(sizeof(tz_strs)/sizeof(tz_strs[0])); i++) {
            if (strcmp(cur_tz, tz_strs[i]) == 0) {
                lv_dropdown_set_selected(s_tz_dd, (uint16_t)i);
                break;
            }
        }
    }

    lv_obj_t *tz_btn = lv_button_create(tzc);
    lv_obj_set_width(tz_btn, LV_PCT(100));
    lv_obj_set_height(tz_btn, 52);
    style_btn_accent(tz_btn);
    lv_obj_t *tz_lbl = lv_label_create(tz_btn);
    lv_label_set_text(tz_lbl, "Applica Fuso Orario");
    lv_obj_center(tz_lbl);
    lv_obj_add_event_cb(tz_btn, tz_save_cb, LV_EVENT_CLICKED, NULL);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  11. Info tab
 * ╚══════════════════════════════════════════════════════════════════════ */

static void build_info_tab(lv_obj_t *tab)
{
    style_tab_page(tab);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ic = lv_obj_create(tab);
    style_bg(ic);
    lv_obj_set_width(ic, LV_PCT(100));
    lv_obj_set_height(ic, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ic, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ic, 8, 0);

    lv_obj_t *it = lv_label_create(ic);
    lv_label_set_text(it, "Sistema");
    style_section_lbl(it);

    make_kv_row(ic, "WiFi",    &s_info_wifi);
    make_kv_row(ic, "IP",      &s_info_ip);
    make_kv_row(ic, "Heap",    &s_info_heap);
    make_kv_row(ic, "Uptime",  &s_info_uptime);
    make_kv_row(ic, "Ora",     &s_info_time);
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  12. Data-refresh LVGL timer (runs inside LVGL task – no extra lock)
 * ╚══════════════════════════════════════════════════════════════════════ */

static void ui_refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* ── Temperature ─────────────────────────────────────────── */
    float temp_c = 0.0f;
    bool  tok    = temperature_sensor_get(&temp_c);
    if (s_temp_lbl) {
        if (tok) {
            lv_label_set_text_fmt(s_temp_lbl, "%.1f\xc2\xb0""C", temp_c);
            bool ok_range = (temp_c >= 24.0f && temp_c <= 28.0f);
            lv_obj_set_style_text_color(s_temp_lbl,
                ok_range ? lv_color_hex(C_ON) : lv_color_hex(C_ERR), 0);
        } else {
            lv_label_set_text(s_temp_lbl, "--.-\xc2\xb0""C");
            lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(C_MUTED), 0);
        }
    }
    if (s_temp_status) {
        lv_label_set_text(s_temp_status, tok ? "Sensore: OK" : "Sensore: Errore");
        lv_obj_set_style_text_color(s_temp_status,
            tok ? lv_color_hex(C_ON) : lv_color_hex(C_ERR), 0);
    }

    /* ── LED status (Home tab) ───────────────────────────────── */
    if (s_led_state_lbl) {
        bool on   = led_controller_is_on();
        int  pct  = on ? (led_controller_get_brightness() * 100 / 255) : 0;
        lv_label_set_text_fmt(s_led_state_lbl, "%s  %d%%",
                              on ? "Accese" : "Spente", pct);
        lv_obj_set_style_text_color(s_led_state_lbl,
            on ? lv_color_hex(C_ON) : lv_color_hex(C_MUTED), 0);
    }
    if (s_led_swatch) {
        uint8_t r, g, b;
        led_controller_get_color(&r, &g, &b);
        bool on = led_controller_is_on();
        lv_obj_set_style_bg_color(s_led_swatch,
            on ? lv_color_make(r, g, b) : lv_color_hex(C_INPUT), 0);
    }

    /* ── Relay states ────────────────────────────────────────── */
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool on = relay_controller_get(i);
        if (s_home_relay_sw[i]) {
            if (on) lv_obj_add_state(s_home_relay_sw[i], LV_STATE_CHECKED);
            else    lv_obj_remove_state(s_home_relay_sw[i], LV_STATE_CHECKED);
        }
        if (s_rel_sw[i]) {
            if (on) lv_obj_add_state(s_rel_sw[i], LV_STATE_CHECKED);
            else    lv_obj_remove_state(s_rel_sw[i], LV_STATE_CHECKED);
        }
    }

    /* ── Info tab ────────────────────────────────────────────── */
    if (s_info_wifi) {
        if (wifi_manager_is_connected()) {
            lv_label_set_text(s_info_wifi, "Connesso");
            lv_obj_set_style_text_color(s_info_wifi, lv_color_hex(C_ON), 0);
        } else {
            lv_label_set_text(s_info_wifi, "Disconnesso");
            lv_obj_set_style_text_color(s_info_wifi, lv_color_hex(C_ERR), 0);
        }
    }
    if (s_info_ip) {
        char ip[20] = "N/D";
        wifi_manager_get_ip_str(ip, sizeof(ip));
        lv_label_set_text(s_info_ip, ip);
    }
    if (s_info_heap) {
        lv_label_set_text_fmt(s_info_heap, "%" PRIu32 " B",
                              esp_get_free_heap_size());
    }
    if (s_info_uptime) {
        int64_t secs = esp_timer_get_time() / 1000000;
        lv_label_set_text_fmt(s_info_uptime, "%" PRId64 "h %" PRId64 "m",
                              secs / 3600, (secs % 3600) / 60);
    }
    if (s_info_time) {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        if (ti.tm_year >= (2024 - 1900)) {
            lv_label_set_text_fmt(s_info_time,
                "%02d:%02d  %02d/%02d/%04d",
                ti.tm_hour, ti.tm_min,
                ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
        } else {
            lv_label_set_text(s_info_time, "-- (NTP n/d)");
        }
    }
}

/* ╔══════════════════════════════════════════════════════════════════════
 * ║  13. Build UI and entry point
 * ╚══════════════════════════════════════════════════════════════════════ */

static void build_ui(void)
{
    /* Dark screen background */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    /* Tab view – tab bar at bottom, 72 px tall */
    lv_obj_t *tv = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 72);
    lv_obj_set_size(tv, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    /* Style the tab button bar */
    lv_obj_t *tb = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(tb, 1, LV_PART_MAIN);
    lv_obj_set_style_text_color(tb, lv_color_hex(C_MUTED),
                                LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tb, lv_color_hex(C_ACCENT),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x1e293b),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tb, &lv_font_montserrat_14, LV_PART_ITEMS);

    /* Build each tab */
    lv_obj_t *tab_home   = lv_tabview_add_tab(tv, LV_SYMBOL_HOME "  Home");
    lv_obj_t *tab_led    = lv_tabview_add_tab(tv, LV_SYMBOL_IMAGE " LED");
    lv_obj_t *tab_relay  = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS " Rel\u00e8");
    lv_obj_t *tab_cfg    = lv_tabview_add_tab(tv, LV_SYMBOL_EDIT " Cfg");
    lv_obj_t *tab_info   = lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Info");

    build_home_tab(tab_home);
    build_led_tab(tab_led);
    build_relay_tab(tab_relay);
    build_config_tab(tab_cfg);
    build_info_tab(tab_info);

    /* Periodic data refresh (2 s) */
    lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);

    ESP_LOGI(TAG, "UI built – 5 tabs");
}

esp_err_t display_ui_init(void)
{
    ESP_LOGI(TAG, "Initialising display …");

    /* ── Hardware ─────────────────────────────────────────────── */
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

    /* ── LVGL ─────────────────────────────────────────────────── */
    lv_init();

    /* Draw buffers in PSRAM */
    size_t buf_sz  = (size_t)LCD_W * LVGL_BUF_LINES * sizeof(lv_color16_t);
    void  *buf1    = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void  *buf2    = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

    /* Touch input device */
    if (s_touch) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, lvgl_touch_cb);
        lv_indev_set_user_data(indev, s_touch);
    }

    /* Tick source – use esp_timer periodic callback */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                             LVGL_TICK_MS * 1000ULL));

    /* Mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Build the UI */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    build_ui();
    xSemaphoreGive(s_mutex);

    /* LVGL handler task on core 1 */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl",
                            LVGL_TASK_STACK, NULL,
                            LVGL_TASK_PRIO, NULL, 1);

    ESP_LOGI(TAG, "Display UI ready – 720×720 MIPI-DSI touch display");
    return ESP_OK;
}

#endif /* CONFIG_DISPLAY_ENABLED */
