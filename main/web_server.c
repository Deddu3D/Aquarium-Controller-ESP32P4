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
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "led_controller.h"
#include "led_scenes.h"
#include "geolocation.h"
#include "sun_position.h"
#include "temperature_sensor.h"
#include "temperature_history.h"
#include "telegram_notify.h"
#include "relay_controller.h"
#include "duckdns.h"
#include "ota_update.h"
#include "auto_heater.h"
#include "esp_ota_ops.h"

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
#define JSON_SCENES_BUF_SIZE   768
#define JSON_TEMP_BUF_SIZE     128
#define JSON_GEO_BUF_SIZE      384
#define JSON_TG_BUF_SIZE       768
#define JSON_DDNS_BUF_SIZE     384
#define JSON_RELAY_CHUNK_SIZE  256
#define JSON_TEMP_CHUNK_SIZE   64

/* HTTP request body receive sizes */
#define POST_BODY_LED_SIZE     256
#define POST_BODY_SCENE_SIZE   512
#define POST_BODY_GEO_SIZE     256
#define POST_BODY_TG_SIZE      512
#define POST_BODY_RELAY_SIZE   256
#define POST_BODY_DDNS_SIZE    256

/* HTTP server configuration */
#define HTTP_STACK_SIZE        8192
#define HTTP_MAX_URI_HANDLERS  32

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
    ".scene-info{display:flex;align-items:center;gap:.5rem;margin-bottom:.3rem}"
    ".scene-dot{width:12px;height:12px;border-radius:50%%;background:#38bdf8}"
    ".scene-name{font-size:1.1rem;font-weight:600}"
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
    "<div class=\"sun-bar-wrap\" id=\"sun-section\" style=\"display:none\">"
    "<div class=\"sun-label\" id=\"sun-label\">SUNLIGHT: --</div>"
    "<div class=\"sun-bar\"><div class=\"sun-dot\" id=\"sun-dot\" style=\"left:0%%\"></div></div>"
    "</div>"
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
    "<div class=\"scene-info\">"
    "<span class=\"scene-dot\" id=\"scene-dot\"></span>"
    "<span class=\"scene-name\" id=\"scene-name\">Off</span>"
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
    "<div class=\"card\">"
    "<div class=\"card-hdr\"><span class=\"icon\">&#x2733;</span> FASE SOLARE</div>"
    "<canvas id=\"solar-chart\" width=\"440\" height=\"140\""
    " style=\"height:140px\"></canvas>"
    "</div>"
    "</div>"
    "</div>"
    "</div>"
    ""
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
    "<div class=\"row\"><span class=\"label\">Scena</span>"
    "<select class=\"fin\" id=\"led-scene\" onchange=\"sendScene()\">"
    "<option value=\"off\">&#x270B; Manuale</option>"
    "<option value=\"daylight\">&#x2600;&#xFE0F; Daylight</option>"
    "<option value=\"sunrise\">&#x1F305; Sunrise</option>"
    "<option value=\"sunset\">&#x1F307; Sunset</option>"
    "<option value=\"moonlight\">&#x1F319; Moonlight</option>"
    "<option value=\"cloudy\">&#x2601;&#xFE0F; Cloudy</option>"
    "<option value=\"storm\">&#x26A1; Storm</option>"
    "<option value=\"full_day_cycle\">&#x1F504; Full Day</option>"
    "</select></div>"
    "<div class=\"led-preview\" id=\"led-preview\"></div>"
    "</div></div></div>"
    "<!-- Scene Settings -->"
    "<div class=\"ccard\" id=\"scfg-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x2699;&#xFE0F; Impostazioni Scena</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"sect\">&#x2600;&#xFE0F; Transizioni</div>"
    "<div class=\"row\"><span class=\"label\">Sunrise (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-sr\" min=\"1\" max=\"120\""
    " style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Sunset (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-ss\" min=\"1\" max=\"120\""
    " style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Full Day trans. (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-fd\" min=\"1\" max=\"120\""
    " style=\"max-width:80px\"></div>"
    "<div class=\"row\"><span class=\"label\">Full Day max lum. %%</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-fdmax\" min=\"1\" max=\"100\""
    " style=\"max-width:80px\"></div>"
    "<div class=\"sect\">&#x1F3A8; Colore</div>"
    "<div class=\"row\"><span class=\"label\">Daylight (K)</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-colk\" min=\"6500\" max=\"20000\""
    " step=\"500\" style=\"max-width:100px\"></div>"
    "<div class=\"sect\">&#x1F319; Moonlight</div>"
    "<div class=\"row\"><span class=\"label\">Fase lunare</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"sc-lunar\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"sect\">&#x2615; Siesta (anti-alghe)</div>"
    "<div class=\"row\"><span class=\"label\">Abilitata</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"sc-sien\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Inizio (HH:MM)</span>"
    "<input type=\"time\" class=\"fin\" id=\"sc-sist\""
    " style=\"max-width:120px\"></div>"
    "<div class=\"row\"><span class=\"label\">Fine (HH:MM)</span>"
    "<input type=\"time\" class=\"fin\" id=\"sc-sied\""
    " style=\"max-width:120px\"></div>"
    "<div class=\"row\"><span class=\"label\">Intensit&#xE0; %%</span>"
    "<input type=\"number\" class=\"fin\" id=\"sc-sipct\" min=\"0\" max=\"100\""
    " style=\"max-width:80px\"></div>"
    "<button class=\"btn\" onclick=\"saveSceneCfg()\">Salva Impostazioni</button>"
    "</div></div></div>"
    "<!-- Geolocation -->"
    "<div class=\"ccard\" id=\"geo-card\">"
    "<div class=\"ccard-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F30D; Geolocalizzazione</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"ccard-body\"><div class=\"ccard-inner\">"
    "<div class=\"row\"><span class=\"label\">Latitudine</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-lat\""
    " step=\"0.0001\" min=\"-90\" max=\"90\" style=\"max-width:130px\"></div>"
    "<div class=\"row\"><span class=\"label\">Longitudine</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-lng\""
    " step=\"0.0001\" min=\"-180\" max=\"180\" style=\"max-width:130px\"></div>"
    "<div class=\"row\"><span class=\"label\">UTC Offset (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-utc\""
    " step=\"30\" min=\"-720\" max=\"840\" style=\"max-width:100px\"></div>"
    "<div class=\"row\"><span class=\"label\">Alba</span>"
    "<span class=\"value\" id=\"geo-sunrise\">--:--</span></div>"
    "<div class=\"row\"><span class=\"label\">Tramonto</span>"
    "<span class=\"value\" id=\"geo-sunset\">--:--</span></div>"
    "<button class=\"btn\" onclick=\"sendGeo()\">Salva Posizione</button>"
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
    "  if(n===0){loadDash()}"
    "  if(n===1){loadLeds();loadScene();loadGeo()}"
    "  if(n===2){loadTg()}"
    "  if(n===3){loadSys();loadDdns();loadOtaStatus();loadHeater();loadRelays()}}"
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
    "  $('led-scene').value='off';"
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
    "/* ── Scene ── */"
    "function sendScene(){"
    "  var s=$('led-scene').value;"
    "  fetch('/api/scenes',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({scene:s})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Scena impostata',1)}).catch(function(){"
    "    toast('Errore scena',0)})}"
    "function loadScene(){"
    "  fetch('/api/scenes').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('led-scene').value=d.active_scene;"
    "    $('sc-sr').value=d.sunrise_duration_min;"
    "    $('sc-ss').value=d.sunset_duration_min;"
    "    $('sc-fd').value=d.transition_duration_min;"
    "    $('sc-fdmax').value=d.fullday_max_brightness_pct;"
    "    $('sc-colk').value=d.color_temp_kelvin;"
    "    $('sc-lunar').checked=d.lunar_moonlight;"
    "    $('sc-sien').checked=d.siesta_enabled;"
    "    var sh=Math.floor(d.siesta_start_min/60);"
    "    var sm=d.siesta_start_min%%60;"
    "    $('sc-sist').value=(sh<10?'0':'')+sh+':'+(sm<10?'0':'')+sm;"
    "    var eh=Math.floor(d.siesta_end_min/60);"
    "    var em=d.siesta_end_min%%60;"
    "    $('sc-sied').value=(eh<10?'0':'')+eh+':'+(em<10?'0':'')+em;"
    "    $('sc-sipct').value=d.siesta_intensity_pct})}"
    "function saveSceneCfg(){"
    "  var st=$('sc-sist').value.split(':');"
    "  var ed=$('sc-sied').value.split(':');"
    "  var data={"
    "    sunrise_duration_min:parseInt($('sc-sr').value),"
    "    sunset_duration_min:parseInt($('sc-ss').value),"
    "    transition_duration_min:parseInt($('sc-fd').value),"
    "    fullday_max_brightness_pct:parseInt($('sc-fdmax').value),"
    "    color_temp_kelvin:parseInt($('sc-colk').value),"
    "    lunar_moonlight:$('sc-lunar').checked,"
    "    siesta_enabled:$('sc-sien').checked,"
    "    siesta_start_min:parseInt(st[0])*60+parseInt(st[1]),"
    "    siesta_end_min:parseInt(ed[0])*60+parseInt(ed[1]),"
    "    siesta_intensity_pct:parseInt($('sc-sipct').value)};"
    "  fetch('/api/scenes',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('Impostazioni salvate',1)}).catch(function(){"
    "    toast('Errore salvataggio',0)})}"
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
    "  loadDashGeo();loadDashHeater()}"
    "function loadDashLeds(){"
    "  fetch('/api/leds').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    fetch('/api/scenes').then(function(r){return r.json()})"
    "    .then(function(sc){"
    "      var name=sc.active_scene==='off'?(d.on?'Manuale':'Off'):sc.active_scene;"
    "      $('scene-name').textContent=name.charAt(0).toUpperCase()+name.slice(1);"
    "      $('scene-dot').style.background=d.on?'#38bdf8':'#475569';"
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
    "/* ── Sunlight / Geo for dashboard ── */"
    "var _geo=null;"
    "function loadDashGeo(){"
    "  fetch('/api/geolocation').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    _geo=d;"
    "    if(!d.sunrise||!d.sunset){$('sun-section').style.display='none';return}"
    "    $('sun-section').style.display='';"
    "    var srp=d.sunrise.split(':'),ssp=d.sunset.split(':');"
    "    var srm=parseInt(srp[0])*60+parseInt(srp[1]);"
    "    var ssm=parseInt(ssp[0])*60+parseInt(ssp[1]);"
    "    var now=new Date();"
    "    var nowm=now.getHours()*60+now.getMinutes();"
    "    var phase='Notte';"
    "    if(nowm>=srm&&nowm<srm+30)phase='Alba';"
    "    else if(nowm>=srm+30&&nowm<ssm-30)phase='Giorno';"
    "    else if(nowm>=ssm-30&&nowm<ssm)phase='Tramonto';"
    "    $('sun-label').textContent='SUNLIGHT: '+phase+"
    "      ' ('+d.sunrise+' - '+d.sunset+')';"
    "    var pct=Math.max(0,Math.min(100,((nowm-srm+60)/(ssm-srm+120))*100));"
    "    $('sun-dot').style.left=pct+'%%';"
    "    drawSolar(srm,ssm,nowm)})}"
    "/* ── Solar phase chart ── */"
    "function drawSolar(srm,ssm,nowm){"
    "  var cv=$('solar-chart');if(!cv)return;"
    "  var dpr=window.devicePixelRatio||1;"
    "  cv.width=cv.clientWidth*dpr;cv.height=cv.clientHeight*dpr;"
    "  var ctx=cv.getContext('2d');ctx.scale(dpr,dpr);"
    "  var W=cv.clientWidth,H=cv.clientHeight;"
    "  var pad={t:12,r:10,b:22,l:10};"
    "  var cw=W-pad.l-pad.r,ch=H-pad.t-pad.b;"
    "  ctx.clearRect(0,0,W,H);"
    "  /* Draw curve */"
    "  ctx.beginPath();ctx.strokeStyle='#d97706';ctx.lineWidth=2.5;"
    "  ctx.lineJoin='round';"
    "  for(var m=0;m<=1440;m+=5){"
    "    var x=pad.l+cw*(m/1440);"
    "    var y;"
    "    if(m<srm||m>ssm){y=pad.t+ch}"
    "    else{"
    "      var t=(m-srm)/(ssm-srm);"
    "      y=pad.t+ch-ch*Math.sin(t*Math.PI)*0.85}"
    "    if(m===0)ctx.moveTo(x,y);else ctx.lineTo(x,y)}"
    "  ctx.stroke();"
    "  /* Fill under curve */"
    "  ctx.lineTo(pad.l+cw,pad.t+ch);"
    "  ctx.lineTo(pad.l,pad.t+ch);ctx.closePath();"
    "  var grd=ctx.createLinearGradient(0,pad.t,0,pad.t+ch);"
    "  grd.addColorStop(0,'rgba(217,119,6,0.3)');"
    "  grd.addColorStop(1,'rgba(217,119,6,0.02)');"
    "  ctx.fillStyle=grd;ctx.fill();"
    "  /* Current time marker */"
    "  if(nowm>=0&&nowm<=1440){"
    "    var nx=pad.l+cw*(nowm/1440);"
    "    var ny;"
    "    if(nowm<srm||nowm>ssm){ny=pad.t+ch}"
    "    else{"
    "      var nt=(nowm-srm)/(ssm-srm);"
    "      ny=pad.t+ch-ch*Math.sin(nt*Math.PI)*0.85}"
    "    ctx.beginPath();ctx.arc(nx,ny,5,0,Math.PI*2);"
    "    ctx.fillStyle='#fbbf24';ctx.fill();"
    "    ctx.strokeStyle='#fff';ctx.lineWidth=2;ctx.stroke()}"
    "  /* X labels */"
    "  ctx.fillStyle='#64748b';ctx.font='11px sans-serif';"
    "  ctx.textAlign='center';ctx.textBaseline='top';"
    "  var labels=['00:00','06:00','12:00','18:00','24:00'];"
    "  for(var i=0;i<labels.length;i++){"
    "    var lx=pad.l+cw*(i/4);"
    "    ctx.fillText(labels[i],lx,pad.t+ch+4)}}"
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
    "/* ── Geolocation (LED Strip tab) ── */"
    "function loadGeo(){"
    "  fetch('/api/geolocation').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('geo-lat').value=d.latitude;"
    "    $('geo-lng').value=d.longitude;"
    "    $('geo-utc').value=d.utc_offset_min;"
    "    $('geo-sunrise').textContent=d.sunrise||'--:--';"
    "    $('geo-sunset').textContent=d.sunset||'--:--'})}"
    "function sendGeo(){"
    "  var lat=parseFloat($('geo-lat').value);"
    "  var lng=parseFloat($('geo-lng').value);"
    "  var utc=parseInt($('geo-utc').value);"
    "  fetch('/api/geolocation',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({latitude:lat,longitude:lng,utc_offset_min:utc})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    $('geo-sunrise').textContent=d.sunrise||'--:--';"
    "    $('geo-sunset').textContent=d.sunset||'--:--';"
    "    toast('Posizione salvata',1)"
    "  }).catch(function(){toast('Errore salvataggio',0)})}"
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
    "    daily_summary_hour:parseInt($('tg-sumhr').value)};"
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
    "      var row=document.createElement('div');"
    "      row.className='row';"
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
    "      var sl=document.createElement('span');"
    "      sl.className='slider';"
    "      tg.appendChild(inp);tg.appendChild(sl);"
    "      row.appendChild(lbl);row.appendChild(tg);"
    "      c.appendChild(row)})})}"
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
    "/* ── Init ── */"
    "loadDash();"
    "setInterval(function(){loadTemp();loadDashLeds();loadDashRelays();"
    "  loadDashHeater()},2000);"
    "setInterval(function(){loadHistory();loadDashGeo()},60000);"
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
        "\"scene\":\"%s\","
        "\"temp_c\":%.1f,"
        "\"free_heap\":%" PRIu32 ","
        "\"min_free_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRId64 "}",
        healthy ? "true" : "false",
        wifi_ok ? "true" : "false",
        temp_ok ? "true" : "false",
        led_ok  ? "true" : "false",
        led_scenes_get_name(led_scenes_get()),
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

    /* Stop any active scene when manual control is used */
    led_scenes_stop();

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

