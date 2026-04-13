/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – LVGL Dashboard UI implementation
 *
 * Creates and periodically refreshes the on-screen aquarium dashboard
 * shown on the 5″ MIPI DSI touch display (800×480).
 *
 * Layout mirrors the Web UI with four swipe-able tabs:
 *   Tab 0 – Riepilogo   : Temperature, LED scene, WiFi, Relay overview
 *   Tab 1 – LED Strip    : Scene name, brightness, colour, on/off
 *   Tab 2 – Telegram     : Enabled, alarms, reminders status
 *   Tab 3 – Manutenzione : WiFi/IP, heap, uptime, heater, DuckDNS
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "display_driver.h"
#include "display_ui.h"
#include "temperature_sensor.h"
#include "led_controller.h"
#include "led_scenes.h"
#include "relay_controller.h"
#include "wifi_manager.h"
#include "auto_heater.h"
#include "telegram_notify.h"
#include "duckdns.h"

static const char *TAG = "disp_ui";

/* ── Colours (same palette as the Web UI) ────────────────────────── */
#define CLR_BG_DARK       lv_color_hex(0x0b1121)
#define CLR_BG_CARD       lv_color_hex(0x131c31)
#define CLR_PRIMARY       lv_color_hex(0x111827)
#define CLR_ACCENT        lv_color_hex(0x38bdf8)
#define CLR_GREEN         lv_color_hex(0x4ade80)
#define CLR_RED           lv_color_hex(0xf87171)
#define CLR_YELLOW        lv_color_hex(0xfbbf24)
#define CLR_TEXT          lv_color_hex(0xe2e8f0)
#define CLR_TEXT_DIM      lv_color_hex(0x64748b)
#define CLR_BORDER        lv_color_hex(0x1a2540)

/* ── Label handles for dynamic refresh ───────────────────────────── */
/* Header */
static lv_obj_t *s_lbl_clock = NULL;

/* Tab 0 – Riepilogo */
static lv_obj_t *s_lbl_temp        = NULL;
static lv_obj_t *s_lbl_temp_status = NULL;
static lv_obj_t *s_lbl_scene_sum   = NULL;
static lv_obj_t *s_lbl_led_bright  = NULL;
static lv_obj_t *s_obj_led_swatch  = NULL;
static lv_obj_t *s_lbl_wifi_sum    = NULL;
static lv_obj_t *s_lbl_relay[RELAY_COUNT] = {NULL};
static lv_obj_t *s_obj_relay_badge[RELAY_COUNT] = {NULL};

/* Tab 1 – LED Strip */
static lv_obj_t *s_lbl_led_power   = NULL;
static lv_obj_t *s_lbl_led_scene   = NULL;
static lv_obj_t *s_lbl_led_bri2    = NULL;
static lv_obj_t *s_lbl_led_rgb     = NULL;
static lv_obj_t *s_obj_led_preview = NULL;
static lv_obj_t *s_lbl_led_config  = NULL;

/* Tab 2 – Telegram */
static lv_obj_t *s_lbl_tg_enabled  = NULL;
static lv_obj_t *s_lbl_tg_alarms   = NULL;
static lv_obj_t *s_lbl_tg_wc       = NULL;
static lv_obj_t *s_lbl_tg_fert     = NULL;
static lv_obj_t *s_lbl_tg_summary  = NULL;

/* Tab 3 – Manutenzione */
static lv_obj_t *s_lbl_sys_wifi    = NULL;
static lv_obj_t *s_lbl_sys_ip      = NULL;
static lv_obj_t *s_lbl_sys_heap    = NULL;
static lv_obj_t *s_lbl_sys_uptime  = NULL;
static lv_obj_t *s_lbl_heater      = NULL;
static lv_obj_t *s_lbl_ddns        = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

/** Create a card container matching the Web UI .card style. */
static lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, CLR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_color(card, CLR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 4, 0);
    return card;
}

/** Card header label (accent colour, bold). */
static lv_obj_t *make_card_header(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, CLR_ACCENT, 0);
    return lbl;
}

/** Create a label inside a parent with given font and colour. */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, colour, 0);
    return lbl;
}

/** Convert 0-255 brightness to 0-100 percentage. */
static inline int brightness_pct(uint8_t br)
{
    return (int)(br * 100 / 255);
}

