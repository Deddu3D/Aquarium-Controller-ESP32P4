/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Web Status Server implementation
 * Lightweight HTTP server serving a WiFi status dashboard.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "led_controller.h"
#include "led_schedule.h"
#include "temperature_sensor.h"
#include "temperature_history.h"
#include "telegram_notify.h"
#include "relay_controller.h"
#include "duckdns.h"
#include "ota_update.h"
#include "auto_heater.h"
#include "co2_controller.h"
#include "timezone_manager.h"
#include "esp_ota_ops.h"
#include "feeding_mode.h"
#include "led_scenes.h"
#include "daily_cycle.h"
#include "voice_control.h"

static const char *TAG = "web_srv";

/* Kconfig fallback for acclimatization ramp duration */
#ifndef CONFIG_LED_RAMP_DURATION_SEC
#define CONFIG_LED_RAMP_DURATION_SEC 30
#endif

static httpd_handle_t s_server = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Escape a string for safe inclusion in JSON output.
 *
 * Escapes quotes, backslashes, and control characters.
 */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < dst_size; i++) {
        char c = src[i];
        switch (c) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        default:
            if ((unsigned char)c < 0x20) {
                j += snprintf(dst + j, dst_size - j, "\\u%04x", (unsigned char)c);
            } else {
                dst[j++] = c;
            }
            break;
        }
    }
    dst[j] = '\0';
}

/**
 * @brief Fill a wifi_status_t struct with current WiFi information.
 */
typedef struct {
    bool  connected;
    char  ip[16];
    char  ssid[33];
    int   rssi;
} wifi_status_t;

static void get_wifi_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = wifi_manager_is_connected();

    if (out->connected) {
        /* IP address */
        wifi_manager_get_ip_str(out->ip, sizeof(out->ip));

        /* SSID + RSSI from driver */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strncpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid) - 1);
            out->rssi = ap.rssi;
        }
    }
}

/* ── HTML status page (/  GET) ───────────────────────────────────── */

/*
 * Buffer size for the rendered HTML page.  The template has grown
 * with mobile-optimized UI and temperature chart; 52 KiB gives
 * comfortable margin.
 */
#define HTML_BUF_SIZE          65536

/* JSON response buffer sizes */
#define JSON_STATUS_BUF_SIZE   384
#define JSON_LEDS_BUF_SIZE     256
#define JSON_SCHED_BUF_SIZE    768
#define JSON_TEMP_BUF_SIZE     128
#define JSON_TG_BUF_SIZE       768
#define JSON_DDNS_BUF_SIZE     384
#define JSON_RELAY_CHUNK_SIZE  512   /* larger to accommodate multi-slot schedules */
#define JSON_TEMP_CHUNK_SIZE   64
#define JSON_CO2_BUF_SIZE      256
#define JSON_TZ_BUF_SIZE       128
#define JSON_FEEDING_BUF_SIZE  192
#define JSON_SCENE_BUF_SIZE    256
#define JSON_DAILY_BUF_SIZE    256
#define JSON_VOICE_BUF_SIZE    1024

/* HTTP request body receive sizes */
#define POST_BODY_LED_SIZE      256
#define POST_BODY_SCHED_SIZE    768
#define POST_BODY_PRESETS_SIZE  768
#define POST_BODY_TG_SIZE       512
#define POST_BODY_RELAY_SIZE    512
#define POST_BODY_DDNS_SIZE     256
#define POST_BODY_CO2_SIZE      256
#define POST_BODY_TZ_SIZE       128
#define POST_BODY_FEEDING_SIZE  128
#define POST_BODY_SCENE_SIZE    256
#define POST_BODY_DAILY_SIZE    192
#define POST_BODY_VOICE_SIZE    384

/* HTTP server configuration */
#define HTTP_STACK_SIZE        8192
#define HTTP_MAX_URI_HANDLERS  50