/* ── Scene status endpoint (/api/scenes  GET) ────────────────────── */

static esp_err_t api_scenes_get_handler(httpd_req_t *req)
{
    const char *active = led_scenes_get_name(led_scenes_get());
    led_scene_config_t cfg = led_scenes_get_config();

    char buf[JSON_SCENES_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"active_scene\":\"%s\","
        "\"scenes\":[\"off\",\"daylight\",\"sunrise\",\"sunset\","
        "\"moonlight\",\"cloudy\",\"storm\",\"full_day_cycle\"],"
        "\"sunrise_duration_min\":%d,"
        "\"sunset_duration_min\":%d,"
        "\"transition_duration_min\":%d,"
        "\"siesta_enabled\":%s,"
        "\"siesta_start_min\":%d,"
        "\"siesta_end_min\":%d,"
        "\"siesta_intensity_pct\":%d,"
        "\"color_temp_kelvin\":%d,"
        "\"lunar_moonlight\":%s,"
        "\"fullday_max_brightness_pct\":%d}",
        active,
        cfg.sunrise_duration_min,
        cfg.sunset_duration_min,
        cfg.transition_duration_min,
        cfg.siesta_enabled ? "true" : "false",
        cfg.siesta_start_min,
        cfg.siesta_end_min,
        cfg.siesta_intensity_pct,
        cfg.color_temp_kelvin,
        cfg.lunar_moonlight ? "true" : "false",
        cfg.fullday_max_brightness_pct);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Scene control endpoint (/api/scenes  POST) ──────────────────── */

static esp_err_t api_scenes_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_SCENE_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Scene POST body: %s", buf);

    /* Set active scene if provided */
    char scene_name[32];
    if (json_get_str(buf, "\"scene\"", scene_name, sizeof(scene_name)) == 0) {
        led_scene_t scene = led_scenes_from_name(scene_name);
        led_scenes_set(scene);
    }

    /* Update scene configuration if any config keys are present */
    led_scene_config_t cfg = led_scenes_get_config();
    bool cfg_changed = false;

    int val;
    val = json_get_int(buf, "\"sunrise_duration_min\"");
    if (val >= 0) { cfg.sunrise_duration_min = (uint16_t)val; cfg_changed = true; }

    val = json_get_int(buf, "\"sunset_duration_min\"");
    if (val >= 0) { cfg.sunset_duration_min = (uint16_t)val; cfg_changed = true; }

    val = json_get_int(buf, "\"transition_duration_min\"");
    if (val >= 0) { cfg.transition_duration_min = (uint16_t)val; cfg_changed = true; }

    val = json_get_bool(buf, "\"siesta_enabled\"");
    if (val >= 0) { cfg.siesta_enabled = (val == 1); cfg_changed = true; }

    val = json_get_int(buf, "\"siesta_start_min\"");
    if (val >= 0) { cfg.siesta_start_min = (uint16_t)val; cfg_changed = true; }

    val = json_get_int(buf, "\"siesta_end_min\"");
    if (val >= 0) { cfg.siesta_end_min = (uint16_t)val; cfg_changed = true; }

    val = json_get_int(buf, "\"siesta_intensity_pct\"");
    if (val >= 0) { cfg.siesta_intensity_pct = (uint8_t)val; cfg_changed = true; }

    val = json_get_int(buf, "\"color_temp_kelvin\"");
    if (val >= 0) { cfg.color_temp_kelvin = (uint16_t)val; cfg_changed = true; }

    val = json_get_bool(buf, "\"lunar_moonlight\"");
    if (val >= 0) { cfg.lunar_moonlight = (val == 1); cfg_changed = true; }

    val = json_get_int(buf, "\"fullday_max_brightness_pct\"");
    if (val >= 0) { cfg.fullday_max_brightness_pct = (uint8_t)val; cfg_changed = true; }

    if (cfg_changed) {
        led_scenes_set_config(&cfg);
    }

    return api_scenes_get_handler(req);
}