/** Scale an RGB colour by brightness and return an lv_color_t. */
static inline lv_color_t scaled_color(uint8_t r, uint8_t g, uint8_t b,
                                      uint8_t br)
{
    float s = (float)br / 255.0f;
    return lv_color_make((uint8_t)(r * s), (uint8_t)(g * s),
                         (uint8_t)(b * s));
}

/** Key → value row inside a card. */
static lv_obj_t *make_row(lv_obj_t *parent, const char *key,
                          const lv_font_t *val_font,
                          lv_color_t val_clr,
                          const char *val_init)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lk = lv_label_create(row);
    lv_label_set_text(lk, key);
    lv_obj_set_style_text_font(lk, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lk, CLR_TEXT_DIM, 0);

    lv_obj_t *lv = lv_label_create(row);
    lv_label_set_text(lv, val_init);
    lv_obj_set_style_text_font(lv, val_font, 0);
    lv_obj_set_style_text_color(lv, val_clr, 0);

    return lv;  /* return the value label for later updates */
}

/* ── Tab content builders ────────────────────────────────────────── */

/** Tab 0 – Riepilogo (summary). */
static void build_tab_riepilogo(lv_obj_t *parent)
{
    /* Two-column row container */
    lv_obj_t *cols = lv_obj_create(parent);
    lv_obj_set_size(cols, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 8, 0);

    /* ── Left column ─────────────────────────────────────────── */
    lv_obj_t *left = lv_obj_create(cols);
    lv_obj_set_size(left, LV_PCT(48), LV_PCT(100));
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);

    /* Temperature card */
    {
        lv_obj_t *card = make_card(left);
        make_card_header(card, LV_SYMBOL_EYE_OPEN "  TEMPERATURA");
        s_lbl_temp = make_label(card, "--.- °C",
                                &lv_font_montserrat_28, CLR_TEXT);
        s_lbl_temp_status = make_label(card, "Sensore: --",
                                       &lv_font_montserrat_14, CLR_TEXT_DIM);
    }

    /* WiFi status card */
    {
        lv_obj_t *card = make_card(left);
        make_card_header(card, LV_SYMBOL_WIFI "  WIFI");
        s_lbl_wifi_sum = make_label(card, "Disconnected",
                                    &lv_font_montserrat_16, CLR_RED);
    }

    /* ── Right column ────────────────────────────────────────── */
    lv_obj_t *right = lv_obj_create(cols);
    lv_obj_set_size(right, LV_PCT(48), LV_PCT(100));
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);

    /* LED scene status card */
    {
        lv_obj_t *card = make_card(right);
        make_card_header(card, LV_SYMBOL_IMAGE "  STATO LUCI");

        lv_obj_t *scene_row = lv_obj_create(card);
        lv_obj_set_width(scene_row, LV_PCT(100));
        lv_obj_set_height(scene_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(scene_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scene_row, 0, 0);
        lv_obj_set_style_pad_all(scene_row, 0, 0);
        lv_obj_set_flex_flow(scene_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(scene_row, 6, 0);

        /* Colour swatch */
        s_obj_led_swatch = lv_obj_create(scene_row);
        lv_obj_set_size(s_obj_led_swatch, 18, 18);
        lv_obj_set_style_radius(s_obj_led_swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_obj_led_swatch, CLR_ACCENT, 0);
        lv_obj_set_style_bg_opa(s_obj_led_swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_obj_led_swatch, 0, 0);

        s_lbl_scene_sum = make_label(scene_row, "Off",
                                     &lv_font_montserrat_16, CLR_TEXT);

        s_lbl_led_bright = make_label(card, "Brightness: --%",
                                      &lv_font_montserrat_14, CLR_TEXT_DIM);
    }

    /* Relays card */
    {
        lv_obj_t *card = make_card(right);
        make_card_header(card, LV_SYMBOL_POWER "  Relè");

        for (int i = 0; i < RELAY_COUNT; i++) {
            lv_obj_t *row = lv_obj_create(card);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 2, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            char buf[32];
            snprintf(buf, sizeof(buf), "Relay %d", i + 1);
            s_lbl_relay[i] = make_label(row, buf,
                                        &lv_font_montserrat_14, CLR_TEXT);

            /* Badge ON/OFF */
            s_obj_relay_badge[i] = make_label(row, "--",
                                              &lv_font_montserrat_14,
                                              CLR_TEXT_DIM);
        }
    }
}

/** Tab 1 – LED Strip. */
static void build_tab_led(lv_obj_t *parent)
{
    /* LED control card */
    lv_obj_t *card = make_card(parent);
    make_card_header(card, LV_SYMBOL_IMAGE "  Controllo LED");

    s_lbl_led_power = make_row(card, "Power",
                               &lv_font_montserrat_16, CLR_TEXT, "--");
    s_lbl_led_scene = make_row(card, "Scena",
                               &lv_font_montserrat_16, CLR_TEXT, "--");
    s_lbl_led_bri2  = make_row(card, "Luminosità",
                               &lv_font_montserrat_16, CLR_TEXT, "--");
    s_lbl_led_rgb   = make_row(card, "RGB",
                               &lv_font_montserrat_16, CLR_TEXT, "--, --, --");

    /* Colour preview bar */
    s_obj_led_preview = lv_obj_create(card);
    lv_obj_set_size(s_obj_led_preview, LV_PCT(100), 20);
    lv_obj_set_style_radius(s_obj_led_preview, 6, 0);
    lv_obj_set_style_bg_color(s_obj_led_preview, CLR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_obj_led_preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_obj_led_preview, CLR_BORDER, 0);
    lv_obj_set_style_border_width(s_obj_led_preview, 1, 0);

    /* Scene settings info card */
    lv_obj_t *card2 = make_card(parent);
    make_card_header(card2, LV_SYMBOL_SETTINGS "  Impostazioni Scena");
    s_lbl_led_config = make_label(card2, "Sunrise: -- min\n"
                                         "Sunset: -- min\n"
                                         "Temp. colore: -- K\n"
                                         "Siesta: --",
                                  &lv_font_montserrat_14, CLR_TEXT_DIM);
}

/** Tab 2 – Telegram. */
static void build_tab_telegram(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent);
    make_card_header(card, LV_SYMBOL_BELL "  Telegram");

    s_lbl_tg_enabled = make_row(card, "Stato",
                                &lv_font_montserrat_16, CLR_TEXT, "--");
    s_lbl_tg_alarms  = make_row(card, "Allarmi Temp.",
                                &lv_font_montserrat_14, CLR_TEXT, "--");
    s_lbl_tg_wc      = make_row(card, "Cambio Acqua",
                                &lv_font_montserrat_14, CLR_TEXT, "--");
    s_lbl_tg_fert    = make_row(card, "Fertilizzante",
                                &lv_font_montserrat_14, CLR_TEXT, "--");
    s_lbl_tg_summary = make_row(card, "Riepilogo Giorn.",
                                &lv_font_montserrat_14, CLR_TEXT, "--");
}