static const char STATUS_HTML_TEMPLATE[] =
    "<!DOCTYPE html>"
    "<html lang=\"it\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta name=\"theme-color\" content=\"#0b1121\">"
    "<title>Aquarium Controller</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#0b1121;color:#e2e8f0;min-height:100vh}"
    ".wrap{max-width:960px;margin:0 auto;padding:.5rem .75rem 2.5rem}"
    "/* Tab bar */"
    ".tab-bar{display:flex;background:#111827;border-radius:8px 8px 0 0;"
    "border-bottom:2px solid #1e293b;margin-bottom:.75rem;overflow-x:auto}"
    ".tab{flex:1;padding:.65rem .4rem;background:none;border:none;"
    "color:#64748b;font-size:.8rem;font-weight:600;cursor:pointer;"
    "text-align:center;border-bottom:2px solid transparent;"
    "transition:color .2s,border-color .2s;white-space:nowrap}"
    ".tab:hover{color:#94a3b8}"
    ".tab.active{color:#38bdf8;border-bottom-color:#38bdf8;background:#1a2540}"
    "/* Panels */"
    ".panel{display:none}.panel.active{display:block}"
    "/* Two column grid */"
    ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}"
    "@media(max-width:700px){.grid2{grid-template-columns:1fr}}"
    "/* Cards */"
    ".card{background:#131c31;border:1px solid #1a2540;border-radius:10px;"
    "padding:1rem;margin-bottom:.75rem}"
    ".card-hdr{font-size:.9rem;font-weight:700;color:#38bdf8;"
    "margin-bottom:.75rem;display:flex;align-items:center;gap:.35rem}"
    ".card-hdr .icon{font-size:1rem}"
    "/* Collapsible card for inner panels */"
    ".ccard{background:#131c31;border:1px solid #1a2540;border-radius:10px;"
    "margin-bottom:.75rem;overflow:hidden}"
    ".ccard-hdr{display:flex;align-items:center;justify-content:space-between;"
    "padding:.85rem 1rem;cursor:pointer;user-select:none}"
    ".ccard-hdr h2{font-size:.9rem;color:#38bdf8;margin:0;font-weight:700}"
    ".ccard-hdr .arr{color:#475569;font-size:.65rem;transition:transform .25s}"
    ".ccard.open .arr{transform:rotate(180deg)}"
    ".ccard-body{max-height:0;overflow:hidden;transition:max-height .3s ease}"
    ".ccard.open .ccard-body{max-height:2000px}"
    ".ccard-inner{padding:0 1rem 1rem}"
    "/* Temperature display */"
    ".temp-big{font-size:2.8rem;font-weight:700;line-height:1.1}"
    ".temp-unit{font-size:1.2rem;color:#94a3b8;margin-left:.25rem}"
    ".temp-sub{font-size:.8rem;color:#64748b;margin-top:.15rem}"
    ".temp-status{display:flex;gap:1rem;align-items:center;margin-top:.4rem;"
    "font-size:.82rem;flex-wrap:wrap}"
    ".ok{color:#4ade80}.err{color:#f87171}.warn{color:#fbbf24}"
    "/* Sect titles */"
    ".sect-title{font-size:.85rem;color:#94a3b8;margin-top:.75rem;"
    "margin-bottom:.4rem;font-weight:600}"
    "/* Chart canvas */"
    "canvas{width:100%%;border-radius:6px;background:#0b1121}"
    "/* Sunlight bar */"
    ".sun-bar-wrap{margin-top:.5rem}"
    ".sun-label{font-size:.8rem;color:#94a3b8;margin-bottom:.3rem}"
    ".sun-bar{position:relative;height:28px;border-radius:6px;"
    "background:linear-gradient(90deg,#1a1206 0%%,#7c4a0a 20%%,#d97706 40%%,#f59e0b 50%%,#d97706 60%%,#7c4a0a 80%%,#1a1206 100%%);"
    "overflow:visible}"
    ".sun-dot{position:absolute;top:50%%;width:14px;height:14px;"
    "border-radius:50%%;background:#fbbf24;border:2px solid #fff;"
    "transform:translate(-50%%,-50%%);transition:left .5s}"
    "/* Rule box */"
    ".rule-box{background:#7c2d12;border:1px solid #9a3412;border-radius:8px;"
    "padding:.6rem .8rem;margin-top:.6rem;font-size:.82rem;color:#fed7aa}"
    ".rule-box.inactive{background:#1e293b;border-color:#334155;color:#64748b}"
    "/* CSV link */"
    ".csv-link{display:block;font-size:.78rem;color:#64748b;margin-top:.5rem;"
    "text-decoration:none}"
    ".csv-link:hover{color:#94a3b8}"
    "/* LED status */"
    ".led-status{display:flex;align-items:center;gap:.5rem;margin-bottom:.3rem}"
    ".led-dot{width:12px;height:12px;border-radius:50%%;background:#38bdf8}"
    ".led-name{font-size:1.1rem;font-weight:600}"
    ".led-bright{font-size:.85rem;color:#94a3b8;margin-bottom:.6rem}"
    ".led-color-row{display:flex;align-items:center;gap:.6rem;margin-bottom:.6rem}"
    ".color-swatch{width:32px;height:32px;border-radius:6px;border:2px solid #334155}"
    ".rgb-label{font-size:.85rem;color:#94a3b8}"
    "/* Buttons */"
    ".btn-row{display:flex;gap:.5rem}"
    ".btn-green{flex:1;padding:.6rem;border:none;border-radius:8px;"
    "background:#166534;color:#4ade80;font-weight:700;font-size:.9rem;"
    "cursor:pointer;text-align:center;transition:background .15s}"
    ".btn-green:hover{background:#15803d}"
    ".btn-red{flex:1;padding:.6rem;border:none;border-radius:8px;"
    "background:#7f1d1d;color:#f87171;font-weight:700;font-size:.9rem;"
    "cursor:pointer;text-align:center;transition:background .15s}"
    ".btn-red:hover{background:#991b1b}"
    ".btn{display:block;width:100%%;padding:.7rem;margin-top:.65rem;"
    "background:#38bdf8;color:#0f172a;border:none;border-radius:8px;"
    "font-size:.9rem;font-weight:600;cursor:pointer;text-align:center;"
    "transition:background .15s}"
    ".btn:active{background:#7dd3fc}"
    ".btn-sm{background:#1e293b;color:#e2e8f0;font-size:.82rem;padding:.55rem;"
    "border:1px solid #334155}"
    ".btn-sm:active{background:#334155}"
    "/* Relay dashboard */"
    ".relay-grid{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}"
    ".relay-item{display:flex;flex-direction:column;gap:.2rem}"
    ".relay-row{display:flex;align-items:center;justify-content:space-between;gap:.4rem}"
    ".relay-name{font-size:.85rem;font-weight:600}"
    ".badge{display:inline-block;padding:.15rem .5rem;border-radius:6px;"
    "font-size:.75rem;font-weight:700;min-width:38px;text-align:center}"
    ".badge-on{background:#166534;color:#4ade80}"
    ".badge-off{background:#1e293b;color:#64748b;border:1px solid #334155}"
    ".relay-detail{font-size:.72rem;color:#475569}"
    "/* Rows (for form fields) */"
    ".row{display:flex;justify-content:space-between;align-items:center;"
    "padding:.55rem 0;border-bottom:1px solid #1a2540;gap:.5rem}"
    ".row:last-child{border-bottom:none}"
    ".label{color:#94a3b8;font-size:.85rem;flex-shrink:0}"
    ".value{font-weight:600;font-size:.9rem;text-align:right}"
    "/* Section dividers */"
    ".sect{font-size:.85rem;color:#38bdf8;padding:.6rem 0 .25rem;"
    "border-bottom:1px solid #1a2540;margin-top:.2rem}"
    "/* Toggle */"
    ".toggle{position:relative;width:48px;height:26px;flex-shrink:0}"
    ".toggle input{opacity:0;width:0;height:0}"
    ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;"
    "background:#475569;border-radius:26px;transition:.25s}"
    ".slider:before{content:'';position:absolute;height:20px;width:20px;"
    "left:3px;bottom:3px;background:#fff;border-radius:50%%;transition:.25s}"
    ".toggle input:checked+.slider{background:#4ade80}"
    ".toggle input:checked+.slider:before{transform:translateX(22px)}"
    "/* Inputs */"
    ".fin{width:100%%;background:#1e293b;color:#e2e8f0;border:1px solid #334155;"
    "border-radius:8px;padding:.55rem .7rem;font-size:.9rem;"
    "text-align:right;appearance:none;-webkit-appearance:none}"
    ".fin:focus{outline:none;border-color:#38bdf8}"
    ".fin-wide{text-align:left}"
    "input[type=range]{width:100%%;height:32px;accent-color:#38bdf8}"
    "input[type=color]{width:48px;height:32px;border:none;border-radius:8px;"
    "cursor:pointer;background:none;padding:0}"
    "select.fin{padding-right:1.5rem}"
    "/* LED preview */"
    ".led-preview{width:100%%;height:24px;border-radius:6px;"
    "margin-top:.4rem;border:1px solid #1a2540}"
    "/* Status bar */"
    ".status-bar{position:fixed;bottom:0;left:0;right:0;"
    "background:#111827;border-top:1px solid #1e293b;"
    "padding:.4rem 1rem;font-size:.72rem;color:#475569;"
    "text-align:center;z-index:50}"
    "/* Toast */"
    ".toast{position:fixed;bottom:2.5rem;left:50%%;transform:translateX(-50%%);"
    "background:#1e293b;color:#e2e8f0;padding:.6rem 1.1rem;border-radius:8px;"
    "font-size:.85rem;box-shadow:0 4px 16px rgba(0,0,0,.5);z-index:99;"
    "opacity:0;transition:opacity .3s;pointer-events:none}"
    ".toast.show{opacity:1}"
    ".toast.ok{border-left:3px solid #4ade80}"
    ".toast.fail{border-left:3px solid #f87171}"
    "/* OTA progress */"
    ".ota-bar{width:100%%;height:8px;background:#1e293b;border-radius:4px;"
    "margin-top:.3rem;overflow:hidden}"
    ".ota-fill{height:100%%;background:#38bdf8;border-radius:4px;"
    "transition:width .3s;width:0%%}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"wrap\">"
    "<!-- Tab bar -->"
    "<div class=\"tab-bar\">"
    "<button class=\"tab active\" onclick=\"switchTab(0)\">&#x26A0; Riepilogo</button>"
    "<button class=\"tab\" onclick=\"switchTab(1)\">&#x25B3; LED Strip</button>"
    "<button class=\"tab\" onclick=\"switchTab(2)\">&#x1F514; Telegram</button>"
    "<button class=\"tab\" onclick=\"switchTab(3)\">&#x2699; Manutenzione</button>"
    "<button class=\"tab\" onclick=\"switchTab(4)\">&#x1F3A4; Voce</button>"
    "</div>"
    ""
    "<!-- ═══ Panel 0: Riepilogo ═══ -->"
    "<div class=\"panel active\" id=\"p0\">"
    "<div class=\"grid2\">"
    "<!-- Left column -->"
    "<div>"
    "<div class=\"card\">"
    "<div class=\"card-hdr\"><span class=\"icon\">&#x26A0;</span> TEMPERATURA</div>"
    "<div class=\"temp-big\" id=\"temp-val\">--.-"
    "<span class=\"temp-unit\">&#xB0;C</span></div>"
    "<div class=\"temp-sub\">Agg. ogni 5 s</div>"
    "<div class=\"temp-status\">"
    "<span id=\"sensor-status\" class=\"ok\">&#x2713; Sensore: --</span>"
    "<span id=\"wifi-rssi\">&#x1F4F6; WiFi: -- dBm</span>"
    "</div>"
    "<div class=\"sect-title\">Storico 24 h</div>"
    "<canvas id=\"temp-chart\" width=\"440\" height=\"180\""
    " style=\"height:180px\"></canvas>"
    "<div class=\"rule-box inactive\" id=\"heater-rule\">"
    "&#x2699; Nessuna regola attiva</div>"
    "<a class=\"csv-link\" href=\"/api/temperature/export.csv\">"
    "&#x2B07; Esporta storico CSV: /api/temperature/export.csv</a>"
    "</div>"
    "</div>"
    "<!-- Right column -->"
    "<div>"
    "<div class=\"card\">"
    "<div class=\"card-hdr\"><span class=\"icon\">&#x25B3;</span> STATO LUCI</div>"
    "<div class=\"led-status\">"
    "<span class=\"led-dot\" id=\"led-dot\"></span>"
    "<span class=\"led-name\" id=\"led-status-name\">Off</span>"
    "</div>"
    "<div class=\"led-bright\" id=\"led-bright\">Luminosit&#xE0; --%%</div>"
    "<div class=\"led-color-row\">"
    "<div class=\"color-swatch\" id=\"color-swatch\"></div>"
    "<span class=\"rgb-label\" id=\"rgb-label\">RGB: -- , -- , --</span>"
    "</div>"
    "<div class=\"btn-row\">"
    "<button class=\"btn-green\" onclick=\"ledQuickOn()\">&#x2713; Accendi</button>"
    "<button class=\"btn-red\" onclick=\"ledQuickOff()\">&#x2717; Spegni</button>"
    "</div>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"card-hdr\"><span class=\"icon\">&#x26A1;</span> REL&#xC8;</div>"
    "<div class=\"relay-grid\" id=\"relay-dash\"></div>"
    "</div>"
    "</div>"
    "</div>"
    "<!-- Feeding mode card (full-width) -->"
    "<div class=\"card\" id=\"feeding-card\">"
    "<div class=\"card-hdr\"><span class=\"icon\">&#x1F41F;</span> PAUSA ALIMENTAZIONE</div>"
    "<div id=\"feeding-status\" style=\"font-size:.85rem;color:#64748b;margin-bottom:.5rem\">"
    "Stato: <span id=\"feeding-state\" class=\"ok\">Inattiva</span></div>"
    "<div id=\"feeding-countdown\" style=\"font-size:1.6rem;font-weight:700;"
    "color:#fbbf24;display:none\">&#x23F1; <span id=\"feeding-remain\">--:--</span></div>"
    "<div class=\"btn-row\" style=\"margin-top:.5rem\">"
    "<button class=\"btn-green\" id=\"btn-feed-start\" onclick=\"startFeeding()\">&#x1F41F; Avvia</button>"
    "<button class=\"btn-red\" id=\"btn-feed-stop\" onclick=\"stopFeeding()\" style=\"display:none\">&#x23F9; Ferma</button>"
    "</div>"
    "</div>"
    "</div>"
    "<!-- ═══ Panel 1: LED Strip ═══ -->"
    "<div class=\"panel\" id=\"p1\">"
    "<div class=\"ccard open\" id=\"led-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4A1; Controllo LED</h2><span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Power</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"led-on\""
    " onchange=\"sendLed()\"><span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0;</span>"
    "<span class=\"value\" id=\"br-val\">128</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"led-br\" min=\"0\""
    " max=\"255\" value=\"128\" oninput=\"document.getElementById("
    "'br-val').textContent=this.value\" onchange=\"sendLed()\"></div>"
    "<div class=\"row\"><span class=\"label\">Colore</span>"
    "<input type=\"color\" id=\"led-color\" value=\"#ffffff\""
    " onchange=\"sendLed()\"></div>"
    "<div class=\"led-preview\" id=\"led-preview\"></div>"
    "</div></div></div>"
    "<!-- LED Schedule -->"
    "<div class=\"ccard\" id=\"sched-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x23F0; Programmazione LED</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Abilitata</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"sched-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"sect\">&#x2600;&#xFE0F; Accensione</div>"
    "<div class=\"row\"><span class=\"label\">Orario accensione</span>"
    "<input type=\"time\" class=\"fin\" id=\"sched-on\""
    " style=\"max-width:130px\" value=\"08:00\"></div>"
    "<div class=\"row\"><span class=\"label\">Rampa colore (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sched-ramp\" min=\"0\" max=\"120\""
    " value=\"30\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0; giorno</span>"
    "<span class=\"value\" id=\"sched-br-val\">255</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"sched-br\" min=\"0\""
    " max=\"255\" value=\"255\" oninput=\"$('sched-br-val').textContent=this.value\"></div>"
    "<div class=\"row\"><span class=\"label\">Colore giorno</span>"
    "<input type=\"color\" id=\"sched-color\" value=\"#c8dcff\"></div>"
    "<div class=\"sect\">&#x1F31D; Pausa</div>"
    "<div class=\"row\"><span class=\"label\">Pausa attiva</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"sched-pause-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Inizio pausa</span>"
    "<input type=\"time\" class=\"fin\" id=\"sched-pause-start\""
    " style=\"max-width:130px\" value=\"12:00\"></div>"
    "<div class=\"row\"><span class=\"label\">Fine pausa</span>"
    "<input type=\"time\" class=\"fin\" id=\"sched-pause-end\""
    " style=\"max-width:130px\" value=\"14:00\"></div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0; pausa</span>"
    "<span class=\"value\" id=\"sched-pbr-val\">80</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"sched-pbr\" min=\"0\""
    " max=\"255\" value=\"80\" oninput=\"$('sched-pbr-val').textContent=this.value\"></div>"
    "<div class=\"row\"><span class=\"label\">Colore pausa</span>"
    "<input type=\"color\" id=\"sched-pcolor\" value=\"#c8dcff\"></div>"
    "<div class=\"sect\">&#x1F317; Spegnimento</div>"
    "<div class=\"row\"><span class=\"label\">Orario spegnimento</span>"
    "<input type=\"time\" class=\"fin\" id=\"sched-off\""
    " style=\"max-width:130px\" value=\"22:00\"></div>"
    "<button class=\"btn\" onclick=\"saveSched()\">Salva Programmazione</button>"
    "</div></div></div>"
    "<!-- Preset -->"
    "<div class=\"ccard\" id=\"preset-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4BE; Preset</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div id=\"preset-rows\"></div>"
    "</div></div></div>"
    "<!-- Scenes -->"
    "<div class=\"ccard\" id=\"scene-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F305; Scene LED</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Scena attiva</span>"
    "<select class=\"fin\" id=\"scene-sel\" style=\"max-width:160px\">"
    "<option value=\"0\">Nessuna</option>"
    "<option value=\"1\">&#x1F305; Alba</option>"
    "<option value=\"2\">&#x1F307; Tramonto</option>"
    "<option value=\"3\">&#x1F319; Chiaro di Luna</option>"
    "<option value=\"4\">&#x26C8; Temporale</option>"
    "<option value=\"5\">&#x2601; Nuvole</option>"
    "</select></div>"
    "<div class=\"sect\">&#x1F305; Alba / &#x1F307; Tramonto</div>"
    "<div class=\"row\"><span class=\"label\">Durata alba (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-sr-dur\""
    " min=\"5\" max=\"120\" value=\"30\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0; massima</span>"
    "<span class=\"value\" id=\"sc-sr-br-val\">255</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"sc-sr-br\" min=\"0\" max=\"255\" value=\"255\""
    " oninput=\"$('sc-sr-br-val').textContent=this.value\"></div>"
    "<div class=\"row\"><span class=\"label\">Durata tramonto (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-ss-dur\""
    " min=\"5\" max=\"120\" value=\"30\" style=\"max-width:80px\"></div>"
    "<div class=\"sect\">&#x1F319; Chiaro di Luna</div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0; luna</span>"
    "<span class=\"value\" id=\"sc-ml-br-val\">15</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"sc-ml-br\" min=\"0\" max=\"60\" value=\"15\""
    " oninput=\"$('sc-ml-br-val').textContent=this.value\"></div>"
    "<div class=\"row\"><span class=\"label\">Colore luna</span>"
    "<input type=\"color\" id=\"sc-ml-color\" value=\"#14285a\"></div>"
    "<div class=\"sect\">&#x26C8; Temporale / &#x2601; Nuvole</div>"
    "<div class=\"row\"><span class=\"label\">Intensit&#xE0; temporale %%</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-storm\""
    " min=\"0\" max=\"100\" value=\"70\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Profondit&#xE0; nuvole %%</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-cloud-d\""
    " min=\"0\" max=\"80\" value=\"40\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Periodo nuvole (s)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-cloud-p\""
    " min=\"10\" max=\"600\" value=\"120\" style=\"max-width:90px\"></div>"
    "<button class=\"btn btn-sm\" onclick=\"saveScene()\" style=\"margin-top:.4rem\">"
    "&#x1F4BE; Salva Impostazioni Scena</button>"
    "<button class=\"btn\" onclick=\"applyScene()\" style=\"margin-top:.4rem\">"
    "&#x25B6; Avvia Scena Selezionata</button>"
    "</div></div></div>"
    "<!-- Daily Cycle -->"
    "<div class=\"ccard\" id=\"daily-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x2600;&#xFE0F; Giornata Naturale</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"dc-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Latitudine (°)</span>"
    "<input type=\"number\" class=\"fin\" id=\"dc-lat\""
    " step=\"0.0001\" min=\"-90\" max=\"90\" value=\"45.46\""
    " style=\"max-width:110px\"></div>"
    "<div class=\"row\"><span class=\"label\">Longitudine (°)</span>"
    "<input type=\"number\" class=\"fin\" id=\"dc-lon\""
    " step=\"0.0001\" min=\"-180\" max=\"180\" value=\"9.19\""
    " style=\"max-width:110px\"></div>"
    "<div class=\"sect\">&#x1F305; Orari calcolati oggi</div>"
    "<div class=\"row\"><span class=\"label\">Alba</span>"
    "<span class=\"value\" id=\"dc-sunrise\">--:--</span></div>"
    "<div class=\"row\"><span class=\"label\">Tramonto</span>"
    "<span class=\"value\" id=\"dc-sunset\">--:--</span></div>"
    "<div class=\"row\"><span class=\"label\">Fase attuale</span>"
    "<span class=\"value\" id=\"dc-phase\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"saveDailyCycle()\" style=\"margin-top:.4rem\">"
    "&#x1F4BE; Salva Giornata Naturale</button>"
    "</div></div></div>"
    "</div>"
    ""
    "<!-- ═══ Panel 2: Telegram ═══ -->"
    "<div class=\"panel\" id=\"p2\">"
    "<div class=\"ccard open\" id=\"tg-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4F1; Telegram</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Bot Token</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"password\" class=\"fin fin-wide\" id=\"tg-token\""
    " placeholder=\"Non configurato\"></div>"
    "<div class=\"row\"><span class=\"label\">Chat ID</span>"
    "<input type=\"text\" class=\"fin\" id=\"tg-chatid\""
    " placeholder=\"-100123456\" style=\"max-width:140px\"></div>"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<button class=\"btn btn-sm\" onclick=\"testTg()\">"
    "&#x1F4E8; Invia Messaggio Test</button>"
    "<div class=\"sect\">&#x1F321;&#xFE0F; Allarmi Temperatura</div>"
    "<div class=\"row\"><span class=\"label\">Abilitati</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-talm\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Alta &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-thigh\""
    " step=\"0.5\" min=\"-10\" max=\"50\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Bassa &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-tlow\""
    " step=\"0.5\" min=\"-10\" max=\"50\" style=\"max-width:90px\"></div>"
    "<div class=\"sect\">&#x1F4A7; Cambio Acqua</div>"
    "<div class=\"row\"><span class=\"label\">Promemoria</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-wc\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Intervallo (giorni)</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-wcdays\""
    " min=\"1\" max=\"90\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Ultimo cambio</span>"
    "<span class=\"value\" id=\"tg-wclast\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"resetWc()\">"
    "&#x2705; Registra Cambio Acqua</button>"
    "<div class=\"sect\">&#x1F33F; Fertilizzante</div>"
    "<div class=\"row\"><span class=\"label\">Promemoria</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-fert\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Intervallo (giorni)</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-fertdays\""
    " min=\"1\" max=\"90\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Ultima dose</span>"
    "<span class=\"value\" id=\"tg-fertlast\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"resetFert()\">"
    "&#x2705; Registra Dose Fertilizzante</button>"
    "<div class=\"sect\">&#x1F4CA; Riepilogo Giornaliero</div>"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-sum\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Ora invio</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-sumhr\""
    " min=\"0\" max=\"23\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Notifiche Rel&#xE8;</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-rel\">"
    "<span class=\"slider\"></span></label></div>"
    "<button class=\"btn\" onclick=\"saveTg()\">Salva Impostazioni</button>"
    "</div></div></div>"
    "</div>"
    ""
    "<!-- ═══ Panel 3: Manutenzione ═══ -->"
    "<div class=\"panel\" id=\"p3\">"
    "<!-- System Status -->"
    "<div class=\"ccard open\" id=\"sys-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4CA; Stato Sistema</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Connessione</span>"
    "<span class=\"value\" id=\"sys-conn\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">IP</span>"
    "<span class=\"value\" id=\"sys-ip\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">SSID</span>"
    "<span class=\"value\" id=\"sys-ssid\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">RSSI</span>"
    "<span class=\"value\" id=\"sys-rssi\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">Heap Libero</span>"
    "<span class=\"value\" id=\"sys-heap\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">Uptime</span>"
    "<span class=\"value\" id=\"sys-uptime\">--</span></div>"
    "</div></div></div>"
    "<!-- OTA Update -->"
    "<div class=\"ccard\" id=\"ota-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F504; Aggiornamento OTA</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">URL Firmware</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"text\" class=\"fin fin-wide\" id=\"ota-url\""
    " placeholder=\"https://example.com/firmware.bin\"></div>"
    "<div class=\"row\"><span class=\"label\">Stato</span>"
    "<span class=\"value\" id=\"ota-status\">idle</span></div>"
    "<div class=\"ota-bar\"><div class=\"ota-fill\" id=\"ota-fill\"></div></div>"
    "<button class=\"btn\" onclick=\"startOta()\">Avvia Aggiornamento</button>"
    "</div></div></div>"
    "<!-- Auto-Heater -->"
    "<div class=\"ccard\" id=\"heater-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F321;&#xFE0F; Auto-Riscaldatore</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"ht-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Canale Rel&#xE8;</span>"
    "<input type=\"number\" class=\"fin\" id=\"ht-relay\""
    " min=\"0\" max=\"3\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Temperatura Target &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"ht-target\""
    " step=\"0.5\" min=\"15\" max=\"35\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Isteresi &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"ht-hyst\""
    " step=\"0.1\" min=\"0.1\" max=\"3\" style=\"max-width:90px\"></div>"
    "<button class=\"btn\" onclick=\"saveHeater()\">Salva Impostazioni</button>"
    "</div></div></div>"
    "<!-- DuckDNS -->"
    "<div class=\"ccard\" id=\"ddns-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F310; DuckDNS</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Dominio</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"text\" class=\"fin fin-wide\" id=\"ddns-domain\""
    " placeholder=\"myaquarium\"></div>"
    "<div class=\"row\" style=\"padding-top:0\">"
    "<span class=\"label\" style=\"font-size:.75rem;color:#475569\">"
    ".duckdns.org</span></div>"
    "<div class=\"row\"><span class=\"label\">Token</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"password\" class=\"fin fin-wide\" id=\"ddns-token\""
    " placeholder=\"Non configurato\"></div>"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"ddns-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Ultimo stato</span>"
    "<span class=\"value\" id=\"ddns-status\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"testDdns()\">"
    "&#x1F504; Aggiorna Ora</button>"
    "<button class=\"btn\" onclick=\"saveDdns()\">Salva Impostazioni</button>"
    "</div></div></div>"
    "<!-- Relay schedules -->"
    "<div class=\"ccard\" id=\"relay-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F50C; Rel&#xE8; &amp; Programmazione</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div id=\"relay-rows\"></div>"
    "</div></div></div>"
    "<!-- CO2 Controller -->"
    "<div class=\"ccard\" id=\"co2-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4A8; CO&#x2082; Controller</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"co2-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Canale Rel&#xE8;</span>"
    "<input type=\"number\" class=\"fin\" id=\"co2-relay\""
    " min=\"0\" max=\"3\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Apertura anticipo (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"co2-pre\""
    " min=\"0\" max=\"60\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Chiusura posticipo (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"co2-post\""
    " min=\"0\" max=\"60\" style=\"max-width:80px\"></div>"
    "<button class=\"btn\" onclick=\"saveCo2()\">Salva CO&#x2082;</button>"
    "</div></div></div>"
    "<!-- Feeding mode settings -->"
    "<div class=\"ccard\" id=\"feed-cfg-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F41F; Alimentazione - Impostazioni</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Canale Rel&#xE8; da pausare</span>"
    "<input type=\"number\" class=\"fin\" id=\"feed-relay\""
    " min=\"-1\" max=\"3\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Durata pausa (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"feed-dur\""
    " min=\"1\" max=\"60\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Abbassa luci</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"feed-dim\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Luminosit&#xE0; alimentazione</span>"
    "<span class=\"value\" id=\"feed-dim-br-val\">60</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"feed-dim-br\" min=\"0\" max=\"255\" value=\"60\""
    " oninput=\"$('feed-dim-br-val').textContent=this.value\"></div>"
    "<button class=\"btn\" onclick=\"saveFeeding()\">Salva Impostazioni</button>"
    "</div></div></div>"
    "<!-- Timezone -->"
    "<div class=\"ccard\" id=\"tz-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F30D; Fuso Orario</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">POSIX TZ</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"text\" class=\"fin fin-wide\" id=\"tz-str\""
    " placeholder=\"CET-1CEST,M3.5.0/2,M10.5.0/3\"></div>"
    "<div class=\"row\"><span class=\"label\">Preset</span>"
    "<select class=\"fin\" id=\"tz-preset\" onchange=\"tzPreset()\" style=\"max-width:180px\">"
    "<option value=\"\">-- Scegli --</option>"
    "<option value=\"CET-1CEST,M3.5.0/2,M10.5.0/3\">Italia (CET)</option>"
    "<option value=\"GMT0BST,M3.5.0/1,M10.5.0\">UK (GMT/BST)</option>"
    "<option value=\"EET-2EEST,M3.5.0/3,M10.5.0/4\">Grecia/Romania (EET)</option>"
    "<option value=\"WET0WEST,M3.5.0/1,M10.5.0\">Portogallo (WET)</option>"
    "<option value=\"EST5EDT,M3.2.0,M11.1.0\">Est USA (EST)</option>"
    "<option value=\"CST6CDT,M3.2.0,M11.1.0\">Centro USA (CST)</option>"
    "<option value=\"MST7MDT,M3.2.0,M11.1.0\">Montagna USA (MST)</option>"
    "<option value=\"PST8PDT,M3.2.0,M11.1.0\">Ovest USA (PST)</option>"
    "<option value=\"JST-9\">Giappone (JST)</option>"
    "<option value=\"AEST-10AEDT,M10.1.0,M4.1.0/3\">Sydney (AEST)</option>"
    "</select></div>"
    "<button class=\"btn\" onclick=\"saveTz()\">Salva Fuso Orario</button>"
    "</div></div></div>"
    "</div>"
    "<!-- ═══ Panel 4: Voce ═══ -->"
    "<div class=\"panel\" id=\"p4\">"
    "<!-- Voice configuration -->"
    "<div class=\"ccard open\" id=\"voice-cfg-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F511; Configurazione Groq API</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Abilitato</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"vc-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Groq API Key</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"password\" class=\"fin fin-wide\" id=\"vc-key\""
    " placeholder=\"sk-...\"></div>"
    "<div class=\"row\"><span class=\"label\">Modello STT</span>"
    "<input type=\"text\" class=\"fin\" id=\"vc-stt\""
    " style=\"max-width:220px\""
    " placeholder=\"whisper-large-v3-turbo\"></div>"
    "<div class=\"row\"><span class=\"label\">Modello LLM</span>"
    "<input type=\"text\" class=\"fin\" id=\"vc-llm\""
    " style=\"max-width:220px\""
    " placeholder=\"llama-3.1-8b-instant\"></div>"
    "<div class=\"row\"><span class=\"label\">Durata registrazione (ms)</span>"
    "<input type=\"number\" class=\"fin\" id=\"vc-rec\""
    " min=\"1000\" max=\"10000\" step=\"500\" style=\"max-width:100px\"></div>"
    "<div class=\"row\"><span class=\"label\">GPIO SCK (BCLK)</span>"
    "<input type=\"number\" class=\"fin\" id=\"vc-sck\""
    " min=\"0\" max=\"54\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">GPIO WS (LRCLK)</span>"
    "<input type=\"number\" class=\"fin\" id=\"vc-ws\""
    " min=\"0\" max=\"54\" style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">GPIO SD (DIN)</span>"
    "<input type=\"number\" class=\"fin\" id=\"vc-sd\""
    " min=\"0\" max=\"54\" style=\"max-width:80px\"></div>"
    "<button class=\"btn\" onclick=\"saveVoiceConfig()\">Salva Configurazione</button>"
    "</div></div></div>"
    "<!-- Voice recording -->"
    "<div class=\"ccard open\" id=\"voice-rec-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F3A4; Controllo Vocale</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div style=\"font-size:.82rem;color:#64748b;margin-bottom:.75rem\">"
    "Parla dopo aver premuto il pulsante. Comandi supportati:<br>"
    "<span style=\"color:#94a3b8\">"
    "&#x2022; &quot;Accendi/spegni il filtro&quot; &#x2022; "
    "&quot;Imposta luminosit&#xE0; al 50%%&quot; &#x2022; "
    "&quot;Avvia la scena luna piena&quot;<br>"
    "&#x2022; &quot;Pausa alimentazione&quot; &#x2022; "
    "&quot;Abilita/disabilita ciclo giornaliero&quot;"
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Stato</span>"
    "<span class=\"value\" id=\"vc-status\">idle</span></div>"
    "<div id=\"vc-transcript-row\" class=\"row\" style=\"display:none\">"
    "<span class=\"label\">Trascrizione</span>"
    "<span class=\"value\" id=\"vc-transcript\" style=\"color:#fbbf24\">--</span>"
    "</div>"
    "<div id=\"vc-command-row\" class=\"row\" style=\"display:none\">"
    "<span class=\"label\">Comando</span>"
    "<span class=\"value\" id=\"vc-command\" style=\"color:#38bdf8;font-size:.78rem\">--</span>"
    "</div>"
    "<div id=\"vc-result-row\" class=\"row\" style=\"display:none\">"
    "<span class=\"label\">Risultato</span>"
    "<span class=\"value\" id=\"vc-result\">--</span>"
    "</div>"
    "<button class=\"btn\" id=\"btn-vc-rec\" onclick=\"startVoiceRec()\">"
    "&#x1F3A4; Avvia Registrazione</button>"
    "</div></div></div>"
    "</div>"
    "<!-- end panels -->"
    "</div>"
    "<!-- Status bar -->"
    "<div class=\"status-bar\">"
    "IP: %s &middot; NTP: %s &middot; Uptime: %s &middot; Partition: %s"
    "</div>"
    "<!-- Toast -->"
    "<div class=\"toast\" id=\"toast\"></div>"
    "<!-- Scripts -->"
    "<script>"
    "function $(i){return document.getElementById(i)}"
    "function tog(hdr){hdr.parentElement.classList.toggle('open')}"
    "function toast(msg,ok){"
    "  var t=$('toast');t.textContent=msg;"
    "  t.className='toast '+(ok?'ok':'fail')+' show';"
    "  clearTimeout(t._t);t._t=setTimeout(function(){"
    "    t.className='toast'},2500)}"
    "/* ── Tab switching ── */"
    "var _tab=0;"
    "function switchTab(n){"
    "  var tabs=document.querySelectorAll('.tab');"
    "  for(var i=0;i<tabs.length;i++){"
    "    $('p'+i).classList.remove('active');"
    "    tabs[i].classList.remove('active')}"
    "  $('p'+n).classList.add('active');"
    "  tabs[n].classList.add('active');"
    "  _tab=n;"
    "  if(n===0){loadDash();loadFeeding()}"
    "  if(n===1){loadLeds();loadSched();loadPresets();loadSceneConfig();loadDailyCycle()}"
    "  if(n===2){loadTg()}"
    "  if(n===3){loadSys();loadDdns();loadOtaStatus();loadHeater();loadRelays();loadCo2();loadTimezone();loadFeedingConfig()}"
    "  if(n===4){loadVoiceConfig()}}"
    "/* ── Color helpers ── */"
    "function hexToRgb(h){"
    "  return{r:parseInt(h.slice(1,3),16),"
    "  g:parseInt(h.slice(3,5),16),"
    "  b:parseInt(h.slice(5,7),16)}}"
    "function rgbToHex(r,g,b){"
    "  return'#'+[r,g,b].map(function(x){"
    "    var h=x.toString(16);return h.length===1?'0'+h:h}).join('')}"
    "function updatePreview(){"
    "  var on=$('led-on').checked;"
    "  var br=parseInt($('led-br').value);"
    "  var c=hexToRgb($('led-color').value);"
    "  if(!on){$('led-preview').style.background='#1e293b';return}"
    "  var s=br/255;"
    "  $('led-preview').style.background="
    "    'rgb('+Math.round(c.r*s)+','+Math.round(c.g*s)+','+"
    "    Math.round(c.b*s)+')'}"
    "/* ── LED control ── */"
    "function sendLed(){"
    "  updatePreview();"
    "  var on=$('led-on').checked;"
    "  var br=parseInt($('led-br').value);"
    "  var c=hexToRgb($('led-color').value);"
    "  fetch('/api/leds',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({on:on,brightness:br,r:c.r,g:c.g,b:c.b})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('LED aggiornato',1)}).catch(function(){"
    "    toast('Errore LED',0)})}"
    "function loadLeds(){"
    "  fetch('/api/leds').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('led-on').checked=d.on;"
    "    $('led-br').value=d.brightness;"
    "    $('br-val').textContent=d.brightness;"
    "    $('led-color').value=rgbToHex(d.r,d.g,d.b);"
    "    updatePreview()})}"
    "/* ── LED Schedule ── */"
    "function pad(v){return v<10?'0'+v:''+v}"
    "function loadSched(){"
    "  fetch('/api/led_schedule').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('sched-en').checked=d.enabled;"
    "    $('sched-on').value=pad(d.on_hour)+':'+pad(d.on_minute);"
    "    $('sched-ramp').value=d.ramp_duration_min;"
    "    $('sched-br').value=d.brightness;"
    "    $('sched-br-val').textContent=d.brightness;"
    "    $('sched-color').value=rgbToHex(d.red,d.green,d.blue);"
    "    $('sched-pause-en').checked=d.pause_enabled;"
    "    $('sched-pause-start').value=pad(d.pause_start_hour)+':'+pad(d.pause_start_minute);"
    "    $('sched-pause-end').value=pad(d.pause_end_hour)+':'+pad(d.pause_end_minute);"
    "    $('sched-pbr').value=d.pause_brightness;"
    "    $('sched-pbr-val').textContent=d.pause_brightness;"
    "    $('sched-pcolor').value=rgbToHex(d.pause_red,d.pause_green,d.pause_blue);"
    "    $('sched-off').value=pad(d.off_hour)+':'+pad(d.off_minute)})}"
    "function saveSched(){"
    "  var onT=$('sched-on').value.split(':');"
    "  var offT=$('sched-off').value.split(':');"
    "  var psT=$('sched-pause-start').value.split(':');"
    "  var peT=$('sched-pause-end').value.split(':');"
    "  var c=hexToRgb($('sched-color').value);"
    "  var pc=hexToRgb($('sched-pcolor').value);"
    "  var data={"
    "    enabled:$('sched-en').checked,"
    "    on_hour:parseInt(onT[0]),on_minute:parseInt(onT[1]),"
    "    ramp_duration_min:parseInt($('sched-ramp').value),"
    "    pause_enabled:$('sched-pause-en').checked,"
    "    pause_start_hour:parseInt(psT[0]),pause_start_minute:parseInt(psT[1]),"
    "    pause_end_hour:parseInt(peT[0]),pause_end_minute:parseInt(peT[1]),"
    "    pause_brightness:parseInt($('sched-pbr').value),"
    "    pause_red:pc.r,pause_green:pc.g,pause_blue:pc.b,"
    "    off_hour:parseInt(offT[0]),off_minute:parseInt(offT[1]),"
    "    brightness:parseInt($('sched-br').value),"
    "    red:c.r,green:c.g,blue:c.b};"
    "  fetch('/api/led_schedule',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Programmazione salvata',1)}).catch(function(){"
    "    toast('Errore salvataggio',0)})}"
    "/* ── LED Presets ── */"
    "function loadPresets(){"
    "  fetch('/api/led_presets').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var c=$('preset-rows');c.innerHTML='';"
    "    d.presets.forEach(function(p){"
    "      var row=document.createElement('div');"
    "      row.style.cssText='display:flex;align-items:center;gap:.5rem;"
    "padding:.5rem 0;border-bottom:1px solid #1a2540';"
    "      var nm=document.createElement('input');"
    "      nm.type='text';nm.className='fin fin-wide';"
    "      nm.style.cssText='flex:1;max-width:none;text-align:left;font-size:.83rem';"
    "      nm.value=p.name;nm.dataset.slot=p.slot;"
    "      var loadBtn=document.createElement('button');"
    "      loadBtn.className='btn btn-sm';"
    "      loadBtn.style.cssText='width:auto;padding:.35rem .6rem;margin-top:0;font-size:.8rem';"
    "      loadBtn.textContent='\\uD83D\\uDCC2 Carica';"
    "      loadBtn.onclick=(function(sl){return function(){loadPreset(sl)}})(p.slot);"
    "      var saveBtn=document.createElement('button');"
    "      saveBtn.className='btn btn-sm';"
    "      saveBtn.style.cssText='width:auto;padding:.35rem .6rem;margin-top:0;font-size:.8rem';"
    "      saveBtn.textContent='\\uD83D\\uDCBE Salva';"
    "      saveBtn.onclick=(function(sl,nmEl){return function(){savePreset(sl,nmEl.value)}})(p.slot,nm);"
    "      row.appendChild(nm);row.appendChild(loadBtn);row.appendChild(saveBtn);"
    "      c.appendChild(row)})})"
    "  .catch(function(){})}"
    "function loadPreset(slot){"
    "  fetch('/api/led_presets',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({action:'load',slot:slot})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    loadSched();toast('Preset caricato',1)"
    "  }).catch(function(){toast('Errore caricamento preset',0)})}"
    "function savePreset(slot,name){"
    "  var onT=$('sched-on').value.split(':');"
    "  var offT=$('sched-off').value.split(':');"
    "  var psT=$('sched-pause-start').value.split(':');"
    "  var peT=$('sched-pause-end').value.split(':');"
    "  var c=hexToRgb($('sched-color').value);"
    "  var pc=hexToRgb($('sched-pcolor').value);"
    "  var data={"
    "    action:'save',slot:slot,name:name||('Preset '+(slot+1)),"
    "    enabled:$('sched-en').checked,"
    "    on_hour:parseInt(onT[0]),on_minute:parseInt(onT[1]),"
    "    ramp_duration_min:parseInt($('sched-ramp').value),"
    "    pause_enabled:$('sched-pause-en').checked,"
    "    pause_start_hour:parseInt(psT[0]),pause_start_minute:parseInt(psT[1]),"
    "    pause_end_hour:parseInt(peT[0]),pause_end_minute:parseInt(peT[1]),"
    "    pause_brightness:parseInt($('sched-pbr').value),"
    "    pause_red:pc.r,pause_green:pc.g,pause_blue:pc.b,"
    "    off_hour:parseInt(offT[0]),off_minute:parseInt(offT[1]),"
    "    brightness:parseInt($('sched-br').value),"
    "    red:c.r,green:c.g,blue:c.b};"
    "  fetch('/api/led_presets',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    loadPresets();toast('Preset salvato',1)"
    "  }).catch(function(){toast('Errore salvataggio preset',0)})}"
    "/* ── Quick LED on/off (Dashboard) ── */"
    "function ledQuickOn(){"
    "  fetch('/api/leds',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({on:true})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('LED acceso',1);loadDashLeds()}).catch(function(){"
    "    toast('Errore LED',0)})}"
    "function ledQuickOff(){"
    "  fetch('/api/leds',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({on:false})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('LED spento',1);loadDashLeds()}).catch(function(){"
    "    toast('Errore LED',0)})}"
    "/* ── Dashboard data loading ── */"
    "function loadDash(){loadTemp();loadHistory();loadDashLeds();loadDashRelays();"
    "  loadDashHeater()}"
    "function loadDashLeds(){"
    "  fetch('/api/leds').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    fetch('/api/led_schedule').then(function(r){return r.json()})"
    "    .then(function(sc){"
    "      var name=sc.enabled?(d.on?'Acceso (auto)':'Spento (auto)'):(d.on?'Acceso':'Spento');"
    "      $('led-status-name').textContent=name;"
    "      $('led-dot').style.background=d.on?'#38bdf8':'#475569';"
    "      var pct=d.on?Math.round(d.brightness/255*100):0;"
    "      $('led-bright').textContent='Luminosit\\u00e0 '+pct+'%%';"
    "      var cr=d.on?d.r:0,cg=d.on?d.g:0,cb=d.on?d.b:0;"
    "      $('color-swatch').style.background='rgb('+cr+','+cg+','+cb+')';"
    "      $('rgb-label').textContent='RGB: '+cr+' , '+cg+' , '+cb})})}"
    "function loadDashRelays(){"
    "  fetch('/api/relays').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var c=$('relay-dash');c.innerHTML='';"
    "    d.relays.forEach(function(rl,i){"
    "      var item=document.createElement('div');"
    "      item.className='relay-item';"
    "      var row=document.createElement('div');"
    "      row.className='relay-row';"
    "      var nm=document.createElement('span');"
    "      nm.className='relay-name';nm.textContent=rl.name;"
    "      var bg=document.createElement('span');"
    "      bg.className='badge '+(rl.on?'badge-on':'badge-off');"
    "      bg.textContent=rl.on?'ON':'OFF';"
    "      bg.style.cursor='pointer';"
    "      bg.onclick=(function(idx,state){return function(){"
    "        fetch('/api/relays',{method:'POST',"
    "          headers:{'Content-Type':'application/json'},"
    "          body:JSON.stringify({index:idx,on:!state})"
    "        }).then(function(){loadDashRelays();"
    "          toast(rl.name+(!state?' ON':' OFF'),1)})"
    "        .catch(function(){toast('Errore rel\\u00e8',0)})}})(i,rl.on);"
    "      row.appendChild(nm);row.appendChild(bg);"
    "      item.appendChild(row);"
    "      c.appendChild(item)})})}"
    "function loadTemp(){"
    "  fetch('/api/temperature').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var el=$('temp-val');"
    "    if(d.valid){"
    "      el.innerHTML=d.temperature_c.toFixed(1)+"
    "        '<span class=\"temp-unit\">\\u00B0C</span>';"
    "      el.style.color=(d.temperature_c>=24&&d.temperature_c<=28)?'#4ade80':'#f87171';"
    "      $('sensor-status').textContent='\\u2713 Sensore: OK';"
    "      $('sensor-status').className='ok'}"
    "    else{"
    "      el.innerHTML='--.-<span class=\"temp-unit\">\\u00B0C</span>';"
    "      el.style.color='#64748b';"
    "      $('sensor-status').textContent='\\u2717 Sensore: Errore';"
    "      $('sensor-status').className='err'}})"
    "  .catch(function(){});"
    "  fetch('/api/status').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.connected){"
    "      $('wifi-rssi').textContent='\\uD83D\\uDCF6 WiFi: '+d.rssi+' dBm';"
    "      $('wifi-rssi').className=d.rssi>-60?'ok':d.rssi>-75?'warn':'err'}"
    "    else{"
    "      $('wifi-rssi').textContent='\\uD83D\\uDCF6 WiFi: disconnesso';"
    "      $('wifi-rssi').className='err'}})"
    "  .catch(function(){})}"
    "/* ── Temperature chart ── */"
    "function drawChart(samples){"
    "  var cv=$('temp-chart');if(!cv)return;"
    "  var dpr=window.devicePixelRatio||1;"
    "  cv.width=cv.clientWidth*dpr;cv.height=cv.clientHeight*dpr;"
    "  var ctx=cv.getContext('2d');ctx.scale(dpr,dpr);"
    "  var W=cv.clientWidth,H=cv.clientHeight;"
    "  var pad={t:18,r:10,b:28,l:44};"
    "  var cw=W-pad.l-pad.r,ch=H-pad.t-pad.b;"
    "  ctx.clearRect(0,0,W,H);"
    "  if(!samples||samples.length<2){"
    "    ctx.fillStyle='#64748b';ctx.font='12px sans-serif';"
    "    ctx.textAlign='center';"
    "    ctx.fillText('In attesa dei dati\\u2026',W/2,H/2);return}"
    "  var mn=1e9,mx=-1e9;"
    "  for(var i=0;i<samples.length;i++){"
    "    if(samples[i].c<mn)mn=samples[i].c;"
    "    if(samples[i].c>mx)mx=samples[i].c}"
    "  var margin=(mx-mn)*0.15;if(margin<0.3)margin=0.3;"
    "  var yMin=mn-margin,yMax=mx+margin;"
    "  /* Grid */"
    "  ctx.strokeStyle='#1a2540';ctx.lineWidth=1;"
    "  var ngy=4;for(var i=0;i<=ngy;i++){"
    "    var y=pad.t+ch-ch*(i/ngy);"
    "    ctx.beginPath();ctx.moveTo(pad.l,y);"
    "    ctx.lineTo(pad.l+cw,y);ctx.stroke();"
    "    ctx.fillStyle='#64748b';ctx.font='10px sans-serif';"
    "    ctx.textAlign='right';ctx.textBaseline='middle';"
    "    ctx.fillText((yMin+(yMax-yMin)*(i/ngy)).toFixed(1),pad.l-4,y)}"
    "  /* X labels */"
    "  var t0=samples[0].t,t1=samples[samples.length-1].t;"
    "  var span=t1-t0;if(span<1)span=1;"
    "  ctx.fillStyle='#64748b';ctx.font='10px sans-serif';"
    "  ctx.textAlign='center';ctx.textBaseline='top';"
    "  var lx=-999;"
    "  for(var i=0;i<samples.length;i++){"
    "    var x=pad.l+cw*((samples[i].t-t0)/span);"
    "    var d=new Date(samples[i].t*1000);"
    "    if(d.getMinutes()===0&&(x-lx)>36){"
    "      ctx.beginPath();ctx.moveTo(x,pad.t);"
    "      ctx.lineTo(x,pad.t+ch);ctx.strokeStyle='#1a2540';"
    "      ctx.stroke();"
    "      var lbl=('0'+d.getHours()).slice(-2)+':00';"
    "      ctx.fillText(lbl,x,pad.t+ch+4);lx=x}}"
    "  /* Line */"
    "  ctx.beginPath();ctx.strokeStyle='#38bdf8';ctx.lineWidth=2;"
    "  ctx.lineJoin='round';"
    "  for(var i=0;i<samples.length;i++){"
    "    var x=pad.l+cw*((samples[i].t-t0)/span);"
    "    var y=pad.t+ch-ch*((samples[i].c-yMin)/(yMax-yMin));"
    "    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y)}"
    "  ctx.stroke();"
    "  /* Gradient fill */"
    "  var last=samples.length-1;"
    "  var lx2=pad.l+cw*((samples[last].t-t0)/span);"
    "  ctx.lineTo(lx2,pad.t+ch);ctx.lineTo(pad.l,pad.t+ch);ctx.closePath();"
    "  var grd=ctx.createLinearGradient(0,pad.t,0,pad.t+ch);"
    "  grd.addColorStop(0,'rgba(56,189,248,0.2)');"
    "  grd.addColorStop(1,'rgba(56,189,248,0.01)');"
    "  ctx.fillStyle=grd;ctx.fill()}"
    "function loadHistory(){"
    "  fetch('/api/temperature_history').then(function(r){return r.json()})"
    "  .then(function(d){drawChart(d.samples)})"
    "  .catch(function(){})}"
    "/* ── Heater (dashboard rule) ── */"
    "function loadDashHeater(){"
    "  fetch('/api/heater').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var el=$('heater-rule');"
    "    if(d.enabled){"
    "      el.className='rule-box';"
    "      el.innerHTML='\\u2699 Regola attiva: Temp &lt; '+"
    "        d.target_temp_c.toFixed(1)+' \\u00B0C \\u2192 Rel\\u00e8 '+(d.relay_index+1)+"
    "        ' (isteresi '+d.hysteresis_c.toFixed(1)+'\\u00B0C)'}"
    "    else{"
    "      el.className='rule-box inactive';"
    "      el.textContent='\\u2699 Nessuna regola attiva'}})}"
    "/* ── Telegram ── */"
    "function tgTs(id,ts){"
    "  var el=$(id);"
    "  if(ts>0){var d=Math.floor((Date.now()/1000-ts)/86400);"
    "    el.textContent=d+' giorni fa'}"
    "  else{el.textContent='Mai'}}"
    "function loadTg(){"
    "  fetch('/api/telegram').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.bot_token_set){$('tg-token').placeholder="
    "      'Token configurato \\u2713'}"
    "    $('tg-chatid').value=d.chat_id||'';"
    "    $('tg-en').checked=d.enabled;"
    "    $('tg-talm').checked=d.temp_alarm_enabled;"
    "    $('tg-thigh').value=d.temp_high_c;"
    "    $('tg-tlow').value=d.temp_low_c;"
    "    $('tg-wc').checked=d.water_change_enabled;"
    "    $('tg-wcdays').value=d.water_change_days;"
    "    $('tg-fert').checked=d.fertilizer_enabled;"
    "    $('tg-fertdays').value=d.fertilizer_days;"
    "    $('tg-sum').checked=d.daily_summary_enabled;"
    "    $('tg-sumhr').value=d.daily_summary_hour;"
    "    if($('tg-rel'))$('tg-rel').checked=d.relay_notify_enabled||false;"
    "    tgTs('tg-wclast',d.last_water_change);"
    "    tgTs('tg-fertlast',d.last_fertilizer)})}"
    "function saveTg(){"
    "  var data={"
    "    chat_id:$('tg-chatid').value,"
    "    enabled:$('tg-en').checked,"
    "    temp_alarm_enabled:$('tg-talm').checked,"
    "    temp_high_c:parseFloat($('tg-thigh').value),"
    "    temp_low_c:parseFloat($('tg-tlow').value),"
    "    water_change_enabled:$('tg-wc').checked,"
    "    water_change_days:parseInt($('tg-wcdays').value),"
    "    fertilizer_enabled:$('tg-fert').checked,"
    "    fertilizer_days:parseInt($('tg-fertdays').value),"
    "    daily_summary_enabled:$('tg-sum').checked,"
    "    daily_summary_hour:parseInt($('tg-sumhr').value),"
    "    relay_notify_enabled:$('tg-rel')?$('tg-rel').checked:false};"
    "  var tk=$('tg-token').value;"
    "  if(tk.length>0){data.bot_token=tk}"
    "  fetch('/api/telegram',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if(d.bot_token_set){$('tg-token').value='';"
    "      $('tg-token').placeholder='Token configurato \\u2713'}"
    "    toast('Impostazioni salvate',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
    "function testTg(){"
    "  fetch('/api/telegram_test',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    toast(d.ok?'Test inviato!':'Errore: '+(d.error||'sconosciuto'),d.ok)"
    "  }).catch(function(){toast('Errore invio',0)})}"
    "function resetWc(){"
    "  fetch('/api/telegram_wc',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    tgTs('tg-wclast',d.last_water_change);"
    "    toast('Cambio acqua registrato',1)})}"
    "function resetFert(){"
    "  fetch('/api/telegram_fert',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    tgTs('tg-fertlast',d.last_fertilizer);"
    "    toast('Dose fertilizzante registrata',1)})}"
    "/* ── Relay control (Manutenzione tab) ── */"
    "function loadRelays(){"
    "  fetch('/api/relays').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var c=$('relay-rows');c.innerHTML='';"
    "    d.relays.forEach(function(rl,i){"
    "      var blk=document.createElement('div');"
    "      blk.style.cssText='border-bottom:1px solid #1a2540;padding:.6rem 0';"
    "      /* Name row */"
    "      var nm=document.createElement('div');"
    "      nm.className='row';"
    "      var lbl=document.createElement('span');"
    "      lbl.className='label';lbl.textContent=rl.name;"
    "      lbl.style.cursor='pointer';lbl.title='Clicca per rinominare';"
    "      lbl.onclick=function(){"
    "        var nn=prompt('Rinomina rel\\u00e8:',rl.name);"
    "        if(nn&&nn.length>0){"
    "          fetch('/api/relays',{method:'POST',"
    "            headers:{'Content-Type':'application/json'},"
    "            body:JSON.stringify({index:i,name:nn})"
    "          }).then(function(){loadRelays();toast('Rinominato',1)})"
    "          .catch(function(){toast('Errore rinomina',0)})}};"
    "      var tg=document.createElement('label');"
    "      tg.className='toggle';"
    "      var inp=document.createElement('input');"
    "      inp.type='checkbox';inp.checked=rl.on;"
    "      inp.onchange=function(){"
    "        fetch('/api/relays',{method:'POST',"
    "          headers:{'Content-Type':'application/json'},"
    "          body:JSON.stringify({index:i,on:inp.checked})"
    "        }).then(function(r){return r.json()}).then(function(){"
    "          toast(rl.name+(inp.checked?' ON':' OFF'),1)})"
    "        .catch(function(){toast('Errore rel\\u00e8',0)})};"
    "      var sl=document.createElement('span');sl.className='slider';"
    "      tg.appendChild(inp);tg.appendChild(sl);"
    "      nm.appendChild(lbl);nm.appendChild(tg);"
    "      blk.appendChild(nm);"
    "      /* Schedule slots */"
    "      var schHdr=document.createElement('div');"
    "      schHdr.style.cssText='font-size:.75rem;color:#64748b;margin:.3rem 0 .2rem';"
    "      schHdr.textContent='Programmazione (fino a 4 slot):';"
    "      blk.appendChild(schHdr);"
    "      var slots=rl.schedules||[];"
    "      for(var s=0;s<4;s++){"
    "        var slot=slots[s]||{enabled:false,on_min:480,off_min:1200};"
    "        (function(si,sl_data){"
    "          var srow=document.createElement('div');"
    "          srow.style.cssText='display:flex;align-items:center;gap:.4rem;"
    "margin:.2rem 0;flex-wrap:wrap';"
    "          var sEn=document.createElement('input');"
    "          sEn.type='checkbox';sEn.checked=sl_data.enabled||false;"
    "          var sOnH=document.createElement('input');"
    "          sOnH.type='time';sOnH.className='fin';"
    "          sOnH.style.cssText='max-width:90px;font-size:.8rem;padding:.3rem';"
    "          var onM=sl_data.on_min||0;"
    "          sOnH.value=('0'+Math.floor(onM/60)).slice(-2)+':'+"
    "            ('0'+(onM%%60)).slice(-2);"
    "          var sOffH=document.createElement('input');"
    "          sOffH.type='time';sOffH.className='fin';"
    "          sOffH.style.cssText='max-width:90px;font-size:.8rem;padding:.3rem';"
    "          var offM=sl_data.off_min||0;"
    "          sOffH.value=('0'+Math.floor(offM/60)).slice(-2)+':'+"
    "            ('0'+(offM%%60)).slice(-2);"
    "          var sSave=document.createElement('button');"
    "          sSave.className='btn-sm btn';"
    "          sSave.style.cssText='width:auto;padding:.3rem .5rem;margin-top:0;"
    "font-size:.75rem';"
    "          sSave.textContent='\\u2713';"
    "          sSave.onclick=function(){"
    "            var onT=sOnH.value.split(':');"
    "            var offT=sOffH.value.split(':');"
    "            var payload={index:i,schedule_slot:si,"
    "              schedule_enabled:sEn.checked,"
    "              schedule_on_min:parseInt(onT[0])*60+parseInt(onT[1]),"
    "              schedule_off_min:parseInt(offT[0])*60+parseInt(offT[1])};"
    "            fetch('/api/relays',{method:'POST',"
    "              headers:{'Content-Type':'application/json'},"
    "              body:JSON.stringify(payload)"
    "            }).then(function(){toast('Slot '+si+' salvato',1)})"
    "            .catch(function(){toast('Errore slot',0)})};"
    "          var span=document.createElement('span');"
    "          span.style.cssText='font-size:.75rem;color:#64748b;min-width:30px';"
    "          span.textContent='S'+(si+1)+':';"
    "          srow.appendChild(span);srow.appendChild(sEn);"
    "          srow.appendChild(sOnH);"
    "          var arr=document.createElement('span');"
    "          arr.textContent='\\u2192';"
    "          arr.style.cssText='color:#64748b;font-size:.8rem';"
    "          srow.appendChild(arr);"
    "          srow.appendChild(sOffH);srow.appendChild(sSave);"
    "          blk.appendChild(srow)"
    "        })(s,slot)}"
    "      c.appendChild(blk)})})}"
    "/* ── System status (Manutenzione) ── */"
    "function loadSys(){"
    "  fetch('/api/status').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('sys-conn').textContent=d.connected?'Connesso':'Disconnesso';"
    "    $('sys-conn').className='value '+(d.connected?'ok':'err');"
    "    $('sys-ip').textContent=d.ip||'\\u2014';"
    "    $('sys-ssid').textContent=d.ssid||'\\u2014';"
    "    $('sys-rssi').textContent=d.rssi+' dBm';"
    "    $('sys-heap').textContent=d.free_heap+' B';"
    "    var h=Math.floor(d.uptime_s/3600);"
    "    var m=Math.floor((d.uptime_s%%3600)/60);"
    "    $('sys-uptime').textContent=h+'h '+m+'m'})}"
    "/* ── DuckDNS ── */"
    "function loadDdns(){"
    "  fetch('/api/duckdns').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.domain){$('ddns-domain').value=d.domain}"
    "    if(d.token_set){$('ddns-token').placeholder="
    "      'Token configurato \\u2713'}"
    "    $('ddns-en').checked=d.enabled;"
    "    $('ddns-status').textContent=d.last_status||'--';"
    "    $('ddns-status').className='value '+"
    "      (d.last_status&&d.last_status.indexOf('OK')===0?'ok':'err')"
    "  })}"
    "function saveDdns(){"
    "  var data={enabled:$('ddns-en').checked};"
    "  var dom=$('ddns-domain').value.trim();"
    "  if(dom.length>0){data.domain=dom}"
    "  var tk=$('ddns-token').value;"
    "  if(tk.length>0){data.token=tk}"
    "  fetch('/api/duckdns',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if(d.token_set){$('ddns-token').value='';"
    "      $('ddns-token').placeholder='Token configurato \\u2713'}"
    "    $('ddns-status').textContent=d.last_status||'--';"
    "    toast('DuckDNS salvato',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
    "function testDdns(){"
    "  $('ddns-status').textContent='aggiornamento\\u2026';"
    "  fetch('/api/duckdns_update',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    $('ddns-status').textContent=d.last_status||'--';"
    "    $('ddns-status').className='value '+(d.ok?'ok':'err');"
    "    toast(d.ok?'DuckDNS aggiornato!':'Aggiornamento fallito',d.ok)"
    "  }).catch(function(){toast('Errore aggiornamento',0)})}"
    "/* ── OTA ── */"
    "function startOta(){"
    "  var url=$('ota-url').value.trim();"
    "  if(!url){toast('Inserisci URL firmware',0);return}"
    "  fetch('/api/ota',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({url:url})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if(d.ok){toast('Aggiornamento avviato',1);pollOta()}"
    "    else{toast('Errore: '+(d.error||'sconosciuto'),0)}"
    "  }).catch(function(){toast('Errore OTA',0)})}"
    "function pollOta(){"
    "  var iv=setInterval(function(){"
    "    fetch('/api/ota_status').then(function(r){return r.json()})"
    "    .then(function(d){"
    "      $('ota-status').textContent=d.status;"
    "      $('ota-fill').style.width=d.progress+'%%';"
    "      if(d.status==='done'||d.status==='error'){"
    "        clearInterval(iv);"
    "        if(d.status==='done')toast('Aggiornamento completato! Riavvio...',1);"
    "        else toast('Errore: '+d.error,0)}})},2000)}"
    "function loadOtaStatus(){"
    "  fetch('/api/ota_status').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('ota-status').textContent=d.status;"
    "    $('ota-fill').style.width=d.progress+'%%';"
    "    if(d.status==='downloading'||d.status==='flashing')pollOta()})}"
    "/* ── Heater (Manutenzione) ── */"
    "function loadHeater(){"
    "  fetch('/api/heater').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('ht-en').checked=d.enabled;"
    "    $('ht-relay').value=d.relay_index;"
    "    $('ht-target').value=d.target_temp_c;"
    "    $('ht-hyst').value=d.hysteresis_c})}"
    "function saveHeater(){"
    "  var data={"
    "    enabled:$('ht-en').checked,"
    "    relay_index:parseInt($('ht-relay').value),"
    "    target_temp_c:parseFloat($('ht-target').value),"
    "    hysteresis_c:parseFloat($('ht-hyst').value)};"
    "  fetch('/api/heater',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Riscaldatore salvato',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
    "/* ── CO2 Controller ── */"
    "function loadCo2(){"
    "  fetch('/api/co2').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('co2-en').checked=d.enabled||false;"
    "    $('co2-relay').value=d.relay_index||0;"
    "    $('co2-pre').value=d.pre_on_min||0;"
    "    $('co2-post').value=d.post_off_min||0})}"
    "function saveCo2(){"
    "  var data={enabled:$('co2-en').checked,"
    "    relay_index:parseInt($('co2-relay').value),"
    "    pre_on_min:parseInt($('co2-pre').value),"
    "    post_off_min:parseInt($('co2-post').value)};"
    "  fetch('/api/co2',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('CO\\u2082 salvato',1)}).catch(function(){toast('Errore CO\\u2082',0)})}"
    "/* ── Timezone ── */"
    "function loadTimezone(){"
    "  fetch('/api/timezone').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('tz-str').value=d.tz||''})}"
    "function saveTz(){"
    "  var tz=$('tz-str').value.trim();"
    "  if(!tz){toast('Inserisci stringa TZ',0);return}"
    "  fetch('/api/timezone',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({tz:tz})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Fuso orario salvato',1)}).catch(function(){toast('Errore TZ',0)})}"
    "function tzPreset(){"
    "  var v=$('tz-preset').value;if(v)$('tz-str').value=v}"
    "/* ── Colour helpers ── */"
    "function hexToRgb(hex){"
    "  var r=parseInt(hex.slice(1,3),16)||0;"
    "  var g=parseInt(hex.slice(3,5),16)||0;"
    "  var b=parseInt(hex.slice(5,7),16)||0;"
    "  return{r:r,g:g,b:b}}"
    "function rgbToHex(r,g,b){"
    "  return'#'+('0'+r.toString(16)).slice(-2)"
    "         +('0'+g.toString(16)).slice(-2)"
    "         +('0'+b.toString(16)).slice(-2)}"
    "/* ── Feeding mode ── */"
    "var _feedTmr=null;"
    "function updateFeedingUI(d){"
    "  var active=d.active||false;"
    "  $('feeding-state').textContent=active?'ATTIVA':'Inattiva';"
    "  $('feeding-state').className=active?'warn':'ok';"
    "  var cd=$('feeding-countdown');"
    "  if(active&&d.remaining_s>0){"
    "    cd.style.display='block';"
    "    var m=Math.floor(d.remaining_s/60);"
    "    var s=d.remaining_s%%60;"
    "    $('feeding-remain').textContent=('0'+m).slice(-2)+':'+('0'+s).slice(-2);"
    "    $('btn-feed-start').style.display='none';"
    "    $('btn-feed-stop').style.display='';"
    "  }else{"
    "    cd.style.display='none';"
    "    $('btn-feed-start').style.display='';"
    "    $('btn-feed-stop').style.display='none';"
    "    if(_feedTmr){clearInterval(_feedTmr);_feedTmr=null}}}"
    "function loadFeeding(){"
    "  fetch('/api/feeding').then(function(r){return r.json()})"
    "  .then(function(d){updateFeedingUI(d)})"
    "  .catch(function(){})}"
    "function startFeeding(){"
    "  fetch('/api/feeding',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({action:'start'})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    updateFeedingUI(d);toast('Alimentazione avviata!',1);"
    "    if(!_feedTmr)_feedTmr=setInterval(function(){loadFeeding()},5000)"
    "  }).catch(function(){toast('Errore avvio alimentazione',0)})}"
    "function stopFeeding(){"
    "  fetch('/api/feeding',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({action:'stop'})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    updateFeedingUI(d);toast('Alimentazione fermata',1)"
    "  }).catch(function(){toast('Errore stop alimentazione',0)})}"
    "function loadFeedingConfig(){"
    "  fetch('/api/feeding').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if($('feed-relay'))$('feed-relay').value=d.relay_index;"
    "    if($('feed-dur'))$('feed-dur').value=d.duration_min;"
    "    if($('feed-dim'))$('feed-dim').checked=d.dim_lights;"
    "    if($('feed-dim-br')){$('feed-dim-br').value=d.dim_brightness;"
    "      $('feed-dim-br-val').textContent=d.dim_brightness}})"
    "  .catch(function(){})}"
    "function saveFeeding(){"
    "  var data={"
    "    relay_index:parseInt($('feed-relay').value),"
    "    duration_min:parseInt($('feed-dur').value),"
    "    dim_lights:$('feed-dim').checked,"
    "    dim_brightness:parseInt($('feed-dim-br').value)};"
    "  fetch('/api/feeding',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Impostazioni alimentazione salvate',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
    "/* ── LED Scenes ── */"
    "function loadSceneConfig(){"
    "  fetch('/api/scene').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if($('scene-sel'))$('scene-sel').value=d.active||0;"
    "    if($('sc-sr-dur'))$('sc-sr-dur').value=d.sunrise_duration_min||30;"
    "    if($('sc-sr-br')){$('sc-sr-br').value=d.sunrise_max_brightness||255;"
    "      $('sc-sr-br-val').textContent=d.sunrise_max_brightness||255}"
    "    if($('sc-ss-dur'))$('sc-ss-dur').value=d.sunset_duration_min||30;"
    "    if($('sc-ml-br')){$('sc-ml-br').value=d.moonlight_brightness||15;"
    "      $('sc-ml-br-val').textContent=d.moonlight_brightness||15}"
    "    if($('sc-ml-color')){"
    "      var r=d.moonlight_r||20,g=d.moonlight_g||40,b=d.moonlight_b||100;"
    "      $('sc-ml-color').value=rgbToHex(r,g,b)}"
    "    if($('sc-storm'))$('sc-storm').value=d.storm_intensity||70;"
    "    if($('sc-cloud-d'))$('sc-cloud-d').value=d.clouds_depth||40;"
    "    if($('sc-cloud-p'))$('sc-cloud-p').value=d.clouds_period_s||120;"
    "  }).catch(function(){})}"
    "function saveScene(){"
    "  var c=hexToRgb($('sc-ml-color').value);"
    "  var data={"
    "    sunrise_duration_min:parseInt($('sc-sr-dur').value),"
    "    sunrise_max_brightness:parseInt($('sc-sr-br').value),"
    "    sunset_duration_min:parseInt($('sc-ss-dur').value),"
    "    moonlight_brightness:parseInt($('sc-ml-br').value),"
    "    moonlight_r:c.r,moonlight_g:c.g,moonlight_b:c.b,"
    "    storm_intensity:parseInt($('sc-storm').value),"
    "    clouds_depth:parseInt($('sc-cloud-d').value),"
    "    clouds_period_s:parseInt($('sc-cloud-p').value)};"
    "  fetch('/api/scene',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Impostazioni scena salvate',1)"
    "  }).catch(function(){toast('Errore salvataggio scena',0)})}"
    "function applyScene(){"
    "  var sc=parseInt($('scene-sel').value);"
    "  fetch('/api/scene',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({start_scene:sc})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    var names=['Nessuna','Alba','Tramonto','Chiaro di Luna','Temporale','Nuvole'];"
    "    toast(sc===0?'Scene fermate':('Scena: '+names[sc]),1)"
    "  }).catch(function(){toast('Errore avvio scena',0)})}"
    "/* ── Daily Cycle ── */"
    "var DC_PHASES=['Notte','Alba','Mattina','Mezzogiorno','Pomeriggio','Tramonto','Sera'];"
    "function minToHHMM(m){if(m<0)return'--:--';"
    "  return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%%60).padStart(2,'0')}"
    "function loadDailyCycle(){"
    "  fetch('/api/daily_cycle').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if($('dc-en'))$('dc-en').checked=!!d.enabled;"
    "    if($('dc-lat'))$('dc-lat').value=d.latitude||0;"
    "    if($('dc-lon'))$('dc-lon').value=d.longitude||0;"
    "    if($('dc-sunrise'))$('dc-sunrise').textContent=minToHHMM(d.sunrise_min);"
    "    if($('dc-sunset'))$('dc-sunset').textContent=minToHHMM(d.sunset_min);"
    "    if($('dc-phase'))$('dc-phase').textContent=DC_PHASES[d.phase]||'--';"
    "  }).catch(function(){})}"
    "function saveDailyCycle(){"
    "  var data={"
    "    enabled:$('dc-en').checked,"
    "    latitude:parseFloat($('dc-lat').value),"
    "    longitude:parseFloat($('dc-lon').value)};"
    "  fetch('/api/daily_cycle',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if($('dc-sunrise'))$('dc-sunrise').textContent=minToHHMM(d.sunrise_min);"
    "    if($('dc-sunset'))$('dc-sunset').textContent=minToHHMM(d.sunset_min);"
    "    if($('dc-phase'))$('dc-phase').textContent=DC_PHASES[d.phase]||'--';"
    "    toast('Giornata naturale salvata',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
    "/* ── Voice control ── */"
    "var _vcPoll=null;"
    "var VC_STATUS=['idle','registrazione...','trascrizione...','elaborazione...','completato','errore'];"
    "function loadVoiceConfig(){"
    "  fetch('/api/voice/config').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('vc-en').checked=d.enabled;"
    "    $('vc-key').value=d.api_key_set?'**** (configurato)':'';"
    "    $('vc-stt').value=d.stt_model||'';"
    "    $('vc-llm').value=d.llm_model||'';"
    "    $('vc-rec').value=d.record_ms||5000;"
    "    $('vc-sck').value=d.i2s_sck_io;"
    "    $('vc-ws').value=d.i2s_ws_io;"
    "    $('vc-sd').value=d.i2s_sd_io;"
    "  }).catch(function(){})}"
    "function saveVoiceConfig(){"
    "  var key=$('vc-key').value;"
    "  var body={enabled:$('vc-en').checked,"
    "    stt_model:$('vc-stt').value,"
    "    llm_model:$('vc-llm').value,"
    "    record_ms:parseInt($('vc-rec').value)||5000,"
    "    i2s_sck_io:parseInt($('vc-sck').value),"
    "    i2s_ws_io:parseInt($('vc-ws').value),"
    "    i2s_sd_io:parseInt($('vc-sd').value)};"
    "  if(key&&key.indexOf('*')<0)body.api_key=key;"
    "  fetch('/api/voice/config',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(body)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Voce salvata',1)}).catch(function(){"
    "    toast('Errore salvataggio voce',0)})}"
    "function startVoiceRec(){"
    "  fetch('/api/voice/record',{method:'POST'})"
    "  .then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.ok){"
    "      $('btn-vc-rec').disabled=true;"
    "      $('vc-status').textContent='registrazione...';"
    "      if(_vcPoll)clearInterval(_vcPoll);"
    "      _vcPoll=setInterval(pollVoiceStatus,1500);"
    "    }else{"
    "      toast(d.error||'Errore avvio registrazione',0)}"
    "  }).catch(function(){toast('Errore connessione',0)})}"
    "function pollVoiceStatus(){"
    "  fetch('/api/voice/status').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var st=VC_STATUS[d.status]||'?';"
    "    $('vc-status').textContent=st;"
    "    if(d.transcript){"
    "      $('vc-transcript').textContent=d.transcript;"
    "      $('vc-transcript-row').style.display='flex'}"
    "    if(d.command){"
    "      $('vc-command').textContent=d.command;"
    "      $('vc-command-row').style.display='flex'}"
    "    if(d.result){"
    "      $('vc-result').textContent=d.result;"
    "      $('vc-result-row').style.display='flex'}"
    "    if(d.status>=4){"
    "      clearInterval(_vcPoll);_vcPoll=null;"
    "      $('btn-vc-rec').disabled=false;"
    "      if(d.status===4)toast(d.result||'Comando eseguito',1);"
    "      else toast('Errore: '+d.result,0)}"
    "  }).catch(function(){})}"
    "/* ── Init ── */"
    "loadDash();"
    "loadSceneConfig();loadDailyCycle();"
    "setInterval(function(){loadTemp();loadDashLeds();loadDashRelays();"
    "  loadDashHeater();loadFeeding()},2000);"
    "setInterval(function(){loadHistory()},60000);"
    "</script>"
    "</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    get_wifi_status(&ws);

    /* Uptime string */
    int64_t us   = esp_timer_get_time();
    int     secs = (int)(us / 1000000);
    int     h = secs / 3600, m = (secs % 3600) / 60;
    char uptime[32];
    snprintf(uptime, sizeof(uptime), "%d h %d min", h, m);

    /* NTP status – time is valid once the year is >= 2024 */
    time_t now_t = time(NULL);
    struct tm ti;
    localtime_r(&now_t, &ti);
    const char *ntp_status = (ti.tm_year >= (2024 - 1900)) ? "OK" : "Non sincr.";

    /* Running OTA partition label */
    const esp_partition_t *part = esp_ota_get_running_partition();
    const char *part_label = part ? part->label : "unknown";

    /* Render HTML – heap-allocated to avoid httpd task stack overflow */
    char *buf = malloc(HTML_BUF_SIZE);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_FAIL;
    }

    int len = snprintf(buf, HTML_BUF_SIZE, STATUS_HTML_TEMPLATE,
                       ws.connected ? ws.ip : "\xe2\x80\x94" /* — */,
                       ntp_status,
                       uptime,
                       part_label);
    if (len >= HTML_BUF_SIZE) {
        len = HTML_BUF_SIZE - 1;   /* output was truncated */
    }

    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, buf, len);
    free(buf);
    return ret;
}