/* ── Temperature GET endpoint (/api/temperature  GET) ────────────── */

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

/* ── Geolocation GET endpoint (/api/geolocation  GET) ────────────── */

static esp_err_t api_geolocation_get_handler(httpd_req_t *req)
{
    geolocation_config_t cfg = geolocation_get();

    /* Also compute today's sunrise/sunset for display */
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    int year  = ti.tm_year + 1900;
    int month = ti.tm_mon + 1;
    int day   = ti.tm_mday;

    /* If time not set, use a default date */
    if (ti.tm_year < (2024 - 1900)) {
        year = 2026; month = 6; day = 21;
    }

    sun_times_t st = sun_position_calc(cfg.latitude, cfg.longitude,
                                       cfg.utc_offset_min,
                                       year, month, day);

    char buf[JSON_GEO_BUF_SIZE];
    int len;
    if (st.valid) {
        len = snprintf(buf, sizeof(buf),
            "{\"latitude\":%.4f,"
            "\"longitude\":%.4f,"
            "\"utc_offset_min\":%d,"
            "\"sunrise\":\"%02d:%02d\","
            "\"sunset\":\"%02d:%02d\"}",
            cfg.latitude, cfg.longitude, cfg.utc_offset_min,
            st.sunrise_min / 60, st.sunrise_min % 60,
            st.sunset_min / 60, st.sunset_min % 60);
    } else {
        len = snprintf(buf, sizeof(buf),
            "{\"latitude\":%.4f,"
            "\"longitude\":%.4f,"
            "\"utc_offset_min\":%d,"
            "\"sunrise\":null,"
            "\"sunset\":null}",
            cfg.latitude, cfg.longitude, cfg.utc_offset_min);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Geolocation POST endpoint (/api/geolocation  POST) ─────────── */

static esp_err_t api_geolocation_post_handler(httpd_req_t *req)
{
    char buf[POST_BODY_GEO_SIZE];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Geolocation POST body: %s", buf);

    geolocation_config_t cfg = geolocation_get();

    double lat, lng, utc_off;
    if (json_get_double(buf, "\"latitude\"", &lat) == 0) {
        cfg.latitude = lat;
    }
    if (json_get_double(buf, "\"longitude\"", &lng) == 0) {
        cfg.longitude = lng;
    }
    if (json_get_double(buf, "\"utc_offset_min\"", &utc_off) == 0) {
        cfg.utc_offset_min = (int)utc_off;
    }

    geolocation_set(&cfg);

    /* Update system timezone offset for localtime_r */
    char tz[16];
    int abs_off = cfg.utc_offset_min < 0 ? -cfg.utc_offset_min : cfg.utc_offset_min;
    /* POSIX TZ sign is inverted: +60 min offset → UTC-1 */
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d",
             cfg.utc_offset_min >= 0 ? '-' : '+',
             abs_off / 60, abs_off % 60);
    setenv("TZ", tz, 1);
    tzset();

    return api_geolocation_get_handler(req);
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
        n = snprintf(chunk, sizeof(chunk),
            "%s{\"index\":%d,\"on\":%s,\"name\":\"%s\","
            "\"schedule\":{\"enabled\":%s,\"on_min\":%d,\"off_min\":%d}}",
            i > 0 ? "," : "",
            i,
            relays[i].on ? "true" : "false",
            escaped_name,
            relays[i].schedule.enabled ? "true" : "false",
            relays[i].schedule.on_min,
            relays[i].schedule.off_min);
        httpd_resp_send_chunk(req, chunk, n);
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
        /* Read current schedule, then update provided fields */
        relay_state_t all[RELAY_COUNT];
        relay_controller_get_all(all);
        relay_schedule_t sched = all[idx].schedule;

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

        relay_controller_set_schedule(idx, &sched);
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

/* ── URI registrations ───────────────────────────────────────────── */

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

static const httpd_uri_t uri_api_scenes_get = {
    .uri      = "/api/scenes",
    .method   = HTTP_GET,
    .handler  = api_scenes_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_scenes_post = {
    .uri      = "/api/scenes",
    .method   = HTTP_POST,
    .handler  = api_scenes_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_geo_get = {
    .uri      = "/api/geolocation",
    .method   = HTTP_GET,
    .handler  = api_geolocation_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_geo_post = {
    .uri      = "/api/geolocation",
    .method   = HTTP_POST,
    .handler  = api_geolocation_post_handler,
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

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

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

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_api_status);
    httpd_register_uri_handler(s_server, &uri_api_health);
    httpd_register_uri_handler(s_server, &uri_api_leds_get);
    httpd_register_uri_handler(s_server, &uri_api_leds_post);
    httpd_register_uri_handler(s_server, &uri_api_scenes_get);
    httpd_register_uri_handler(s_server, &uri_api_scenes_post);
    httpd_register_uri_handler(s_server, &uri_api_geo_get);
    httpd_register_uri_handler(s_server, &uri_api_geo_post);
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

    ESP_LOGI(TAG, "HTTP server started – open http://<device-ip>/ in a browser");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