/** Tab 3 – Manutenzione. */
static void build_tab_manutenzione(lv_obj_t *parent)
{
    /* System status card */
    {
        lv_obj_t *card = make_card(parent);
        make_card_header(card, LV_SYMBOL_SETTINGS "  Stato Sistema");
        s_lbl_sys_wifi   = make_row(card, "Connessione",
                                    &lv_font_montserrat_14, CLR_TEXT, "--");
        s_lbl_sys_ip     = make_row(card, "IP",
                                    &lv_font_montserrat_14, CLR_TEXT, "--");
        s_lbl_sys_heap   = make_row(card, "Heap Libero",
                                    &lv_font_montserrat_14, CLR_TEXT, "--");
        s_lbl_sys_uptime = make_row(card, "Uptime",
                                    &lv_font_montserrat_14, CLR_TEXT, "--");
    }

    /* Auto-heater card */
    {
        lv_obj_t *card = make_card(parent);
        make_card_header(card, LV_SYMBOL_CHARGE "  Auto-Riscaldatore");
        s_lbl_heater = make_label(card, "-- ",
                                  &lv_font_montserrat_14, CLR_TEXT_DIM);
    }

    /* DuckDNS card */
    {
        lv_obj_t *card = make_card(parent);
        make_card_header(card, LV_SYMBOL_LOOP "  DuckDNS");
        s_lbl_ddns = make_label(card, "-- ",
                                &lv_font_montserrat_14, CLR_TEXT_DIM);
    }
}

