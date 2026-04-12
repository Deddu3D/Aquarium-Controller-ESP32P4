/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – LVGL Dashboard UI implementation
 *
 * Creates and periodically refreshes the on-screen aquarium dashboard
 * shown on the 5″ MIPI DSI touch display (800×480).
 *
 * Layout (landscape, 800×480):
 * ┌──────────────────────────────────────────────────────┐
 * │  🐟 Aquarium Controller                   HH:MM:SS  │  ← header bar
 * ├──────────┬──────────┬──────────┬─────────────────────┤
 * │  🌡️      │  💡 LED  │  📶 WiFi │   Relay 1 [ON/OFF]  │
 * │  24.5 °C │ Daylight │  Conn.   │   Relay 2 [ON/OFF]  │
 * │          │          │          │   Relay 3 [ON/OFF]  │
 * │          │          │          │   Relay 4 [ON/OFF]  │
 * └──────────┴──────────┴──────────┴─────────────────────┘
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

#include "display_driver.h"
#include "display_ui.h"
#include "temperature_sensor.h"
#include "led_scenes.h"
#include "relay_controller.h"
#include "wifi_manager.h"

static const char *TAG = "disp_ui";

/* ── Colours ─────────────────────────────────────────────────────── */
#define CLR_BG_DARK      lv_color_hex(0x1a1a2e)
#define CLR_BG_CARD      lv_color_hex(0x16213e)
#define CLR_PRIMARY       lv_color_hex(0x0f3460)
#define CLR_ACCENT        lv_color_hex(0x00b4d8)
#define CLR_GREEN         lv_color_hex(0x06d6a0)
#define CLR_RED           lv_color_hex(0xef476f)
#define CLR_TEXT          lv_color_hex(0xedf2f4)
#define CLR_TEXT_DIM      lv_color_hex(0x8d99ae)

/* ── Label handles for dynamic refresh ───────────────────────────── */
static lv_obj_t *s_lbl_clock    = NULL;
static lv_obj_t *s_lbl_temp     = NULL;
static lv_obj_t *s_lbl_scene    = NULL;
static lv_obj_t *s_lbl_wifi     = NULL;
static lv_obj_t *s_lbl_relay[RELAY_COUNT] = {NULL};

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Create a rounded card container with dark background.
 */
static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, CLR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return card;
}

/**
 * @brief Create a label inside a parent with given font and colour.
 */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, colour, 0);
    return lbl;
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

    /* ── Header bar ───────────────────────────────────────────────── */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 800, 52);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, CLR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 16, 0);

    make_label(header, LV_SYMBOL_HOME "  Aquarium Controller",
               &lv_font_montserrat_18, CLR_TEXT);

    s_lbl_clock = lv_label_create(header);
    lv_label_set_text(s_lbl_clock, "--:--:--");
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_clock, CLR_ACCENT, 0);
    lv_obj_align(s_lbl_clock, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Content area (below header) ──────────────────────────────── */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, 780, 408);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 10, 0);

    /* ── Temperature card ─────────────────────────────────────────── */
    lv_obj_t *card_temp = make_card(body, 170, 390);
    make_label(card_temp, LV_SYMBOL_EYE_OPEN "  Temperature",
               &lv_font_montserrat_14, CLR_ACCENT);
    s_lbl_temp = make_label(card_temp, "-- °C",
                            &lv_font_montserrat_28, CLR_TEXT);

    /* ── LED Scene card ───────────────────────────────────────────── */
    lv_obj_t *card_led = make_card(body, 170, 390);
    make_label(card_led, LV_SYMBOL_IMAGE "  LED Scene",
               &lv_font_montserrat_14, CLR_ACCENT);
    s_lbl_scene = make_label(card_led, "Off",
                             &lv_font_montserrat_22, CLR_TEXT);

    /* ── WiFi card ────────────────────────────────────────────────── */
    lv_obj_t *card_wifi = make_card(body, 170, 390);
    make_label(card_wifi, LV_SYMBOL_WIFI "  WiFi",
               &lv_font_montserrat_14, CLR_ACCENT);
    s_lbl_wifi = make_label(card_wifi, "Disconnected",
                            &lv_font_montserrat_18, CLR_RED);

    /* ── Relays card ──────────────────────────────────────────────── */
    lv_obj_t *card_rel = make_card(body, 230, 390);
    make_label(card_rel, LV_SYMBOL_POWER "  Relays",
               &lv_font_montserrat_14, CLR_ACCENT);

    for (int i = 0; i < RELAY_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(card_rel);
        lv_obj_set_size(row, LV_PCT(100), 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);

        char buf[48];
        snprintf(buf, sizeof(buf), "Relay %d: --", i + 1);
        s_lbl_relay[i] = make_label(row, buf,
                                    &lv_font_montserrat_16, CLR_TEXT);
    }

    display_unlock();

    ESP_LOGI(TAG, "Dashboard UI created");
    return ESP_OK;
}

void display_ui_refresh(void)
{
    display_lock();

    if (!s_lbl_clock) {
        display_unlock();
        return;   /* UI not created yet */
    }

    /* ── Clock ────────────────────────────────────────────────────── */
    {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        lv_label_set_text(s_lbl_clock, buf);
    }

    /* ── Temperature ──────────────────────────────────────────────── */
    {
        float temp;
        if (temperature_sensor_get(&temp)) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f °C", (double)temp);
            lv_label_set_text(s_lbl_temp, buf);
            lv_obj_set_style_text_color(
                s_lbl_temp,
                (temp >= 24.0f && temp <= 28.0f) ? CLR_GREEN : CLR_RED, 0);
        } else {
            lv_label_set_text(s_lbl_temp, "-- °C");
            lv_obj_set_style_text_color(s_lbl_temp, CLR_TEXT_DIM, 0);
        }
    }

    /* ── LED scene ────────────────────────────────────────────────── */
    {
        led_scene_t scene = led_scenes_get();
        const char *name  = led_scenes_get_name(scene);
        lv_label_set_text(s_lbl_scene, name ? name : "Off");
    }

    /* ── WiFi ─────────────────────────────────────────────────────── */
    {
        if (wifi_manager_is_connected()) {
            lv_label_set_text(s_lbl_wifi, "Connected");
            lv_obj_set_style_text_color(s_lbl_wifi, CLR_GREEN, 0);
        } else {
            lv_label_set_text(s_lbl_wifi, "Disconnected");
            lv_obj_set_style_text_color(s_lbl_wifi, CLR_RED, 0);
        }
    }

    /* ── Relays ───────────────────────────────────────────────────── */
    {
        relay_state_t rels[RELAY_COUNT];
        relay_controller_get_all(rels);
        for (int i = 0; i < RELAY_COUNT; i++) {
            char buf[64];
            const char *name = rels[i].name[0] ? rels[i].name : "Relay";
            snprintf(buf, sizeof(buf), "%s %d: %s",
                     name, i + 1, rels[i].on ? "ON" : "OFF");
            lv_label_set_text(s_lbl_relay[i], buf);
            lv_obj_set_style_text_color(
                s_lbl_relay[i], rels[i].on ? CLR_GREEN : CLR_TEXT_DIM, 0);
        }
    }

    display_unlock();
}