/* ── JSON status endpoint (/api/status  GET) ─────────────────────── */

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    get_wifi_status(&ws);

    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char escaped_ssid[128];
    json_escape(ws.connected ? ws.ssid : "", escaped_ssid, sizeof(escaped_ssid));

    char buf[JSON_STATUS_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,"
        "\"ip\":\"%s\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"free_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRId64 "}",
        ws.connected ? "true" : "false",
        ws.connected ? ws.ip : "",
        escaped_ssid,
        ws.connected ? ws.rssi : 0,
        esp_get_free_heap_size(),
        uptime_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Health check endpoint (/api/health  GET) ────────────────────── */

/**
 * @brief Lightweight health check endpoint for monitoring.
 *
 * Returns a JSON object with overall system health, subsystem status,
 * free heap memory, minimum free heap, and uptime.  Useful for
 * external monitoring tools and availability checks.
 */
static esp_err_t api_health_get_handler(httpd_req_t *req)
{
    bool wifi_ok = wifi_manager_is_connected();

    float temp_c = 0.0f;
    bool temp_ok = temperature_sensor_get(&temp_c);

    bool led_ok  = (led_controller_get_num_leds() > 0);

    /* Overall health: all critical subsystems must be OK */
    bool healthy = wifi_ok;   /* WiFi is the only hard requirement */

    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"healthy\":%s,"
        "\"wifi\":%s,"
        "\"temperature_sensor\":%s,"
        "\"led_strip\":%s,"
        "\"led_schedule_enabled\":%s,"
        "\"temp_c\":%.1f,"
        "\"free_heap\":%" PRIu32 ","
        "\"min_free_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRId64 "}",
        healthy ? "true" : "false",
        wifi_ok ? "true" : "false",
        temp_ok ? "true" : "false",
        led_ok  ? "true" : "false",
        led_schedule_get_config().enabled ? "true" : "false",
        temp_ok ? (double)temp_c : 0.0,
        esp_get_free_heap_size(),
        esp_get_minimum_free_heap_size(),
        uptime_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── LED status endpoint (/api/leds  GET) ─────────────────────────── */

static esp_err_t api_leds_get_handler(httpd_req_t *req)
{
    uint8_t r, g, b;
    led_controller_get_color(&r, &g, &b);

    char buf[JSON_LEDS_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"on\":%s,"
        "\"brightness\":%d,"
        "\"r\":%d,\"g\":%d,\"b\":%d,"
        "\"num_leds\":%d}",
        led_controller_is_on() ? "true" : "false",
        led_controller_get_brightness(),
        r, g, b,
        led_controller_get_num_leds());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── LED control endpoint (/api/leds  POST) ──────────────────────── */

/**
 * @brief Simple integer parser – find "key":value in a JSON-like string.
 *        Returns -1 if key not found.
 */
static int json_get_int(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    /* skip possible ": */
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    return atoi(p);
}

/**
 * @brief Check for "key":true / "key":false in a JSON-like string.
 *        Returns 1 for true, 0 for false, -1 if not found.
 */
static int json_get_bool(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

/**
 * @brief Extract a string value for "key":"value" in a JSON-like string.
 *        Returns 0 on success, -1 if key not found.
 */
static int json_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    /* skip ": and spaces */
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/**
 * @brief Extract a double value for "key":value in a JSON-like string.
 *        Returns 0 on success, -1 if key not found.
 */
static int json_get_double(const char *json, const char *key, double *out)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    char *end = NULL;
    double val = strtod(p, &end);
    if (end == p) return -1;
    *out = val;
    return 0;
}

/** @brief Clamp a double to [0, max] and return as uint16_t. */
static uint16_t clamp_u16(double v, double max)
{
    if (v < 0.0) return 0;
    if (v > max) return (uint16_t)max;
    return (uint16_t)v;
}

static esp_err_t api_leds_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_LED_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "LED POST body: %s", buf);

    /* Parse fields */
    int on_val = json_get_bool(buf, "\"on\"");
    int br_val = json_get_int(buf, "\"brightness\"");
    int r_val  = json_get_int(buf, "\"r\"");
    int g_val  = json_get_int(buf, "\"g\"");
    int b_val  = json_get_int(buf, "\"b\"");

    /* Apply color if all three components are present */
    if (r_val >= 0 && g_val >= 0 && b_val >= 0) {
        led_controller_set_color(
            (uint8_t)(r_val > 255 ? 255 : r_val),
            (uint8_t)(g_val > 255 ? 255 : g_val),
            (uint8_t)(b_val > 255 ? 255 : b_val));
    }

    /* Apply brightness */
    if (br_val >= 0) {
        led_controller_set_brightness((uint8_t)(br_val > 255 ? 255 : br_val));
    }

    /* Apply on/off with acclimatization ramp (fade) */
    {
        uint32_t ramp_ms = (uint32_t)CONFIG_LED_RAMP_DURATION_SEC * 1000;
        if (on_val == 1) {
            led_controller_fade_on(ramp_ms);
        } else if (on_val == 0) {
            led_controller_fade_off(ramp_ms);
        }
    }

    /* Respond with updated state */
    return api_leds_get_handler(req);
}