/* ===================================================================
 *  Public API
 * =================================================================*/

esp_err_t display_ui_init(void)
{
    lv_display_t *disp = display_get_lvgl_display();
    if (!disp) {
        ESP_LOGE(TAG, "Display not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    display_lock();

    /* ── Screen background ────────────────────────────────────────── */
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, CLR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Header bar (fixed at top) ────────────────────────────────── */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 800, 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, CLR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);

    make_label(header, LV_SYMBOL_HOME "  Aquarium Controller",
               &lv_font_montserrat_16, CLR_TEXT);

    s_lbl_clock = lv_label_create(header);
    lv_label_set_text(s_lbl_clock, "--:--:--");
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_clock, CLR_ACCENT, 0);
    lv_obj_align(s_lbl_clock, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Tab view (below header, fills remaining space) ───────────── */
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_size(tv, 800, 440);
    lv_obj_align(tv, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Style the tab bar (top buttons) */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tab_bar, CLR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_bar, CLR_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tab_bar, CLR_TEXT_DIM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tab_bar, CLR_ACCENT,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tab_bar, &lv_font_montserrat_14,
                               LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x1a2540),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);

    /* ── Add the four tabs ────────────────────────────────────────── */
    lv_obj_t *tab0 = lv_tabview_add_tab(tv, LV_SYMBOL_HOME " Riepilogo");
    lv_obj_t *tab1 = lv_tabview_add_tab(tv, LV_SYMBOL_IMAGE " LED Strip");
    lv_obj_t *tab2 = lv_tabview_add_tab(tv, LV_SYMBOL_BELL " Telegram");
    lv_obj_t *tab3 = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS " Manutenzione");

    /* Style each tab content area */
    lv_obj_t *tabs[] = {tab0, tab1, tab2, tab3};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(tabs[i], CLR_BG_DARK, 0);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tabs[i], 8, 0);
        lv_obj_set_flex_flow(tabs[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(tabs[i], 8, 0);
    }

    /* ── Populate tabs ────────────────────────────────────────────── */
    build_tab_riepilogo(tab0);
    build_tab_led(tab1);
    build_tab_telegram(tab2);
    build_tab_manutenzione(tab3);

    display_unlock();

    ESP_LOGI(TAG, "Dashboard UI created (tabbed layout)");
    return ESP_OK;
}

/* ── Refresh helpers ─────────────────────────────────────────────── */

/** Format an uptime duration in seconds to "Xd Xh Xm" string. */
static void format_uptime(uint32_t sec, char *buf, size_t len)
{
    uint32_t d = sec / 86400;
    uint32_t h = (sec % 86400) / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (d > 0) {
        snprintf(buf, len, "%lud %luh %lum", (unsigned long)d,
                 (unsigned long)h, (unsigned long)m);
    } else if (h > 0) {
        snprintf(buf, len, "%luh %lum", (unsigned long)h, (unsigned long)m);
    } else {
        snprintf(buf, len, "%lum", (unsigned long)m);
    }
}

