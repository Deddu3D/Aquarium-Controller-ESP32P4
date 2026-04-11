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

static const char *TAG = "web_srv";

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
 * @brief Escape a string for safe inclusion in HTML output.
 *
 * Prevents XSS injection by replacing &, <, >, ", ' with HTML entities.
 */
static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < dst_size; i++) {
        switch (src[i]) {
        case '&':  j += snprintf(dst + j, dst_size - j, "&amp;");   break;
        case '<':  j += snprintf(dst + j, dst_size - j, "&lt;");    break;
        case '>':  j += snprintf(dst + j, dst_size - j, "&gt;");    break;
        case '"':  j += snprintf(dst + j, dst_size - j, "&quot;");  break;
        case '\'': j += snprintf(dst + j, dst_size - j, "&#x27;"); break;
        default:   dst[j++] = src[i]; break;
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
 * with mobile-optimized UI and temperature chart; 40 KiB gives
 * comfortable margin.
 */
#define HTML_BUF_SIZE 53248

static const char STATUS_HTML_TEMPLATE[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,"
    "viewport-fit=cover\">"
    "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
    "<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black-translucent\">"
    "<meta name=\"theme-color\" content=\"#0f172a\">"
    "<title>Aquarium Controller</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}"
    "html{font-size:16px}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#0f172a;color:#e2e8f0;min-height:100vh;min-height:100dvh;"
    "padding:env(safe-area-inset-top) env(safe-area-inset-right)"
    " env(safe-area-inset-bottom) env(safe-area-inset-left)}"
    ".wrap{max-width:480px;margin:0 auto;padding:.75rem .75rem 2rem}"
    "h1{font-size:1.3rem;text-align:center;padding:1rem 0 .75rem;color:#38bdf8}"
    /* Card styles */
    ".card{background:#1e293b;border-radius:14px;margin-bottom:.75rem;"
    "box-shadow:0 4px 20px rgba(0,0,0,.35);overflow:hidden}"
    ".card-hdr{display:flex;align-items:center;justify-content:space-between;"
    "padding:.9rem 1rem;cursor:pointer;-webkit-user-select:none;user-select:none}"
    ".card-hdr h2{font-size:1rem;color:#38bdf8;margin:0}"
    ".card-hdr .arr{color:#64748b;font-size:.7rem;transition:transform .25s}"
    ".card.open .arr{transform:rotate(180deg)}"
    ".card-body{max-height:0;overflow:hidden;transition:max-height .3s ease}"
    ".card.open .card-body{max-height:2000px}"
    ".card-inner{padding:0 1rem 1rem}"
    /* Rows */
    ".row{display:flex;justify-content:space-between;align-items:center;"
    "padding:.65rem 0;border-bottom:1px solid #334155;gap:.5rem}"
    ".row:last-child{border-bottom:none}"
    ".label{color:#94a3b8;font-size:.9rem;flex-shrink:0}"
    ".value{font-weight:600;font-size:.95rem;text-align:right}"
    ".ok{color:#4ade80}.err{color:#f87171}"
    /* Section dividers inside cards */
    ".sect{font-size:.9rem;color:#38bdf8;padding:.7rem 0 .3rem;"
    "border-bottom:1px solid #334155;margin-top:.3rem}"
    /* Buttons */
    ".btn{display:block;width:100%%;padding:.75rem;margin-top:.75rem;"
    "background:#38bdf8;color:#0f172a;border:none;border-radius:10px;"
    "font-size:.95rem;font-weight:600;cursor:pointer;text-align:center;"
    "touch-action:manipulation;transition:background .15s}"
    ".btn:active{background:#7dd3fc}"
    ".btn-sm{background:#334155;color:#e2e8f0;font-size:.85rem;padding:.6rem}"
    ".btn-sm:active{background:#475569}"
    /* Toggle switch */
    ".toggle{position:relative;width:52px;height:30px;flex-shrink:0}"
    ".toggle input{opacity:0;width:0;height:0}"
    ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;"
    "background:#475569;border-radius:30px;transition:.25s}"
    ".slider:before{content:'';position:absolute;height:24px;width:24px;"
    "left:3px;bottom:3px;background:#fff;border-radius:50%%;transition:.25s}"
    ".toggle input:checked+.slider{background:#4ade80}"
    ".toggle input:checked+.slider:before{transform:translateX(22px)}"
    /* Form inputs */
    ".fin{width:100%%;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:8px;padding:.6rem .75rem;font-size:.95rem;"
    "text-align:right;appearance:none;-webkit-appearance:none}"
    ".fin:focus{outline:none;border-color:#38bdf8}"
    ".fin-wide{text-align:left}"
    /* Range slider */
    "input[type=range]{width:100%%;height:36px;accent-color:#38bdf8;"
    "touch-action:pan-x}"
    /* Color picker */
    "input[type=color]{width:52px;height:36px;border:none;border-radius:8px;"
    "cursor:pointer;background:none;padding:0}"
    /* Select dropdown */
    "select.fin{padding-right:1.5rem}"
    /* LED preview */
    ".led-preview{width:100%%;height:28px;border-radius:8px;"
    "margin-top:.5rem;border:1px solid #334155}"
    /* Tab bar */
    ".tab-bar{display:flex;background:#1e293b;border-radius:12px;"
    "margin-bottom:.75rem;overflow:hidden}"
    ".tab{flex:1;padding:.65rem .3rem;background:none;border:none;"
    "color:#64748b;font-size:.75rem;font-weight:600;cursor:pointer;"
    "text-align:center;transition:color .2s,background .2s;"
    "display:flex;flex-direction:column;align-items:center;gap:.2rem}"
    ".tab .tab-icon{font-size:1.2rem}"
    ".tab.active{color:#38bdf8;background:#334155}"
    ".panel{display:none}"
    ".panel.active{display:block}"
    /* Toast notification */
    ".toast{position:fixed;bottom:1.5rem;left:50%%;transform:translateX(-50%%);"
    "background:#334155;color:#e2e8f0;padding:.7rem 1.2rem;border-radius:10px;"
    "font-size:.9rem;box-shadow:0 4px 16px rgba(0,0,0,.5);z-index:99;"
    "opacity:0;transition:opacity .3s;pointer-events:none}"
    ".toast.show{opacity:1}"
    ".toast.ok{border-left:3px solid #4ade80}"
    ".toast.fail{border-left:3px solid #f87171}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"wrap\">"
    "<h1>&#x1F41F; Aquarium Controller</h1>"
    /* ── Tab bar ── */
    "<div class=\"tab-bar\">"
    "<button class=\"tab active\" onclick=\"switchTab(0)\">"
    "<span class=\"tab-icon\">&#x1F3E0;</span>Dashboard</button>"
    "<button class=\"tab\" onclick=\"switchTab(1)\">"
    "<span class=\"tab-icon\">&#x1F4A1;</span>Luci</button>"
    "<button class=\"tab\" onclick=\"switchTab(2)\">"
    "<span class=\"tab-icon\">&#x1F50C;</span>Rel&#xE8;</button>"
    "<button class=\"tab\" onclick=\"switchTab(3)\">"
    "<span class=\"tab-icon\">&#x1F4F1;</span>Telegram</button>"
    "<button class=\"tab\" onclick=\"switchTab(4)\">"
    "<span class=\"tab-icon\">&#x1F310;</span>Network</button>"
    "</div>"
    /* ════════════════════════════════════════════════════════════════
     *  Panel 0 – Dashboard (Riepilogo & Azioni Rapide)
     * ════════════════════════════════════════════════════════════════ */
    "<div class=\"panel active\" id=\"p0\">"
    /* WiFi status card */
    "<div class=\"card open\" id=\"wifi-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4F6; Status</h2><span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Connection</span>"
    "<span class=\"value %s\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">IP</span>"
    "<span class=\"value\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">SSID</span>"
    "<span class=\"value\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">RSSI</span>"
    "<span class=\"value\">%d dBm</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span>"
    "<span class=\"value\">%" PRIu32 " B</span></div>"
    "<div class=\"row\"><span class=\"label\">Uptime</span>"
    "<span class=\"value\">%s</span></div>"
    "</div></div></div>"
    /* Temperature card */
    "<div class=\"card open\" id=\"temp-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F321;&#xFE0F; Temperature</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Water</span>"
    "<span class=\"value\" id=\"temp-val\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">Min</span>"
    "<span class=\"value\" id=\"temp-min\">--</span></div>"
    "<div class=\"row\"><span class=\"label\">Max</span>"
    "<span class=\"value\" id=\"temp-max\">--</span></div>"
    "<canvas id=\"temp-chart\" width=\"440\" height=\"200\""
    " style=\"width:100%%;height:200px;margin-top:.5rem;"
    "border-radius:8px;background:#0f172a\"></canvas>"
    "</div></div></div>"
    /* Quick Actions card */
    "<div class=\"card open\" id=\"qa-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x26A1; Quick Actions</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">LED</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"q-led\""
    " onchange=\"toggleQuickLed()\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Scene</span>"
    "<select class=\"fin\" id=\"q-scene\" onchange=\"sendQScene()\">"
    "<option value=\"off\">&#x270B; Manual</option>"
    "<option value=\"daylight\">&#x2600;&#xFE0F; Daylight</option>"
    "<option value=\"sunrise\">&#x1F305; Sunrise</option>"
    "<option value=\"sunset\">&#x1F307; Sunset</option>"
    "<option value=\"moonlight\">&#x1F319; Moonlight</option>"
    "<option value=\"cloudy\">&#x2601;&#xFE0F; Cloudy</option>"
    "<option value=\"storm\">&#x26A1; Storm</option>"
    "<option value=\"full_day_cycle\">&#x1F504; Full Day</option>"
    "</select></div>"
    "<div class=\"sect\">&#x1F50C; Relays</div>"
    "<div id=\"q-relays\"></div>"
    "</div></div></div>"
    "</div>"
    /* ════════════════════════════════════════════════════════════════
     *  Panel 1 – Luci (Full Light Control)
     * ════════════════════════════════════════════════════════════════ */
    "<div class=\"panel\" id=\"p1\">"
    /* LED control card */
    "<div class=\"card open\" id=\"led-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4A1; LED Control</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Power</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"led-on\""
    " onchange=\"sendLed()\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Brightness</span>"
    "<span class=\"value\" id=\"br-val\">128</span></div>"
    "<div class=\"row\"><input type=\"range\" id=\"led-br\" min=\"0\""
    " max=\"255\" value=\"128\" oninput=\"document.getElementById("
    "'br-val').textContent=this.value\" onchange=\"sendLed()\"></div>"
    "<div class=\"row\"><span class=\"label\">Color</span>"
    "<input type=\"color\" id=\"led-color\" value=\"#ffffff\""
    " onchange=\"sendLed()\"></div>"
    "<div class=\"row\"><span class=\"label\">Scene</span>"
    "<select class=\"fin\" id=\"led-scene\" onchange=\"sendScene()\">"
    "<option value=\"off\">&#x270B; Manual</option>"
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
    /* Geolocation card */
    "<div class=\"card\" id=\"geo-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F30D; Geolocation</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Latitude</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-lat\""
    " step=\"0.0001\" min=\"-90\" max=\"90\" style=\"max-width:130px\"></div>"
    "<div class=\"row\"><span class=\"label\">Longitude</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-lng\""
    " step=\"0.0001\" min=\"-180\" max=\"180\" style=\"max-width:130px\"></div>"
    "<div class=\"row\"><span class=\"label\">UTC Offset (min)</span>"
    "<input type=\"number\" class=\"fin\" id=\"geo-utc\""
    " step=\"30\" min=\"-720\" max=\"840\" style=\"max-width:100px\"></div>"
    "<div class=\"row\"><span class=\"label\">Sunrise</span>"
    "<span class=\"value\" id=\"geo-sunrise\">--:--</span></div>"
    "<div class=\"row\"><span class=\"label\">Sunset</span>"
    "<span class=\"value\" id=\"geo-sunset\">--:--</span></div>"
    "<button class=\"btn\" onclick=\"sendGeo()\">Save Location</button>"
    "</div></div></div>"
    "</div>"
    /* ════════════════════════════════════════════════════════════════
     *  Panel 2 – Relays
     * ════════════════════════════════════════════════════════════════ */
    "<div class=\"panel\" id=\"p2\">"
    "<div class=\"card open\" id=\"relay-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F50C; Relays</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div id=\"relay-rows\"></div>"
    "</div></div></div>"
    "</div>"
    /* ════════════════════════════════════════════════════════════════
     *  Panel 3 – Telegram
     * ════════════════════════════════════════════════════════════════ */
    "<div class=\"panel\" id=\"p3\">"
    "<div class=\"card open\" id=\"tg-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F4F1; Telegram</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Bot Token</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"password\" class=\"fin fin-wide\" id=\"tg-token\""
    " placeholder=\"Not configured\"></div>"
    "<div class=\"row\"><span class=\"label\">Chat ID</span>"
    "<input type=\"text\" class=\"fin\" id=\"tg-chatid\""
    " placeholder=\"-100123456\" style=\"max-width:140px\"></div>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<button class=\"btn btn-sm\" onclick=\"testTg()\">"
    "&#x1F4E8; Send Test Message</button>"
    /* Temperature alarms section */
    "<div class=\"sect\">&#x1F321;&#xFE0F; Temperature Alarms</div>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-talm\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">High &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-thigh\""
    " step=\"0.5\" min=\"-10\" max=\"50\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Low &#xB0;C</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-tlow\""
    " step=\"0.5\" min=\"-10\" max=\"50\" style=\"max-width:90px\"></div>"
    /* Water change section */
    "<div class=\"sect\">&#x1F4A7; Water Change</div>"
    "<div class=\"row\"><span class=\"label\">Reminder</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-wc\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Interval (days)</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-wcdays\""
    " min=\"1\" max=\"90\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Last change</span>"
    "<span class=\"value\" id=\"tg-wclast\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"resetWc()\">"
    "&#x2705; Record Water Change</button>"
    /* Fertilizer section */
    "<div class=\"sect\">&#x1F33F; Fertilizer</div>"
    "<div class=\"row\"><span class=\"label\">Reminder</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-fert\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Interval (days)</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-fertdays\""
    " min=\"1\" max=\"90\" style=\"max-width:90px\"></div>"
    "<div class=\"row\"><span class=\"label\">Last dose</span>"
    "<span class=\"value\" id=\"tg-fertlast\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"resetFert()\">"
    "&#x2705; Record Fertilizer Dose</button>"
    /* Daily summary section */
    "<div class=\"sect\">&#x1F4CA; Daily Summary</div>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-sum\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Send at hour</span>"
    "<input type=\"number\" class=\"fin\" id=\"tg-sumhr\""
    " min=\"0\" max=\"23\" style=\"max-width:90px\"></div>"
    "<button class=\"btn\" onclick=\"saveTg()\">Save Settings</button>"
    "</div></div></div>"
    "</div>"
    /* ════════════════════════════════════════════════════════════════
     *  Panel 4 – Network (DuckDNS)
     * ════════════════════════════════════════════════════════════════ */
    "<div class=\"panel\" id=\"p4\">"
    "<div class=\"card open\" id=\"ddns-card\">"
    "<div class=\"card-hdr\" onclick=\"tog(this)\">"
    "<h2>&#x1F310; DuckDNS</h2>"
    "<span class=\"arr\">&#x25BC;</span></div>"
    "<div class=\"card-body\"><div class=\"card-inner\">"
    "<div class=\"row\"><span class=\"label\">Domain</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"text\" class=\"fin fin-wide\" id=\"ddns-domain\""
    " placeholder=\"myaquarium\"></div>"
    "<div class=\"row\" style=\"padding-top:0\">"
    "<span class=\"label\" style=\"font-size:.8rem;color:#64748b\">"
    ".duckdns.org</span></div>"
    "<div class=\"row\"><span class=\"label\">Token</span></div>"
    "<div class=\"row\" style=\"border-bottom:none;padding-top:0\">"
    "<input type=\"password\" class=\"fin fin-wide\" id=\"ddns-token\""
    " placeholder=\"Not configured\"></div>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"ddns-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Last status</span>"
    "<span class=\"value\" id=\"ddns-status\">--</span></div>"
    "<button class=\"btn btn-sm\" onclick=\"testDdns()\">"
    "&#x1F504; Update Now</button>"
    "<button class=\"btn\" onclick=\"saveDdns()\">Save Settings</button>"
    "</div></div></div>"
    "</div>"
    /* ── end panels ── */
    "</div>"
    /* Toast element */
    "<div class=\"toast\" id=\"toast\"></div>"
    /* ════════════════════════════════════════════════════════════════
     *  Scripts
     * ════════════════════════════════════════════════════════════════ */
    "<script>"
    "function $(i){return document.getElementById(i)}"
    "function tog(hdr){hdr.parentElement.classList.toggle('open')}"
    "function toast(msg,ok){"
    "  var t=$('toast');t.textContent=msg;"
    "  t.className='toast '+(ok?'ok':'fail')+' show';"
    "  clearTimeout(t._t);t._t=setTimeout(function(){"
    "    t.className='toast'},2500)}"
    /* ── Tab switching ── */
    "var _tab=0;"
    "function switchTab(n){"
    "  var tabs=document.querySelectorAll('.tab');"
    "  for(var i=0;i<5;i++){"
    "    $('p'+i).classList.remove('active');"
    "    tabs[i].classList.remove('active')}"
    "  $('p'+n).classList.add('active');"
    "  tabs[n].classList.add('active');"
    "  _tab=n;"
    "  if(n===0){loadTemp();loadHistory();loadQScene();loadQRelays()}"
    "  if(n===1){loadLeds();loadScene();loadGeo()}"
    "  if(n===2){loadRelays()}"
    "  if(n===3){loadTg()}"
    "  if(n===4){loadDdns()}}"
    /* ── Color helpers ── */
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
    /* ── LED control (Luci tab) ── */
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
    "    $('q-led').checked=on;"
    "    toast('LED updated',1)}).catch(function(){"
    "    toast('LED error',0)})}"
    "function loadLeds(){"
    "  fetch('/api/leds').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('led-on').checked=d.on;"
    "    $('led-br').value=d.brightness;"
    "    $('br-val').textContent=d.brightness;"
    "    $('led-color').value=rgbToHex(d.r,d.g,d.b);"
    "    $('q-led').checked=d.on;"
    "    updatePreview()})}"
    /* ── Scene control (Luci tab) ── */
    "function sendScene(){"
    "  var s=$('led-scene').value;"
    "  fetch('/api/scenes',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({scene:s})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    $('q-scene').value=s;"
    "    toast('Scene set',1)}).catch(function(){"
    "    toast('Scene error',0)})}"
    "function loadScene(){"
    "  fetch('/api/scenes').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    $('led-scene').value=d.active_scene;"
    "    $('q-scene').value=d.active_scene})}"
    /* ── Quick Actions (Dashboard) ── */
    "function toggleQuickLed(){"
    "  var on=$('q-led').checked;"
    "  fetch('/api/leds',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({on:on})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    toast('LED '+(on?'ON':'OFF'),1)}).catch(function(){"
    "    toast('LED error',0)})}"
    "function sendQScene(){"
    "  var s=$('q-scene').value;"
    "  fetch('/api/scenes',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({scene:s})"
    "  }).then(function(r){return r.json()}).then(function(){"
    "    $('led-scene').value=s;"
    "    toast('Scene set',1)}).catch(function(){"
    "    toast('Scene error',0)})}"
    "function loadQScene(){"
    "  fetch('/api/scenes').then(function(r){return r.json()})"
    "  .then(function(d){$('q-scene').value=d.active_scene})}"
    "function loadQRelays(){"
    "  fetch('/api/relays').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var c=$('q-relays');c.innerHTML='';"
    "    d.relays.forEach(function(rl,i){"
    "      var row=document.createElement('div');"
    "      row.className='row';"
    "      var lbl=document.createElement('span');"
    "      lbl.className='label';lbl.textContent=rl.name;"
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
    "        .catch(function(){toast('Relay error',0)})};"
    "      var sl=document.createElement('span');"
    "      sl.className='slider';"
    "      tg.appendChild(inp);tg.appendChild(sl);"
    "      row.appendChild(lbl);row.appendChild(tg);"
    "      c.appendChild(row)})})"
    "  .catch(function(){})}"
    /* ── Geolocation (Luci tab) ── */
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
    "    toast('Location saved',1)"
    "  }).catch(function(){toast('Save failed',0)})}"
    /* ── Telegram (Telegram tab) ── */
    "function tgTs(id,ts){"
    "  var el=$(id);"
    "  if(ts>0){var d=Math.floor((Date.now()/1000-ts)/86400);"
    "    el.textContent=d+' days ago'}"
    "  else{el.textContent='Never'}}"
    "function loadTg(){"
    "  fetch('/api/telegram').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.bot_token_set){$('tg-token').placeholder="
    "      'Token configured \\u2713'}"
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
    "      $('tg-token').placeholder='Token configured \\u2713'}"
    "    toast('Settings saved',1)"
    "  }).catch(function(){toast('Save failed',0)})}"
    "function testTg(){"
    "  fetch('/api/telegram_test',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    toast(d.ok?'Test sent!':'Failed: '+(d.error||'unknown'),d.ok)"
    "  }).catch(function(){toast('Send error',0)})}"
    "function resetWc(){"
    "  fetch('/api/telegram_wc',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    tgTs('tg-wclast',d.last_water_change);"
    "    toast('Water change recorded',1)})}"
    "function resetFert(){"
    "  fetch('/api/telegram_fert',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    tgTs('tg-fertlast',d.last_fertilizer);"
    "    toast('Fertilizer recorded',1)})}"
    /* ── Relay control (Relay tab) ── */
    "function loadRelays(){"
    "  fetch('/api/relays').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var c=$('relay-rows');c.innerHTML='';"
    "    d.relays.forEach(function(rl,i){"
    "      var row=document.createElement('div');"
    "      row.className='row';"
    "      var lbl=document.createElement('span');"
    "      lbl.className='label';lbl.textContent=rl.name;"
    "      lbl.style.cursor='pointer';lbl.title='Click to rename';"
    "      lbl.onclick=function(){"
    "        var nn=prompt('Rename relay:',rl.name);"
    "        if(nn&&nn.length>0){"
    "          fetch('/api/relays',{method:'POST',"
    "            headers:{'Content-Type':'application/json'},"
    "            body:JSON.stringify({index:i,name:nn})"
    "          }).then(function(){loadRelays();toast('Renamed',1)})"
    "          .catch(function(){toast('Rename failed',0)})}};"
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
    "        .catch(function(){toast('Relay error',0)})};"
    "      var sl=document.createElement('span');"
    "      sl.className='slider';"
    "      tg.appendChild(inp);tg.appendChild(sl);"
    "      row.appendChild(lbl);row.appendChild(tg);"
    "      c.appendChild(row)})})"
    "  .catch(function(){})}"
    /* ── DuckDNS (Network tab) ── */
    "function loadDdns(){"
    "  fetch('/api/duckdns').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    if(d.domain){$('ddns-domain').value=d.domain}"
    "    if(d.token_set){$('ddns-token').placeholder="
    "      'Token configured \\u2713'}"
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
    "      $('ddns-token').placeholder='Token configured \\u2713'}"
    "    $('ddns-status').textContent=d.last_status||'--';"
    "    toast('DuckDNS settings saved',1)"
    "  }).catch(function(){toast('Save failed',0)})}"
    "function testDdns(){"
    "  $('ddns-status').textContent='updating\\u2026';"
    "  fetch('/api/duckdns_update',{method:'POST'})"
    "  .then(function(r){return r.json()}).then(function(d){"
    "    $('ddns-status').textContent=d.last_status||'--';"
    "    $('ddns-status').className='value '+(d.ok?'ok':'err');"
    "    toast(d.ok?'DuckDNS updated!':'Update failed',d.ok)"
    "  }).catch(function(){toast('Update error',0)})}"
    /* ── Temperature ── */
    "function loadTemp(){"
    "  fetch('/api/temperature').then(function(r){return r.json()})"
    "  .then(function(d){"
    "    var el=$('temp-val');"
    "    if(d.valid){el.textContent=d.temperature_c.toFixed(2)+' \\u00B0C';"
    "      el.className='value ok'}"
    "    else{el.textContent='No sensor';el.className='value err'}"
    "  }).catch(function(){var el=$('temp-val');"
    "    el.textContent='Error';el.className='value err'})}"
    /* ── Temperature chart ── */
    "function drawChart(samples){"
    "  var cv=$('temp-chart');if(!cv)return;"
    "  var dpr=window.devicePixelRatio||1;"
    "  cv.width=cv.clientWidth*dpr;cv.height=cv.clientHeight*dpr;"
    "  var ctx=cv.getContext('2d');ctx.scale(dpr,dpr);"
    "  var W=cv.clientWidth,H=cv.clientHeight;"
    "  var pad={t:20,r:10,b:30,l:46};"
    "  var cw=W-pad.l-pad.r,ch=H-pad.t-pad.b;"
    "  ctx.clearRect(0,0,W,H);"
    "  if(!samples||samples.length<2){"
    "    ctx.fillStyle='#64748b';ctx.font='13px sans-serif';"
    "    ctx.textAlign='center';"
    "    ctx.fillText('Waiting for data\\u2026',W/2,H/2);return}"
    "  var mn=1e9,mx=-1e9;"
    "  for(var i=0;i<samples.length;i++){"
    "    if(samples[i].c<mn)mn=samples[i].c;"
    "    if(samples[i].c>mx)mx=samples[i].c}"
    "  $('temp-min').textContent=mn.toFixed(1)+' \\u00B0C';"
    "  $('temp-min').className='value ok';"
    "  $('temp-max').textContent=mx.toFixed(1)+' \\u00B0C';"
    "  $('temp-max').className='value ok';"
    "  var margin=(mx-mn)*0.15;if(margin<0.3)margin=0.3;"
    "  var yMin=mn-margin,yMax=mx+margin;"
    /* Grid lines */
    "  ctx.strokeStyle='#1e293b';ctx.lineWidth=1;"
    "  var ngy=5;for(var i=0;i<=ngy;i++){"
    "    var y=pad.t+ch-ch*(i/ngy);"
    "    ctx.beginPath();ctx.moveTo(pad.l,y);"
    "    ctx.lineTo(pad.l+cw,y);ctx.stroke();"
    "    ctx.fillStyle='#64748b';ctx.font='11px sans-serif';"
    "    ctx.textAlign='right';ctx.textBaseline='middle';"
    "    ctx.fillText((yMin+(yMax-yMin)*(i/ngy)).toFixed(1),pad.l-4,y)}"
    /* X-axis labels (hours) */
    "  var t0=samples[0].t,t1=samples[samples.length-1].t;"
    "  var span=t1-t0;if(span<1)span=1;"
    "  ctx.fillStyle='#64748b';ctx.font='11px sans-serif';"
    "  ctx.textAlign='center';ctx.textBaseline='top';"
    "  var lx=-999;"
    "  for(var i=0;i<samples.length;i++){"
    "    var x=pad.l+cw*((samples[i].t-t0)/span);"
    "    var d=new Date(samples[i].t*1000);"
    "    if(d.getMinutes()===0&&(x-lx)>36){"
    "      ctx.beginPath();ctx.moveTo(x,pad.t);"
    "      ctx.lineTo(x,pad.t+ch);ctx.strokeStyle='#1e293b';"
    "      ctx.stroke();"
    "      var lbl=('0'+d.getHours()).slice(-2)+':00';"
    "      ctx.fillText(lbl,x,pad.t+ch+4);lx=x}}"
    /* Line */
    "  ctx.beginPath();ctx.strokeStyle='#38bdf8';ctx.lineWidth=2;"
    "  ctx.lineJoin='round';"
    "  for(var i=0;i<samples.length;i++){"
    "    var x=pad.l+cw*((samples[i].t-t0)/span);"
    "    var y=pad.t+ch-ch*((samples[i].c-yMin)/(yMax-yMin));"
    "    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y)}"
    "  ctx.stroke();"
    /* Gradient fill */
    "  var last=samples.length-1;"
    "  var lx2=pad.l+cw*((samples[last].t-t0)/span);"
    "  ctx.lineTo(lx2,pad.t+ch);ctx.lineTo(pad.l,pad.t+ch);ctx.closePath();"
    "  var grd=ctx.createLinearGradient(0,pad.t,0,pad.t+ch);"
    "  grd.addColorStop(0,'rgba(56,189,248,0.25)');"
    "  grd.addColorStop(1,'rgba(56,189,248,0.02)');"
    "  ctx.fillStyle=grd;ctx.fill()}"
    "function loadHistory(){"
    "  fetch('/api/temperature_history').then(function(r){return r.json()})"
    "  .then(function(d){drawChart(d.samples)})"
    "  .catch(function(){})}"
    /* ── Initial data load (Dashboard is default tab) ── */
    "loadTemp();setInterval(loadTemp,5000);"
    "loadHistory();setInterval(loadHistory,60000);"
    "loadQScene();loadQRelays();"
    "fetch('/api/leds').then(function(r){return r.json()})"
    ".then(function(d){$('q-led').checked=d.on});"
    "</script>"
    "</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    get_wifi_status(&ws);

    /* Uptime string */
    int64_t us   = esp_timer_get_time();
    int     secs = (int)(us / 1000000);
    int     h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char uptime[32];
    snprintf(uptime, sizeof(uptime), "%dh %dm %ds", h, m, s);

    /* HTML-escape the SSID to prevent XSS injection */
    char escaped_ssid[128];
    html_escape(ws.connected ? ws.ssid : "\xe2\x80\x94" /* — */, escaped_ssid, sizeof(escaped_ssid));

    /* Render HTML – heap-allocated to avoid httpd task stack overflow */
    char *buf = malloc(HTML_BUF_SIZE);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_FAIL;
    }

    int len = snprintf(buf, HTML_BUF_SIZE, STATUS_HTML_TEMPLATE,
                       ws.connected ? "ok" : "err",
                       ws.connected ? "Connected" : "Disconnected",
                       ws.connected ? ws.ip : "\xe2\x80\x94" /* — */,
                       escaped_ssid,
                       ws.connected ? ws.rssi : 0,
                       esp_get_free_heap_size(),
                       uptime);
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

    char buf[384];
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

/* ── LED status endpoint (/api/leds  GET) ─────────────────────────── */

static esp_err_t api_leds_get_handler(httpd_req_t *req)
{
    uint8_t r, g, b;
    led_controller_get_color(&r, &g, &b);

    char buf[256];
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
    char buf[256];
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

    /* Apply on/off */
    if (on_val == 1) {
        led_controller_on();
    } else if (on_val == 0) {
        led_controller_off();
    }

    /* Respond with updated state */
    return api_leds_get_handler(req);
}

/* ── Scene status endpoint (/api/scenes  GET) ────────────────────── */

static esp_err_t api_scenes_get_handler(httpd_req_t *req)
{
    const char *active = led_scenes_get_name(led_scenes_get());

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"active_scene\":\"%s\","
        "\"scenes\":[\"off\",\"daylight\",\"sunrise\",\"sunset\","
        "\"moonlight\",\"cloudy\",\"storm\",\"full_day_cycle\"]}",
        active);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ── Scene control endpoint (/api/scenes  POST) ──────────────────── */

static esp_err_t api_scenes_post_handler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Scene POST body: %s", buf);

    char scene_name[32];
    if (json_get_str(buf, "\"scene\"", scene_name, sizeof(scene_name)) == 0) {
        led_scene_t scene = led_scenes_from_name(scene_name);
        led_scenes_set(scene);
    }

    return api_scenes_get_handler(req);
}

/* ── Temperature GET endpoint (/api/temperature  GET) ────────────── */

static esp_err_t api_temperature_get_handler(httpd_req_t *req)
{
    float temp_c = 0.0f;
    bool valid = temperature_sensor_get(&temp_c);

    char buf[128];
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
    char chunk[64];
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

    char buf[384];
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
    char buf[256];
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
    char *buf = malloc(768);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int len = snprintf(buf, 768,
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
    char *buf = malloc(512);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, 511);
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

    char chunk[128];
    int n;

    n = snprintf(chunk, sizeof(chunk), "{\"count\":%d,\"relays\":[", RELAY_COUNT);
    httpd_resp_send_chunk(req, chunk, n);

    for (int i = 0; i < RELAY_COUNT; i++) {
        json_escape(relays[i].name, escaped_name, sizeof(escaped_name));
        n = snprintf(chunk, sizeof(chunk),
            "%s{\"index\":%d,\"on\":%s,\"name\":\"%s\"}",
            i > 0 ? "," : "",
            i,
            relays[i].on ? "true" : "false",
            escaped_name);
        httpd_resp_send_chunk(req, chunk, n);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Relay POST endpoint (/api/relays  POST) ─────────────────────── */

static esp_err_t api_relays_post_handler(httpd_req_t *req)
{
    char buf[256];
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
        relay_controller_set_name(idx, name);
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

    char buf[384];
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
    char buf[256];
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

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 8192;
    config.max_uri_handlers = 25;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_api_status);
    httpd_register_uri_handler(s_server, &uri_api_leds_get);
    httpd_register_uri_handler(s_server, &uri_api_leds_post);
    httpd_register_uri_handler(s_server, &uri_api_scenes_get);
    httpd_register_uri_handler(s_server, &uri_api_scenes_post);
    httpd_register_uri_handler(s_server, &uri_api_geo_get);
    httpd_register_uri_handler(s_server, &uri_api_geo_post);
    httpd_register_uri_handler(s_server, &uri_api_temp_get);
    httpd_register_uri_handler(s_server, &uri_api_temp_hist_get);
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