/* ── LED Schedule status endpoint (/api/led_schedule  GET) ────────── */

static esp_err_t api_led_schedule_get_handler(httpd_req_t *req)
{
    led_schedule_config_t cfg = led_schedule_get_config();

    char buf[JSON_SCHED_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,"
        "\"on_hour\":%d,\"on_minute\":%d,"
        "\"ramp_duration_min\":%d,"
        "\"pause_enabled\":%s,"
        "\"pause_start_hour\":%d,\"pause_start_minute\":%d,"
        "\"pause_end_hour\":%d,\"pause_end_minute\":%d,"
        "\"pause_brightness\":%d,"
        "\"pause_red\":%d,\"pause_green\":%d,\"pause_blue\":%d,"
        "\"off_hour\":%d,\"off_minute\":%d,"
        "\"brightness\":%d,"
        "\"red\":%d,\"green\":%d,\"blue\":%d}",
        cfg.enabled       ? "true" : "false",
        cfg.on_hour,   cfg.on_minute,
        cfg.ramp_duration_min,
        cfg.pause_enabled ? "true" : "false",
        cfg.pause_start_hour, cfg.pause_start_minute,
        cfg.pause_end_hour,   cfg.pause_end_minute,
        cfg.pause_brightness,
        cfg.pause_red, cfg.pause_green, cfg.pause_blue,
        cfg.off_hour,  cfg.off_minute,
        cfg.brightness,
        cfg.red, cfg.green, cfg.blue);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── LED Schedule control endpoint (/api/led_schedule  POST) ─────── */

static esp_err_t api_led_schedule_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_SCHED_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "LED Schedule POST body: %s", buf);

    led_schedule_config_t cfg = led_schedule_get_config();
    int val;
    double dval;

    val = json_get_bool(buf, "\"enabled\"");
    if (val >= 0) cfg.enabled = (val == 1);

    val = json_get_int(buf, "\"on_hour\"");
    if (val >= 0) cfg.on_hour = (uint8_t)val;
    val = json_get_int(buf, "\"on_minute\"");
    if (val >= 0) cfg.on_minute = (uint8_t)val;

    if (json_get_double(buf, "\"ramp_duration_min\"", &dval) == 0)
        cfg.ramp_duration_min = clamp_u16(dval, 120.0);

    val = json_get_bool(buf, "\"pause_enabled\"");
    if (val >= 0) cfg.pause_enabled = (val == 1);

    val = json_get_int(buf, "\"pause_start_hour\"");
    if (val >= 0) cfg.pause_start_hour = (uint8_t)val;
    val = json_get_int(buf, "\"pause_start_minute\"");
    if (val >= 0) cfg.pause_start_minute = (uint8_t)val;
    val = json_get_int(buf, "\"pause_end_hour\"");
    if (val >= 0) cfg.pause_end_hour = (uint8_t)val;
    val = json_get_int(buf, "\"pause_end_minute\"");
    if (val >= 0) cfg.pause_end_minute = (uint8_t)val;

    val = json_get_int(buf, "\"pause_brightness\"");
    if (val >= 0) cfg.pause_brightness = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"pause_red\"");
    if (val >= 0) cfg.pause_red = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"pause_green\"");
    if (val >= 0) cfg.pause_green = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"pause_blue\"");
    if (val >= 0) cfg.pause_blue = (uint8_t)(val > 255 ? 255 : val);

    val = json_get_int(buf, "\"off_hour\"");
    if (val >= 0) cfg.off_hour = (uint8_t)val;
    val = json_get_int(buf, "\"off_minute\"");
    if (val >= 0) cfg.off_minute = (uint8_t)val;

    val = json_get_int(buf, "\"brightness\"");
    if (val >= 0) cfg.brightness = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"red\"");
    if (val >= 0) cfg.red = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"green\"");
    if (val >= 0) cfg.green = (uint8_t)(val > 255 ? 255 : val);
    val = json_get_int(buf, "\"blue\"");
    if (val >= 0) cfg.blue = (uint8_t)(val > 255 ? 255 : val);

    led_schedule_set_config(&cfg);

    return api_led_schedule_get_handler(req);
}