void display_ui_refresh(void)
{
    display_lock();

    if (!s_lbl_clock) {
        display_unlock();
        return;   /* UI not created yet */
    }

    /* ── Clock (header) ───────────────────────────────────────────── */
    {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        lv_label_set_text(s_lbl_clock, buf);
    }

    /* ── Tab 0 – Riepilogo ────────────────────────────────────────── */

    /* Temperature */
    {
        float temp;
        if (temperature_sensor_get(&temp)) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f °C", (double)temp);
            lv_label_set_text(s_lbl_temp, buf);
            bool ok = (temp >= 24.0f && temp <= 28.0f);
            lv_obj_set_style_text_color(s_lbl_temp,
                                        ok ? CLR_GREEN : CLR_RED, 0);
            lv_label_set_text(s_lbl_temp_status,
                              ok ? LV_SYMBOL_OK " Sensore OK"
                                 : LV_SYMBOL_WARNING " Fuori range");
            lv_obj_set_style_text_color(s_lbl_temp_status,
                                        ok ? CLR_GREEN : CLR_YELLOW, 0);
        } else {
            lv_label_set_text(s_lbl_temp, "-- °C");
            lv_obj_set_style_text_color(s_lbl_temp, CLR_TEXT_DIM, 0);
            lv_label_set_text(s_lbl_temp_status, "Sensore: N/A");
            lv_obj_set_style_text_color(s_lbl_temp_status, CLR_TEXT_DIM, 0);
        }
    }

    /* LED scene summary */
    {
        led_scene_t scene = led_scenes_get();
        const char *name  = led_scenes_get_name(scene);
        lv_label_set_text(s_lbl_scene_sum, name ? name : "Off");

        bool on = led_controller_is_on();
        uint8_t br = led_controller_get_brightness();
        char buf[32];
        snprintf(buf, sizeof(buf), "Luminosità: %d%%", brightness_pct(br));
        lv_label_set_text(s_lbl_led_bright, buf);

        uint8_t r, g, b;
        led_controller_get_color(&r, &g, &b);
        if (on) {
            lv_obj_set_style_bg_color(s_obj_led_swatch,
                                      scaled_color(r, g, b, br), 0);
        } else {
            lv_obj_set_style_bg_color(s_obj_led_swatch,
                                      CLR_TEXT_DIM, 0);
        }
    }

    /* WiFi summary */
    {
        if (wifi_manager_is_connected()) {
            char ip[16];
            wifi_manager_get_ip_str(ip, sizeof(ip));
            char buf[40];
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", ip);
            lv_label_set_text(s_lbl_wifi_sum, buf);
            lv_obj_set_style_text_color(s_lbl_wifi_sum, CLR_GREEN, 0);
        } else {
            lv_label_set_text(s_lbl_wifi_sum, LV_SYMBOL_CLOSE " Disconnesso");
            lv_obj_set_style_text_color(s_lbl_wifi_sum, CLR_RED, 0);
        }
    }

    /* Relays */
    {
        relay_state_t rels[RELAY_COUNT];
        relay_controller_get_all(rels);
        for (int i = 0; i < RELAY_COUNT; i++) {
            const char *name = rels[i].name[0] ? rels[i].name : "Relay";
            char buf[48];
            snprintf(buf, sizeof(buf), "%s %d", name, i + 1);
            lv_label_set_text(s_lbl_relay[i], buf);

            lv_label_set_text(s_obj_relay_badge[i],
                              rels[i].on ? "ON" : "OFF");
            lv_obj_set_style_text_color(s_obj_relay_badge[i],
                rels[i].on ? CLR_GREEN : CLR_TEXT_DIM, 0);
        }
    }

    /* ── Tab 1 – LED Strip ────────────────────────────────────────── */
    {
        bool on = led_controller_is_on();
        lv_label_set_text(s_lbl_led_power, on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_lbl_led_power,
                                    on ? CLR_GREEN : CLR_RED, 0);

        led_scene_t scene = led_scenes_get();
        const char *sname = led_scenes_get_name(scene);
        lv_label_set_text(s_lbl_led_scene, sname ? sname : "Manuale");

        uint8_t br = led_controller_get_brightness();
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", br);
        lv_label_set_text(s_lbl_led_bri2, buf);

        uint8_t r, g, b;
        led_controller_get_color(&r, &g, &b);
        snprintf(buf, sizeof(buf), "%d, %d, %d", r, g, b);
        lv_label_set_text(s_lbl_led_rgb, buf);

        /* Preview bar */
        if (on) {
            lv_obj_set_style_bg_color(s_obj_led_preview,
                                      scaled_color(r, g, b, br), 0);
        } else {
            lv_obj_set_style_bg_color(s_obj_led_preview, CLR_BG_DARK, 0);
        }

        /* Scene config info */
        led_scene_config_t cfg = led_scenes_get_config();
        char cfg_buf[128];
        snprintf(cfg_buf, sizeof(cfg_buf),
                 "Sunrise: %d min\n"
                 "Sunset: %d min\n"
                 "Temp. colore: %d K\n"
                 "Siesta: %s",
                 cfg.sunrise_duration_min,
                 cfg.sunset_duration_min,
                 cfg.color_temp_kelvin,
                 cfg.siesta_enabled ? "Attiva" : "Disattiva");
        lv_label_set_text(s_lbl_led_config, cfg_buf);
    }

    /* ── Tab 2 – Telegram ─────────────────────────────────────────── */
    {
        telegram_config_t tg = telegram_notify_get_config();

        lv_label_set_text(s_lbl_tg_enabled,
                          tg.enabled ? "Abilitato" : "Disabilitato");
        lv_obj_set_style_text_color(s_lbl_tg_enabled,
                                    tg.enabled ? CLR_GREEN : CLR_TEXT_DIM, 0);

        if (tg.temp_alarm_enabled) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.1f–%.1f °C",
                     (double)tg.temp_low_c, (double)tg.temp_high_c);
            lv_label_set_text(s_lbl_tg_alarms, buf);
            lv_obj_set_style_text_color(s_lbl_tg_alarms, CLR_GREEN, 0);
        } else {
            lv_label_set_text(s_lbl_tg_alarms, "Disabilitato");
            lv_obj_set_style_text_color(s_lbl_tg_alarms, CLR_TEXT_DIM, 0);
        }

        lv_label_set_text(s_lbl_tg_wc,
            tg.water_change_enabled ? "Attivo" : "Disabilitato");
        lv_obj_set_style_text_color(s_lbl_tg_wc,
            tg.water_change_enabled ? CLR_GREEN : CLR_TEXT_DIM, 0);

        lv_label_set_text(s_lbl_tg_fert,
            tg.fertilizer_enabled ? "Attivo" : "Disabilitato");
        lv_obj_set_style_text_color(s_lbl_tg_fert,
            tg.fertilizer_enabled ? CLR_GREEN : CLR_TEXT_DIM, 0);

        lv_label_set_text(s_lbl_tg_summary,
            tg.daily_summary_enabled ? "Attivo" : "Disabilitato");
        lv_obj_set_style_text_color(s_lbl_tg_summary,
            tg.daily_summary_enabled ? CLR_GREEN : CLR_TEXT_DIM, 0);
    }

    /* ── Tab 3 – Manutenzione ─────────────────────────────────────── */
    {
        /* WiFi */
        lv_label_set_text(s_lbl_sys_wifi,
            wifi_manager_is_connected() ? "Connesso" : "Disconnesso");
        lv_obj_set_style_text_color(s_lbl_sys_wifi,
            wifi_manager_is_connected() ? CLR_GREEN : CLR_RED, 0);

        /* IP */
        char ip[16];
        wifi_manager_get_ip_str(ip, sizeof(ip));
        lv_label_set_text(s_lbl_sys_ip, ip);

        /* Heap */
        {
            uint32_t heap = esp_get_free_heap_size();
            char buf[24];
            snprintf(buf, sizeof(buf), "%lu KB",
                     (unsigned long)(heap / 1024));
            lv_label_set_text(s_lbl_sys_heap, buf);
        }

        /* Uptime */
        {
            uint32_t sec = (uint32_t)(xTaskGetTickCount() /
                                      configTICK_RATE_HZ);
            char buf[32];
            format_uptime(sec, buf, sizeof(buf));
            lv_label_set_text(s_lbl_sys_uptime, buf);
        }

        /* Auto-heater */
        {
            auto_heater_config_t ht = auto_heater_get_config();
            char buf[80];
            if (ht.enabled) {
                snprintf(buf, sizeof(buf),
                         "Attivo – %.1f °C (±%.1f) Relè %d",
                         (double)ht.target_temp_c,
                         (double)ht.hysteresis_c,
                         ht.relay_index + 1);
                lv_obj_set_style_text_color(s_lbl_heater, CLR_GREEN, 0);
            } else {
                snprintf(buf, sizeof(buf), "Disabilitato");
                lv_obj_set_style_text_color(s_lbl_heater, CLR_TEXT_DIM, 0);
            }
            lv_label_set_text(s_lbl_heater, buf);
        }

        /* DuckDNS */
        {
            duckdns_config_t ddns = duckdns_get_config();
            const char *status = duckdns_get_last_status();
            char buf[96];
            if (ddns.enabled) {
                snprintf(buf, sizeof(buf), "%s.duckdns.org – %s",
                         ddns.domain, status ? status : "?");
                lv_obj_set_style_text_color(s_lbl_ddns, CLR_GREEN, 0);
            } else {
                snprintf(buf, sizeof(buf), "Disabilitato");
                lv_obj_set_style_text_color(s_lbl_ddns, CLR_TEXT_DIM, 0);
            }
            lv_label_set_text(s_lbl_ddns, buf);
        }
    }

    display_unlock();
}