/* ── LED Presets GET endpoint (/api/led_presets  GET) ────────────── */

static esp_err_t api_led_presets_get_handler(httpd_req_t *req)
{
    /* Heap-allocate: 5 presets × ~420 bytes each + wrapper ≈ 2500 bytes */
    const size_t buf_size = 4096;
    char *buf = malloc(buf_size);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int pos = snprintf(buf, buf_size, "{\"presets\":[");

    for (int i = 0; i < LED_PRESET_COUNT; i++) {
        led_preset_t p;
        led_preset_get(i, &p);

        if (pos >= (int)buf_size - 2) {
            break;  /* buffer full */
        }

        /* Escape name */
        char ename[LED_PRESET_NAME_LEN * 2 + 4];
        json_escape(p.name, ename, sizeof(ename));

        int written = snprintf(buf + pos, buf_size - (size_t)pos,
            "%s{\"slot\":%d,\"name\":\"%s\","
            "\"config\":{"
            "\"enabled\":%s,"
            "\"on_hour\":%d,\"on_minute\":%d,"
            "\"ramp_duration_min\":%d,"
            "\"pause_enabled\":%s,"
            "\"pause_start_hour\":%d,\"pause_start_minute\":%d,"
            "\"pause_end_hour\":%d,\"pause_end_minute\":%d,"
            "\"pause_brightness\":%d,"
            "\"pause_red\":%d,\"pause_green\":%d,\"pause_blue\":%d,"
            "\"off_hour\":%d,\"off_minute\":%d,"
            "\"brightness\":%d,"
            "\"red\":%d,\"green\":%d,\"blue\":%d}}",
            i > 0 ? "," : "",
            i, ename,
            p.config.enabled       ? "true" : "false",
            p.config.on_hour,   p.config.on_minute,
            p.config.ramp_duration_min,
            p.config.pause_enabled ? "true" : "false",
            p.config.pause_start_hour, p.config.pause_start_minute,
            p.config.pause_end_hour,   p.config.pause_end_minute,
            p.config.pause_brightness,
            p.config.pause_red, p.config.pause_green, p.config.pause_blue,
            p.config.off_hour,  p.config.off_minute,
            p.config.brightness,
            p.config.red, p.config.green, p.config.blue);
        if (written > 0 && pos + written < (int)buf_size) {
            pos += written;
        }
    }

    if (pos + 3 < (int)buf_size) {
        buf[pos++] = ']';
        buf[pos++] = '}';
        buf[pos]   = '\0';
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, pos);
    free(buf);
    return err;
}

/* ── LED Presets control endpoint (/api/led_presets  POST) ──────── */
/* Body: {"action":"save","slot":N,"name":"...",<all schedule fields>}  */
/* Body: {"action":"load","slot":N}                                      */

static esp_err_t api_led_presets_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_PRESETS_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "LED Presets POST body: %s", buf);

    char action[16] = {0};
    json_get_str(buf, "\"action\"", action, sizeof(action));

    int slot = json_get_int(buf, "\"slot\"");
    if (slot < 0 || slot >= LED_PRESET_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot");
        return ESP_FAIL;
    }

    if (strcmp(action, "load") == 0) {
        esp_err_t err = led_preset_load(slot);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Load failed");
            return ESP_FAIL;
        }
    } else if (strcmp(action, "save") == 0) {
        char name[LED_PRESET_NAME_LEN] = {0};
        json_get_str(buf, "\"name\"", name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "Preset %d", slot + 1);
        }

        /* Build config from POST fields, starting from current preset config */
        led_preset_t current;
        led_preset_get(slot, &current);
        led_schedule_config_t cfg = current.config;
        int val;
        double dval;

        val = json_get_bool(buf, "\"enabled\"");
        if (val >= 0) cfg.enabled = (val == 1);
        val = json_get_int(buf, "\"on_hour\"");
        if (val >= 0) cfg.on_hour = (uint8_t)val;
        val = json_get_int(buf, "\"on_minute\"");
        if (val >= 0) cfg.on_minute = (uint8_t)val;
        if (json_get_double(buf, "\"ramp_duration_min\"", &dval) == 0)
            cfg.ramp_duration_min = clamp_u16(dval, 120.0);
        val = json_get_bool(buf, "\"pause_enabled\"");
        if (val >= 0) cfg.pause_enabled = (val == 1);
        val = json_get_int(buf, "\"pause_start_hour\"");
        if (val >= 0) cfg.pause_start_hour = (uint8_t)val;
        val = json_get_int(buf, "\"pause_start_minute\"");
        if (val >= 0) cfg.pause_start_minute = (uint8_t)val;
        val = json_get_int(buf, "\"pause_end_hour\"");
        if (val >= 0) cfg.pause_end_hour = (uint8_t)val;
        val = json_get_int(buf, "\"pause_end_minute\"");
        if (val >= 0) cfg.pause_end_minute = (uint8_t)val;
        val = json_get_int(buf, "\"pause_brightness\"");
        if (val >= 0) cfg.pause_brightness = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"pause_red\"");
        if (val >= 0) cfg.pause_red = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"pause_green\"");
        if (val >= 0) cfg.pause_green = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"pause_blue\"");
        if (val >= 0) cfg.pause_blue = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"off_hour\"");
        if (val >= 0) cfg.off_hour = (uint8_t)val;
        val = json_get_int(buf, "\"off_minute\"");
        if (val >= 0) cfg.off_minute = (uint8_t)val;
        val = json_get_int(buf, "\"brightness\"");
        if (val >= 0) cfg.brightness = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"red\"");
        if (val >= 0) cfg.red = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"green\"");
        if (val >= 0) cfg.green = (uint8_t)(val > 255 ? 255 : val);
        val = json_get_int(buf, "\"blue\"");
        if (val >= 0) cfg.blue = (uint8_t)(val > 255 ? 255 : val);

        esp_err_t err = led_preset_save(slot, name, &cfg);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
            return ESP_FAIL;
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
        return ESP_FAIL;
    }

    return api_led_presets_get_handler(req);
}


static esp_err_t api_temperature_get_handler(httpd_req_t *req)
{
    float temp_c = 0.0f;
    bool valid = temperature_sensor_get(&temp_c);

    char buf[JSON_TEMP_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"temperature_c\":%.2f}",
        valid ? "true" : "false",
        valid ? temp_c : 0.0f);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Temperature history GET endpoint (/api/temperature_history  GET) ── */

static esp_err_t api_temp_history_get_handler(httpd_req_t *req)
{
    /* Heap-allocate the sample buffer (~3.5 KB) */
    temp_sample_t *samples = malloc(
        TEMP_HISTORY_MAX_SAMPLES * sizeof(temp_sample_t));
    if (samples == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int count = 0;
    temperature_history_get(samples, &count);

    httpd_resp_set_type(req, "application/json");

    /* Stream the response using chunked encoding to keep RAM usage low.
     * Each sample serialises to ~30 bytes; a small fixed buffer is fine. */
    char chunk[JSON_TEMP_CHUNK_SIZE];
    int n;

    n = snprintf(chunk, sizeof(chunk),
        "{\"count\":%d,\"interval_sec\":%d,\"samples\":[",
        count, CONFIG_TEMP_HISTORY_INTERVAL_SEC);
    httpd_resp_send_chunk(req, chunk, n);

    for (int i = 0; i < count; i++) {
        n = snprintf(chunk, sizeof(chunk),
            "%s{\"t\":%" PRId64 ",\"c\":%.2f}",
            i > 0 ? "," : "",
            (int64_t)samples[i].timestamp,
            (double)samples[i].temp_c);
        httpd_resp_send_chunk(req, chunk, n);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);   /* finalise chunked response */

    free(samples);
    return ESP_OK;
}

/* ── Temperature CSV export (/api/temperature/export.csv  GET) ───── */

static esp_err_t api_temp_csv_get_handler(httpd_req_t *req)
{
    temp_sample_t *samples = malloc(
        TEMP_HISTORY_MAX_SAMPLES * sizeof(temp_sample_t));
    if (samples == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int count = 0;
    temperature_history_get(samples, &count);

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"temperature_history.csv\"");

    char chunk[64];
    int n = snprintf(chunk, sizeof(chunk), "timestamp,temperature_c\r\n");
    httpd_resp_send_chunk(req, chunk, n);

    for (int i = 0; i < count; i++) {
        n = snprintf(chunk, sizeof(chunk),
            "%" PRId64 ",%.2f\r\n",
            (int64_t)samples[i].timestamp,
            (double)samples[i].temp_c);
        httpd_resp_send_chunk(req, chunk, n);
    }

    httpd_resp_send_chunk(req, NULL, 0);   /* finalise chunked response */
    free(samples);
    return ESP_OK;
}

/* ── Telegram GET endpoint (/api/telegram  GET) ──────────────────── */

static esp_err_t api_telegram_get_handler(httpd_req_t *req)
{
    telegram_config_t cfg = telegram_notify_get_config();

    char escaped_chatid[128];
    json_escape(cfg.chat_id, escaped_chatid, sizeof(escaped_chatid));

    /* Use a heap buffer – stack-safe for the httpd task */
    char *buf = malloc(JSON_TG_BUF_SIZE);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int len = snprintf(buf, JSON_TG_BUF_SIZE,
        "{\"bot_token_set\":%s,"
        "\"chat_id\":\"%s\","
        "\"enabled\":%s,"
        "\"temp_alarm_enabled\":%s,"
        "\"temp_high_c\":%.1f,"
        "\"temp_low_c\":%.1f,"
        "\"water_change_enabled\":%s,"
        "\"water_change_days\":%d,"
        "\"fertilizer_enabled\":%s,"
        "\"fertilizer_days\":%d,"
        "\"daily_summary_enabled\":%s,"
        "\"daily_summary_hour\":%d,"
        "\"relay_notify_enabled\":%s,"
        "\"last_water_change\":%" PRId64 ","
        "\"last_fertilizer\":%" PRId64 "}",
        cfg.bot_token[0] != '\0' ? "true" : "false",
        escaped_chatid,
        cfg.enabled ? "true" : "false",
        cfg.temp_alarm_enabled ? "true" : "false",
        (double)cfg.temp_high_c,
        (double)cfg.temp_low_c,
        cfg.water_change_enabled ? "true" : "false",
        cfg.water_change_days,
        cfg.fertilizer_enabled ? "true" : "false",
        cfg.fertilizer_days,
        cfg.daily_summary_enabled ? "true" : "false",
        cfg.daily_summary_hour,
        cfg.relay_notify_enabled ? "true" : "false",
        telegram_notify_get_last_water_change(),
        telegram_notify_get_last_fertilizer());

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, buf, len);
    free(buf);
    return ret;
}

/* ── Telegram POST endpoint (/api/telegram  POST) ────────────────── */

static esp_err_t api_telegram_post_handler(httpd_req_t *req)
{
    char *buf = malloc(POST_BODY_TG_SIZE);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, POST_BODY_TG_SIZE - 1);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';
    ESP_LOGI(TAG, "Telegram POST body: %s", buf);

    /* Start from current config */
    telegram_config_t cfg = telegram_notify_get_config();

    /* Parse fields – only update what is provided */
    char str_val[128];
    if (json_get_str(buf, "\"bot_token\"", str_val, sizeof(str_val)) == 0) {
        strncpy(cfg.bot_token, str_val, sizeof(cfg.bot_token) - 1);
        cfg.bot_token[sizeof(cfg.bot_token) - 1] = '\0';
    }
    if (json_get_str(buf, "\"chat_id\"", str_val, sizeof(str_val)) == 0) {
        strncpy(cfg.chat_id, str_val, sizeof(cfg.chat_id) - 1);
        cfg.chat_id[sizeof(cfg.chat_id) - 1] = '\0';
    }

    int bval;
    bval = json_get_bool(buf, "\"enabled\"");
    if (bval >= 0) cfg.enabled = bval;
    bval = json_get_bool(buf, "\"temp_alarm_enabled\"");
    if (bval >= 0) cfg.temp_alarm_enabled = bval;
    bval = json_get_bool(buf, "\"water_change_enabled\"");
    if (bval >= 0) cfg.water_change_enabled = bval;
    bval = json_get_bool(buf, "\"fertilizer_enabled\"");
    if (bval >= 0) cfg.fertilizer_enabled = bval;
    bval = json_get_bool(buf, "\"daily_summary_enabled\"");
    if (bval >= 0) cfg.daily_summary_enabled = bval;
    bval = json_get_bool(buf, "\"relay_notify_enabled\"");
    if (bval >= 0) cfg.relay_notify_enabled = bval;

    double dval;
    if (json_get_double(buf, "\"temp_high_c\"", &dval) == 0)
        cfg.temp_high_c = (float)dval;
    if (json_get_double(buf, "\"temp_low_c\"", &dval) == 0)
        cfg.temp_low_c = (float)dval;

    /* Enforce temp_high > temp_low – swap if inverted, ensure 0.5°C gap */
    if (cfg.temp_high_c <= cfg.temp_low_c) {
        float tmp_high = cfg.temp_high_c;
        float tmp_low  = cfg.temp_low_c;
        cfg.temp_low_c  = tmp_high;
        cfg.temp_high_c = tmp_low;
        if (cfg.temp_high_c <= cfg.temp_low_c) {
            cfg.temp_high_c = cfg.temp_low_c + 0.5f;
        }
    }

    if (json_get_double(buf, "\"water_change_days\"", &dval) == 0)
        cfg.water_change_days = (int)dval;
    if (json_get_double(buf, "\"fertilizer_days\"", &dval) == 0)
        cfg.fertilizer_days = (int)dval;
    if (json_get_double(buf, "\"daily_summary_hour\"", &dval) == 0)
        cfg.daily_summary_hour = (int)dval;

    free(buf);

    telegram_notify_set_config(&cfg);

    return api_telegram_get_handler(req);
}

/* ── Telegram test endpoint (/api/telegram_test  POST) ───────────── */

static esp_err_t api_telegram_test_handler(httpd_req_t *req)
{
    esp_err_t err = telegram_notify_send(
        "\xf0\x9f\x90\x9f <b>Aquarium Controller</b>\n\n"
        "Test message received successfully!\n"
        "Telegram notifications are working.");

    char buf[128];
    int len;
    if (err == ESP_OK) {
        len = snprintf(buf, sizeof(buf), "{\"ok\":true}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        len = snprintf(buf, sizeof(buf),
            "{\"ok\":false,\"error\":\"clock_not_synced\"}");
    } else {
        len = snprintf(buf, sizeof(buf),
            "{\"ok\":false,\"error\":\"send_failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Water change reset endpoint (/api/telegram_wc  POST) ────────── */

static esp_err_t api_telegram_wc_handler(httpd_req_t *req)
{
    telegram_notify_reset_water_change();

    char buf[64];
    int len = snprintf(buf, sizeof(buf),
        "{\"last_water_change\":%" PRId64 "}",
        telegram_notify_get_last_water_change());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Fertilizer reset endpoint (/api/telegram_fert  POST) ────────── */

static esp_err_t api_telegram_fert_handler(httpd_req_t *req)
{
    telegram_notify_reset_fertilizer();

    char buf[64];
    int len = snprintf(buf, sizeof(buf),
        "{\"last_fertilizer\":%" PRId64 "}",
        telegram_notify_get_last_fertilizer());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Relay GET endpoint (/api/relays  GET) ───────────────────────── */

static esp_err_t api_relays_get_handler(httpd_req_t *req)
{
    relay_state_t relays[RELAY_COUNT];
    relay_controller_get_all(relays);

    char escaped_name[RELAY_NAME_MAX * 2];

    /* Build JSON response using chunked encoding */
    httpd_resp_set_type(req, "application/json");

    char chunk[JSON_RELAY_CHUNK_SIZE];
    int n;

    n = snprintf(chunk, sizeof(chunk), "{\"count\":%d,\"relays\":[", RELAY_COUNT);
    httpd_resp_send_chunk(req, chunk, n);

    for (int i = 0; i < RELAY_COUNT; i++) {
        json_escape(relays[i].name, escaped_name, sizeof(escaped_name));

        /* Header part of each relay JSON object */
        n = snprintf(chunk, sizeof(chunk),
            "%s{\"index\":%d,\"on\":%s,\"name\":\"%s\",\"schedules\":[",
            i > 0 ? "," : "",
            i,
            relays[i].on ? "true" : "false",
            escaped_name);
        httpd_resp_send_chunk(req, chunk, n);

        /* Emit each schedule slot */
        for (int s = 0; s < RELAY_SCHEDULE_SLOTS; s++) {
            n = snprintf(chunk, sizeof(chunk),
                "%s{\"enabled\":%s,\"on_min\":%d,\"off_min\":%d}",
                s > 0 ? "," : "",
                relays[i].schedules[s].enabled ? "true" : "false",
                relays[i].schedules[s].on_min,
                relays[i].schedules[s].off_min);
            httpd_resp_send_chunk(req, chunk, n);
        }

        httpd_resp_send_chunk(req, "]}", 2);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Relay POST endpoint (/api/relays  POST) ─────────────────────── */

static esp_err_t api_relays_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_RELAY_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Relay POST body: %s", buf);

    int idx = json_get_int(buf, "\"index\"");
    if (idx < 0 || idx >= RELAY_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index");
        return ESP_FAIL;
    }

    /* Apply on/off if provided */
    int on_val = json_get_bool(buf, "\"on\"");
    if (on_val >= 0) {
        relay_controller_set(idx, on_val == 1);
    }

    /* Apply name if provided */
    char name[RELAY_NAME_MAX];
    if (json_get_str(buf, "\"name\"", name, sizeof(name)) == 0) {
        /* Reject empty names */
        if (name[0] != '\0') {
            relay_controller_set_name(idx, name);
        }
    }

    /* Apply schedule if provided */
    int sched_en = json_get_bool(buf, "\"schedule_enabled\"");
    if (sched_en >= 0) {
        /* Which slot to update (default 0 for backwards compat) */
        int slot = json_get_int(buf, "\"schedule_slot\"");
        if (slot < 0) slot = 0;
        if (slot >= RELAY_SCHEDULE_SLOTS) slot = RELAY_SCHEDULE_SLOTS - 1;

        relay_state_t all[RELAY_COUNT];
        relay_controller_get_all(all);
        relay_schedule_t sched = all[idx].schedules[slot];

        sched.enabled = (sched_en == 1);

        double dval;
        if (json_get_double(buf, "\"schedule_on_min\"", &dval) == 0) {
            int v = (int)dval;
            if (v >= 0 && v <= 1439) sched.on_min = (uint16_t)v;
        }
        if (json_get_double(buf, "\"schedule_off_min\"", &dval) == 0) {
            int v = (int)dval;
            if (v >= 0 && v <= 1439) sched.off_min = (uint16_t)v;
        }

        relay_controller_set_schedule(idx, slot, &sched);
    }

    /* Respond with full relay state */
    return api_relays_get_handler(req);
}

/* ── DuckDNS GET endpoint (/api/duckdns  GET) ───────────────────── */

static esp_err_t api_duckdns_get_handler(httpd_req_t *req)
{
    duckdns_config_t cfg = duckdns_get_config();

    char escaped_domain[128];
    json_escape(cfg.domain, escaped_domain, sizeof(escaped_domain));

    const char *status = duckdns_get_last_status();
    char escaped_status[128];
    json_escape(status, escaped_status, sizeof(escaped_status));

    char buf[JSON_DDNS_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"domain\":\"%s\","
        "\"token_set\":%s,"
        "\"enabled\":%s,"
        "\"last_status\":\"%s\"}",
        escaped_domain,
        cfg.token[0] != '\0' ? "true" : "false",
        cfg.enabled ? "true" : "false",
        escaped_status);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── DuckDNS POST endpoint (/api/duckdns  POST) ─────────────────── */

static esp_err_t api_duckdns_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_DDNS_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "DuckDNS POST body: %s", buf);

    duckdns_config_t cfg = duckdns_get_config();

    char str_val[64];
    if (json_get_str(buf, "\"domain\"", str_val, sizeof(str_val)) == 0) {
        strncpy(cfg.domain, str_val, sizeof(cfg.domain) - 1);
        cfg.domain[sizeof(cfg.domain) - 1] = '\0';
    }
    if (json_get_str(buf, "\"token\"", str_val, sizeof(str_val)) == 0) {
        strncpy(cfg.token, str_val, sizeof(cfg.token) - 1);
        cfg.token[sizeof(cfg.token) - 1] = '\0';
    }

    int bval = json_get_bool(buf, "\"enabled\"");
    if (bval >= 0) {
        cfg.enabled = bval;
    }

    duckdns_set_config(&cfg);

    return api_duckdns_get_handler(req);
}

/* ── DuckDNS update now endpoint (/api/duckdns_update  POST) ─────── */

static esp_err_t api_duckdns_update_handler(httpd_req_t *req)
{
    esp_err_t err = duckdns_update_now();

    const char *status = duckdns_get_last_status();
    char escaped_status[128];
    json_escape(status, escaped_status, sizeof(escaped_status));

    char resp[192];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"last_status\":\"%s\"}",
        err == ESP_OK ? "true" : "false",
        escaped_status);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len);
}

/* ── Auto-heater GET endpoint (/api/heater  GET) ─────────────────── */

static esp_err_t api_heater_get_handler(httpd_req_t *req)
{
    auto_heater_config_t cfg = auto_heater_get_config();

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,"
        "\"relay_index\":%d,"
        "\"target_temp_c\":%.1f,"
        "\"hysteresis_c\":%.1f}",
        cfg.enabled ? "true" : "false",
        cfg.relay_index,
        (double)cfg.target_temp_c,
        (double)cfg.hysteresis_c);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Auto-heater POST endpoint (/api/heater  POST) ───────────────── */

static esp_err_t api_heater_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_RELAY_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';
    ESP_LOGI(TAG, "Heater POST body: %s", buf);

    auto_heater_config_t cfg = auto_heater_get_config();

    int bval = json_get_bool(buf, "\"enabled\"");
    if (bval >= 0) cfg.enabled = (bval == 1);

    double dval;
    if (json_get_double(buf, "\"relay_index\"", &dval) == 0)
        cfg.relay_index = (int)dval;
    if (json_get_double(buf, "\"target_temp_c\"", &dval) == 0)
        cfg.target_temp_c = (float)dval;
    if (json_get_double(buf, "\"hysteresis_c\"", &dval) == 0)
        cfg.hysteresis_c = (float)dval;

    auto_heater_set_config(&cfg);

    return api_heater_get_handler(req);
}

/* ── OTA update endpoints ────────────────────────────────────────── */

/**
 * @brief Start an OTA firmware update.  POST body: {"url":"https://…"}
 */
static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_DDNS_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';
    ESP_LOGI(TAG, "OTA POST body: %s", buf);

    char url[256];
    if (json_get_str(buf, "\"url\"", url, sizeof(url)) != 0 ||
        url[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'url' field");
        return ESP_FAIL;
    }

    esp_err_t err = ota_update_start(url);
    char resp[128];
    int len;
    if (err == ESP_OK) {
        len = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        len = snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"error\":\"update_already_in_progress\"}");
    } else {
        len = snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"error\":\"start_failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len);
}

/**
 * @brief Get the current OTA update progress.
 */
static esp_err_t api_ota_status_get_handler(httpd_req_t *req)
{
    ota_progress_t p = ota_update_get_progress();

    static const char *const status_names[] = {
        [OTA_STATUS_IDLE]        = "idle",
        [OTA_STATUS_DOWNLOADING] = "downloading",
        [OTA_STATUS_FLASHING]    = "flashing",
        [OTA_STATUS_DONE]        = "done",
        [OTA_STATUS_ERROR]       = "error",
    };
    const char *sn = (p.status <= OTA_STATUS_ERROR) ?
                     status_names[p.status] : "unknown";

    char escaped_err[128];
    json_escape(p.error_msg, escaped_err, sizeof(escaped_err));

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"progress\":%d,\"error\":\"%s\"}",
        sn, p.progress_pct, escaped_err);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── CO2 controller GET endpoint (/api/co2  GET) ─────────────────── */

static esp_err_t api_co2_get_handler(httpd_req_t *req)
{
    co2_config_t cfg = co2_controller_get_config();

    char buf[JSON_CO2_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,"
        "\"relay_index\":%d,"
        "\"pre_on_min\":%d,"
        "\"post_off_min\":%d}",
        cfg.enabled ? "true" : "false",
        cfg.relay_index,
        cfg.pre_on_min,
        cfg.post_off_min);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── CO2 controller POST endpoint (/api/co2  POST) ───────────────── */

static esp_err_t api_co2_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_CO2_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    co2_config_t cfg = co2_controller_get_config();

    int bval = json_get_bool(buf, "\"enabled\"");
    if (bval >= 0) cfg.enabled = (bval == 1);

    double dval;
    if (json_get_double(buf, "\"relay_index\"", &dval) == 0)
        cfg.relay_index = (int)dval;
    if (json_get_double(buf, "\"pre_on_min\"", &dval) == 0)
        cfg.pre_on_min = (int)dval;
    if (json_get_double(buf, "\"post_off_min\"", &dval) == 0)
        cfg.post_off_min = (int)dval;

    co2_controller_set_config(&cfg);

    return api_co2_get_handler(req);
}

/* ── Timezone GET endpoint (/api/timezone  GET) ──────────────────── */

static esp_err_t api_timezone_get_handler(httpd_req_t *req)
{
    char tz[TZ_STRING_MAX];
    timezone_manager_get(tz, sizeof(tz));

    char escaped[TZ_STRING_MAX * 2];
    json_escape(tz, escaped, sizeof(escaped));

    char buf[JSON_TZ_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf), "{\"tz\":\"%s\"}", escaped);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Timezone POST endpoint (/api/timezone  POST) ────────────────── */

static esp_err_t api_timezone_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_TZ_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char tz_str[TZ_STRING_MAX];
    if (json_get_str(buf, "\"tz\"", tz_str, sizeof(tz_str)) != 0 ||
        tz_str[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'tz' field");
        return ESP_FAIL;
    }

    esp_err_t err = timezone_manager_set(tz_str);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid TZ string");
        return ESP_FAIL;
    }

    return api_timezone_get_handler(req);
}

/* ── Feeding mode GET endpoint (/api/feeding  GET) ───────────────── */

static esp_err_t api_feeding_get_handler(httpd_req_t *req)
{
    feeding_config_t cfg = feeding_mode_get_config();
    bool active      = feeding_mode_is_active();
    int  remaining_s = feeding_mode_get_remaining_s();

    char buf[JSON_FEEDING_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"active\":%s,"
        "\"remaining_s\":%d,"
        "\"relay_index\":%d,"
        "\"duration_min\":%d,"
        "\"dim_lights\":%s,"
        "\"dim_brightness\":%d}",
        active ? "true" : "false",
        remaining_s,
        cfg.relay_index,
        cfg.duration_min,
        cfg.dim_lights ? "true" : "false",
        cfg.dim_brightness);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Feeding mode POST endpoint (/api/feeding  POST) ─────────────── */
/*
 * Actions:
 *   {"action":"start"}              – start feeding mode
 *   {"action":"stop"}               – stop feeding mode
 *   {"relay_index":N,"duration_min":M,...} – update config only
 */

static esp_err_t api_feeding_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_FEEDING_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char action[16] = {0};
    json_get_str(buf, "\"action\"", action, sizeof(action));

    if (strcmp(action, "start") == 0) {
        feeding_mode_start();
    } else if (strcmp(action, "stop") == 0) {
        feeding_mode_stop();
    } else {
        /* Config update */
        feeding_config_t cfg = feeding_mode_get_config();
        double dval;
        int bval;

        if (json_get_double(buf, "\"relay_index\"", &dval) == 0)
            cfg.relay_index = (int)dval;
        if (json_get_double(buf, "\"duration_min\"", &dval) == 0)
            cfg.duration_min = (int)dval;
        bval = json_get_bool(buf, "\"dim_lights\"");
        if (bval >= 0) cfg.dim_lights = (bval == 1);
        if (json_get_double(buf, "\"dim_brightness\"", &dval) == 0)
            cfg.dim_brightness = (uint8_t)(dval > 255 ? 255 : dval);

        feeding_mode_set_config(&cfg);
    }

    return api_feeding_get_handler(req);
}

/* ── LED Scene GET endpoint (/api/scene  GET) ────────────────────── */

static esp_err_t api_scene_get_handler(httpd_req_t *req)
{
    led_scenes_config_t cfg = led_scenes_get_config();
    led_scene_t active = led_scenes_get_active();

    char buf[JSON_SCENE_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"active\":%d,"
        "\"sunrise_duration_min\":%d,"
        "\"sunrise_max_brightness\":%d,"
        "\"sunset_duration_min\":%d,"
        "\"moonlight_brightness\":%d,"
        "\"moonlight_r\":%d,\"moonlight_g\":%d,\"moonlight_b\":%d,"
        "\"storm_intensity\":%d,"
        "\"clouds_depth\":%d,"
        "\"clouds_period_s\":%d}",
        (int)active,
        cfg.sunrise_duration_min,
        cfg.sunrise_max_brightness,
        cfg.sunset_duration_min,
        cfg.moonlight_brightness,
        cfg.moonlight_r, cfg.moonlight_g, cfg.moonlight_b,
        cfg.storm_intensity,
        cfg.clouds_depth,
        cfg.clouds_period_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── LED Scene POST endpoint (/api/scene  POST) ──────────────────── */
/*
 * {"start_scene":N}                          – start/stop scene N
 * {"sunrise_duration_min":30,...}            – update config only
 * Both can be combined: start + config update in one call.
 */

static esp_err_t api_scene_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_SCENE_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    /* Config update */
    led_scenes_config_t cfg = led_scenes_get_config();
    double dval;

    if (json_get_double(buf, "\"sunrise_duration_min\"", &dval) == 0)
        cfg.sunrise_duration_min = (uint16_t)(dval > 120 ? 120 : (dval < 5 ? 5 : dval));
    if (json_get_double(buf, "\"sunrise_max_brightness\"", &dval) == 0)
        cfg.sunrise_max_brightness = (uint8_t)(dval > 255 ? 255 : dval);
    if (json_get_double(buf, "\"sunset_duration_min\"", &dval) == 0)
        cfg.sunset_duration_min = (uint8_t)(dval > 120 ? 120 : (dval < 5 ? 5 : dval));
    if (json_get_double(buf, "\"moonlight_brightness\"", &dval) == 0)
        cfg.moonlight_brightness = (uint8_t)(dval > 60 ? 60 : dval);
    if (json_get_double(buf, "\"moonlight_r\"", &dval) == 0)
        cfg.moonlight_r = (uint8_t)(dval > 255 ? 255 : dval);
    if (json_get_double(buf, "\"moonlight_g\"", &dval) == 0)
        cfg.moonlight_g = (uint8_t)(dval > 255 ? 255 : dval);
    if (json_get_double(buf, "\"moonlight_b\"", &dval) == 0)
        cfg.moonlight_b = (uint8_t)(dval > 255 ? 255 : dval);
    if (json_get_double(buf, "\"storm_intensity\"", &dval) == 0)
        cfg.storm_intensity = (uint8_t)(dval > 100 ? 100 : dval);
    if (json_get_double(buf, "\"clouds_depth\"", &dval) == 0)
        cfg.clouds_depth = (uint8_t)(dval > 80 ? 80 : dval);
    if (json_get_double(buf, "\"clouds_period_s\"", &dval) == 0)
        cfg.clouds_period_s = (uint16_t)(dval > 600 ? 600 : (dval < 10 ? 10 : dval));

    led_scenes_set_config(&cfg);

    /* Scene activation */
    int start_scene = json_get_int(buf, "\"start_scene\"");
    if (start_scene >= 0) {
        led_scenes_start((led_scene_t)start_scene);
    }

    return api_scene_get_handler(req);
}

/* ── Daily Cycle GET endpoint (/api/daily_cycle  GET) ────────────── */

static esp_err_t api_daily_cycle_get_handler(httpd_req_t *req)
{
    daily_cycle_config_t cfg = daily_cycle_get_config();
    daily_cycle_phase_t  phase = daily_cycle_get_phase();
    int sunrise_min = daily_cycle_get_sunrise_min();
    int sunset_min  = daily_cycle_get_sunset_min();

    char buf[JSON_DAILY_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,"
        "\"latitude\":%.4f,"
        "\"longitude\":%.4f,"
        "\"phase\":%d,"
        "\"sunrise_min\":%d,"
        "\"sunset_min\":%d}",
        cfg.enabled ? "true" : "false",
        cfg.latitude,
        cfg.longitude,
        (int)phase,
        sunrise_min,
        sunset_min);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Daily Cycle POST endpoint (/api/daily_cycle  POST) ──────────── */
/*
 * {"enabled":true,"latitude":45.46,"longitude":9.19}
 * Any subset of fields may be provided; missing fields retain current values.
 */

static esp_err_t api_daily_cycle_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_DAILY_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    daily_cycle_config_t cfg = daily_cycle_get_config();
    double dval;

    int en = json_get_bool(buf, "\"enabled\"");
    if (en >= 0) cfg.enabled = (bool)en;

    if (json_get_double(buf, "\"latitude\"",  &dval) == 0) cfg.latitude  = (float)dval;
    if (json_get_double(buf, "\"longitude\"", &dval) == 0) cfg.longitude = (float)dval;

    esp_err_t err = daily_cycle_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config");
        return ESP_FAIL;
    }

    return api_daily_cycle_get_handler(req);
}

/* ── /api/voice/config (GET) ─────────────────────────────────────── */

static esp_err_t api_voice_config_get_handler(httpd_req_t *req)
{
    voice_config_t cfg = voice_control_get_config();
    char buf[JSON_VOICE_BUF_SIZE];
    snprintf(buf, sizeof(buf),
        "{"
          "\"enabled\":%s,"
          "\"api_key_set\":%s,"
          "\"stt_model\":\"%s\","
          "\"llm_model\":\"%s\","
          "\"record_ms\":%d,"
          "\"i2s_sck_io\":%d,"
          "\"i2s_ws_io\":%d,"
          "\"i2s_sd_io\":%d"
        "}",
        cfg.enabled ? "true" : "false",
        cfg.groq_api_key[0] != '\0' ? "true" : "false",
        cfg.stt_model,
        cfg.llm_model,
        cfg.record_ms,
        cfg.i2s_sck_io,
        cfg.i2s_ws_io,
        cfg.i2s_sd_io);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── /api/voice/config (POST) ────────────────────────────────────── */

static esp_err_t api_voice_config_post_handler(httpd_req_t *req)
{
    char body[POST_BODY_VOICE_SIZE];
    int  received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    voice_config_t cfg = voice_control_get_config();

    /* enabled */
    const char *en = strstr(body, "\"enabled\":");
    if (en) cfg.enabled = (strstr(en + 10, "true") != NULL);

    /* api_key – only update when present and not the placeholder */
    const char *key_tag = strstr(body, "\"api_key\":\"");
    if (key_tag) {
        key_tag += 11;
        const char *end = strchr(key_tag, '"');
        if (end && end > key_tag && (end - key_tag) < (int)sizeof(cfg.groq_api_key)) {
            size_t klen = (size_t)(end - key_tag);
            strncpy(cfg.groq_api_key, key_tag, klen);
            cfg.groq_api_key[klen] = '\0';
        }
    }

    /* stt_model */
    const char *sm = strstr(body, "\"stt_model\":\"");
    if (sm) {
        sm += 13;
        const char *e = strchr(sm, '"');
        if (e && e > sm && (e - sm) < (int)sizeof(cfg.stt_model)) {
            size_t l = (size_t)(e - sm);
            strncpy(cfg.stt_model, sm, l);
            cfg.stt_model[l] = '\0';
        }
    }

    /* llm_model */
    const char *lm = strstr(body, "\"llm_model\":\"");
    if (lm) {
        lm += 13;
        const char *e = strchr(lm, '"');
        if (e && e > lm && (e - lm) < (int)sizeof(cfg.llm_model)) {
            size_t l = (size_t)(e - lm);
            strncpy(cfg.llm_model, lm, l);
            cfg.llm_model[l] = '\0';
        }
    }

    /* record_ms */
    const char *rm = strstr(body, "\"record_ms\":");
    if (rm) cfg.record_ms = (int)strtol(rm + 12, NULL, 10);

    /* GPIOs */
    const char *sck = strstr(body, "\"i2s_sck_io\":");
    if (sck) cfg.i2s_sck_io = (int)strtol(sck + 13, NULL, 10);
    const char *ws = strstr(body, "\"i2s_ws_io\":");
    if (ws)  cfg.i2s_ws_io  = (int)strtol(ws  + 12, NULL, 10);
    const char *sd = strstr(body, "\"i2s_sd_io\":");
    if (sd)  cfg.i2s_sd_io  = (int)strtol(sd  + 12, NULL, 10);

    esp_err_t err = voice_control_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── /api/voice/record (POST) ────────────────────────────────────── */

static esp_err_t api_voice_record_post_handler(httpd_req_t *req)
{
    /* Consume any request body (ignored) */
    char dummy[32];
    httpd_req_recv(req, dummy, sizeof(dummy) - 1);

    esp_err_t err = voice_control_start_record();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        char buf[128];
        voice_status_t st = voice_control_get_status();
        if (st >= VOICE_STATUS_RECORDING && st <= VOICE_STATUS_PROCESSING) {
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"Pipeline in corso\"}");
        } else {
            snprintf(buf, sizeof(buf),
                     "{\"ok\":false,\"error\":\"Voce disabilitata o API key mancante\"}");
        }
        httpd_resp_sendstr(req, buf);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Avvio fallito\"}");
    }
    return ESP_OK;
}

/* ── /api/voice/status (GET) ─────────────────────────────────────── */

static esp_err_t api_voice_status_get_handler(httpd_req_t *req)
{
    voice_status_t st = voice_control_get_status();
    char transcript[512], command[512], result[128];
    voice_control_get_transcript(transcript, sizeof(transcript));
    voice_control_get_last_command(command, sizeof(command));
    voice_control_get_last_result(result, sizeof(result));

    /* JSON-escape the strings */
    char esc_transcript[1024], esc_command[1024], esc_result[256];
    json_escape(transcript, esc_transcript, sizeof(esc_transcript));
    json_escape(command,    esc_command,    sizeof(esc_command));
    json_escape(result,     esc_result,     sizeof(esc_result));

    char buf[JSON_VOICE_BUF_SIZE + 1024];
    snprintf(buf, sizeof(buf),
        "{"
          "\"status\":%d,"
          "\"transcript\":\"%s\","
          "\"command\":\"%s\","
          "\"result\":\"%s\""
        "}",
        (int)st, esc_transcript, esc_command, esc_result);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static const httpd_uri_t uri_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_status = {
    .uri      = "/api/status",
    .method   = HTTP_GET,
    .handler  = api_status_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_health = {
    .uri      = "/api/health",
    .method   = HTTP_GET,
    .handler  = api_health_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_leds_get = {
    .uri      = "/api/leds",
    .method   = HTTP_GET,
    .handler  = api_leds_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_leds_post = {
    .uri      = "/api/leds",
    .method   = HTTP_POST,
    .handler  = api_leds_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_led_sched_get = {
    .uri      = "/api/led_schedule",
    .method   = HTTP_GET,
    .handler  = api_led_schedule_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_led_sched_post = {
    .uri      = "/api/led_schedule",
    .method   = HTTP_POST,
    .handler  = api_led_schedule_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_led_presets_get = {
    .uri      = "/api/led_presets",
    .method   = HTTP_GET,
    .handler  = api_led_presets_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_led_presets_post = {
    .uri      = "/api/led_presets",
    .method   = HTTP_POST,
    .handler  = api_led_presets_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_temp_get = {
    .uri      = "/api/temperature",
    .method   = HTTP_GET,
    .handler  = api_temperature_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_temp_hist_get = {
    .uri      = "/api/temperature_history",
    .method   = HTTP_GET,
    .handler  = api_temp_history_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_temp_csv_get = {
    .uri      = "/api/temperature/export.csv",
    .method   = HTTP_GET,
    .handler  = api_temp_csv_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tg_get = {
    .uri      = "/api/telegram",
    .method   = HTTP_GET,
    .handler  = api_telegram_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tg_post = {
    .uri      = "/api/telegram",
    .method   = HTTP_POST,
    .handler  = api_telegram_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tg_test = {
    .uri      = "/api/telegram_test",
    .method   = HTTP_POST,
    .handler  = api_telegram_test_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tg_wc = {
    .uri      = "/api/telegram_wc",
    .method   = HTTP_POST,
    .handler  = api_telegram_wc_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tg_fert = {
    .uri      = "/api/telegram_fert",
    .method   = HTTP_POST,
    .handler  = api_telegram_fert_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_relays_get = {
    .uri      = "/api/relays",
    .method   = HTTP_GET,
    .handler  = api_relays_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_relays_post = {
    .uri      = "/api/relays",
    .method   = HTTP_POST,
    .handler  = api_relays_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ddns_get = {
    .uri      = "/api/duckdns",
    .method   = HTTP_GET,
    .handler  = api_duckdns_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ddns_post = {
    .uri      = "/api/duckdns",
    .method   = HTTP_POST,
    .handler  = api_duckdns_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ddns_update = {
    .uri      = "/api/duckdns_update",
    .method   = HTTP_POST,
    .handler  = api_duckdns_update_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ota_post = {
    .uri      = "/api/ota",
    .method   = HTTP_POST,
    .handler  = api_ota_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ota_status = {
    .uri      = "/api/ota_status",
    .method   = HTTP_GET,
    .handler  = api_ota_status_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_heater_get = {
    .uri      = "/api/heater",
    .method   = HTTP_GET,
    .handler  = api_heater_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_heater_post = {
    .uri      = "/api/heater",
    .method   = HTTP_POST,
    .handler  = api_heater_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_co2_get = {
    .uri      = "/api/co2",
    .method   = HTTP_GET,
    .handler  = api_co2_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_co2_post = {
    .uri      = "/api/co2",
    .method   = HTTP_POST,
    .handler  = api_co2_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tz_get = {
    .uri      = "/api/timezone",
    .method   = HTTP_GET,
    .handler  = api_timezone_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_tz_post = {
    .uri      = "/api/timezone",
    .method   = HTTP_POST,
    .handler  = api_timezone_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_feeding_get = {
    .uri      = "/api/feeding",
    .method   = HTTP_GET,
    .handler  = api_feeding_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_feeding_post = {
    .uri      = "/api/feeding",
    .method   = HTTP_POST,
    .handler  = api_feeding_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_scene_get = {
    .uri      = "/api/scene",
    .method   = HTTP_GET,
    .handler  = api_scene_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_scene_post = {
    .uri      = "/api/scene",
    .method   = HTTP_POST,
    .handler  = api_scene_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_daily_cycle_get = {
    .uri      = "/api/daily_cycle",
    .method   = HTTP_GET,
    .handler  = api_daily_cycle_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_daily_cycle_post = {
    .uri      = "/api/daily_cycle",
    .method   = HTTP_POST,
    .handler  = api_daily_cycle_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_voice_config_get = {
    .uri      = "/api/voice/config",
    .method   = HTTP_GET,
    .handler  = api_voice_config_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_voice_config_post = {
    .uri      = "/api/voice/config",
    .method   = HTTP_POST,
    .handler  = api_voice_config_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_voice_record_post = {
    .uri      = "/api/voice/record",
    .method   = HTTP_POST,
    .handler  = api_voice_record_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_voice_status_get = {
    .uri      = "/api/voice/status",
    .method   = HTTP_GET,
    .handler  = api_voice_status_get_handler,
    .user_ctx = NULL,
};

/* ── Public API ──────────────────────────────────────────────────── */

/* Embedded TLS certificate and key (built from server.crt / server.key) */
#ifdef CONFIG_AQUARIUM_HTTPS_ENABLE
extern const uint8_t server_cert_pem_start[] asm("_binary_server_crt_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_crt_end");
extern const uint8_t server_key_pem_start[]  asm("_binary_server_key_start");
extern const uint8_t server_key_pem_end[]    asm("_binary_server_key_end");
#endif

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

#ifdef CONFIG_AQUARIUM_HTTPS_ENABLE
    /* ── HTTPS mode ─────────────────────────────────────────────── */
    httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_config.httpd.stack_size       = HTTP_STACK_SIZE;
    ssl_config.httpd.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    ssl_config.httpd.lru_purge_enable = true;

    ssl_config.servercert     = server_cert_pem_start;
    ssl_config.servercert_len = (size_t)(server_cert_pem_end - server_cert_pem_start);
    ssl_config.prvtkey_pem    = server_key_pem_start;
    ssl_config.prvtkey_len    = (size_t)(server_key_pem_end - server_key_pem_start);

    ESP_LOGI(TAG, "Starting HTTPS server on port 443");
    esp_err_t ret = httpd_ssl_start(&s_server, &ssl_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(ret));
        return ret;
    }
#else
    /* ── HTTP mode ──────────────────────────────────────────────── */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = HTTP_STACK_SIZE;
    config.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_api_status);
    httpd_register_uri_handler(s_server, &uri_api_health);
    httpd_register_uri_handler(s_server, &uri_api_leds_get);
    httpd_register_uri_handler(s_server, &uri_api_leds_post);
    httpd_register_uri_handler(s_server, &uri_api_led_sched_get);
    httpd_register_uri_handler(s_server, &uri_api_led_sched_post);
    httpd_register_uri_handler(s_server, &uri_api_led_presets_get);
    httpd_register_uri_handler(s_server, &uri_api_led_presets_post);
    httpd_register_uri_handler(s_server, &uri_api_temp_get);
    httpd_register_uri_handler(s_server, &uri_api_temp_hist_get);
    httpd_register_uri_handler(s_server, &uri_api_temp_csv_get);
    httpd_register_uri_handler(s_server, &uri_api_tg_get);
    httpd_register_uri_handler(s_server, &uri_api_tg_post);
    httpd_register_uri_handler(s_server, &uri_api_tg_test);
    httpd_register_uri_handler(s_server, &uri_api_tg_wc);
    httpd_register_uri_handler(s_server, &uri_api_tg_fert);
    httpd_register_uri_handler(s_server, &uri_api_relays_get);
    httpd_register_uri_handler(s_server, &uri_api_relays_post);
    httpd_register_uri_handler(s_server, &uri_api_ddns_get);
    httpd_register_uri_handler(s_server, &uri_api_ddns_post);
    httpd_register_uri_handler(s_server, &uri_api_ddns_update);
    httpd_register_uri_handler(s_server, &uri_api_ota_post);
    httpd_register_uri_handler(s_server, &uri_api_ota_status);
    httpd_register_uri_handler(s_server, &uri_api_heater_get);
    httpd_register_uri_handler(s_server, &uri_api_heater_post);
    httpd_register_uri_handler(s_server, &uri_api_co2_get);
    httpd_register_uri_handler(s_server, &uri_api_co2_post);
    httpd_register_uri_handler(s_server, &uri_api_tz_get);
    httpd_register_uri_handler(s_server, &uri_api_tz_post);
    httpd_register_uri_handler(s_server, &uri_api_feeding_get);
    httpd_register_uri_handler(s_server, &uri_api_feeding_post);
    httpd_register_uri_handler(s_server, &uri_api_scene_get);
    httpd_register_uri_handler(s_server, &uri_api_scene_post);
    httpd_register_uri_handler(s_server, &uri_api_daily_cycle_get);
    httpd_register_uri_handler(s_server, &uri_api_daily_cycle_post);
    httpd_register_uri_handler(s_server, &uri_api_voice_config_get);
    httpd_register_uri_handler(s_server, &uri_api_voice_config_post);
    httpd_register_uri_handler(s_server, &uri_api_voice_record_post);
    httpd_register_uri_handler(s_server, &uri_api_voice_status_get);

#ifdef CONFIG_AQUARIUM_HTTPS_ENABLE
    ESP_LOGI(TAG, "HTTPS server started – open https://<device-ip>/ in a browser");
    ESP_LOGI(TAG, "Note: self-signed cert will trigger a browser security warning");
#else
    ESP_LOGI(TAG, "HTTP server started – open http://<device-ip>/ in a browser");
#endif
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
#ifdef CONFIG_AQUARIUM_HTTPS_ENABLE
        httpd_ssl_stop(s_server);
#else
        httpd_stop(s_server);
#endif
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP(S) server stopped");
    }
}
