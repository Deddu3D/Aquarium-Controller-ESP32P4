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
#include "telegram_notify.h"

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
 * with the geolocation + telegram cards; 20 KiB gives comfortable margin.
 */
#define HTML_BUF_SIZE 20480

static const char STATUS_HTML_TEMPLATE[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Aquarium Controller</title>"
    "<style>"
    "* { box-sizing: border-box; margin: 0; padding: 0; }"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
    "       background: #0f172a; color: #e2e8f0; min-height: 100vh;"
    "       display: flex; flex-direction: column; align-items: center; padding: 1rem; }"
    ".card { background: #1e293b; border-radius: 16px; padding: 2rem;"
    "        max-width: 420px; width: 90%%; box-shadow: 0 8px 32px rgba(0,0,0,.4);"
    "        margin-bottom: 1rem; }"
    "h1 { font-size: 1.4rem; text-align: center; margin-bottom: 1.2rem;"
    "     color: #38bdf8; }"
    "h2 { font-size: 1.1rem; text-align: center; margin-bottom: 1rem;"
    "     color: #38bdf8; }"
    ".row { display: flex; justify-content: space-between; align-items: center;"
    "       padding: .6rem 0; border-bottom: 1px solid #334155; }"
    ".row:last-child { border-bottom: none; }"
    ".label { color: #94a3b8; }"
    ".value { font-weight: 600; }"
    ".ok  { color: #4ade80; }"
    ".err { color: #f87171; }"
    ".refresh { display: block; margin: 1.2rem auto 0; padding: .5rem 1.5rem;"
    "           background: #38bdf8; color: #0f172a; border: none;"
    "           border-radius: 8px; font-size: 1rem; cursor: pointer; }"
    ".refresh:hover { background: #7dd3fc; }"
    /* LED control styles */
    ".toggle { position: relative; width: 50px; height: 26px; }"
    ".toggle input { opacity: 0; width: 0; height: 0; }"
    ".slider { position: absolute; cursor: pointer; top: 0; left: 0;"
    "          right: 0; bottom: 0; background: #475569; border-radius: 26px;"
    "          transition: .3s; }"
    ".slider:before { content: ''; position: absolute; height: 20px; width: 20px;"
    "                 left: 3px; bottom: 3px; background: white; border-radius: 50%%;"
    "                 transition: .3s; }"
    ".toggle input:checked + .slider { background: #4ade80; }"
    ".toggle input:checked + .slider:before { transform: translateX(24px); }"
    "input[type=range] { width: 100%%; accent-color: #38bdf8; }"
    "input[type=color] { width: 50px; height: 30px; border: none;"
    "                     border-radius: 6px; cursor: pointer; background: none; }"
    ".led-preview { width: 100%%; height: 24px; border-radius: 8px;"
    "               margin-top: .5rem; border: 1px solid #334155; }"
    "select { background: #334155; color: #e2e8f0; border: 1px solid #475569;"
    "         border-radius: 6px; padding: .3rem .5rem; font-size: .9rem; }"
    "</style>"
    "</head>"
    "<body>"
    /* WiFi status card */
    "<div class=\"card\">"
    "<h1>&#x1F41F; Aquarium Controller</h1>"
    "<div class=\"row\"><span class=\"label\">Status</span>"
    "<span class=\"value %s\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">IP Address</span>"
    "<span class=\"value\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">SSID</span>"
    "<span class=\"value\">%s</span></div>"
    "<div class=\"row\"><span class=\"label\">RSSI</span>"
    "<span class=\"value\">%d dBm</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span>"
    "<span class=\"value\">%" PRIu32 " bytes</span></div>"
    "<div class=\"row\"><span class=\"label\">Uptime</span>"
    "<span class=\"value\">%s</span></div>"
    "</div>"
    /* Water temperature card */
    "<div class=\"card\" id=\"temp-card\">"
    "<h2>&#x1F321;&#xFE0F; Water Temperature</h2>"
    "<div class=\"row\"><span class=\"label\">Temperature</span>"
    "<span class=\"value\" id=\"temp-val\">--</span></div>"
    "</div>"
    /* LED control card */
    "<div class=\"card\" id=\"led-card\">"
    "<h2>&#x1F4A1; LED Strip Control</h2>"
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
    "<select id=\"led-scene\" onchange=\"sendScene()\">"
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
    "</div>"
    /* Geolocation settings card */
    "<div class=\"card\" id=\"geo-card\">"
    "<h2>&#x1F30D; Geolocation</h2>"
    "<div class=\"row\"><span class=\"label\">Latitude</span>"
    "<input type=\"number\" id=\"geo-lat\" step=\"0.0001\" min=\"-90\" max=\"90\""
    " style=\"width:110px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Longitude</span>"
    "<input type=\"number\" id=\"geo-lng\" step=\"0.0001\" min=\"-180\" max=\"180\""
    " style=\"width:110px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">UTC Offset (min)</span>"
    "<input type=\"number\" id=\"geo-utc\" step=\"30\" min=\"-720\" max=\"840\""
    " style=\"width:110px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Sunrise</span>"
    "<span class=\"value\" id=\"geo-sunrise\">--:--</span></div>"
    "<div class=\"row\"><span class=\"label\">Sunset</span>"
    "<span class=\"value\" id=\"geo-sunset\">--:--</span></div>"
    "<button class=\"refresh\" onclick=\"sendGeo()\">Save Location</button>"
    "</div>"
    /* Telegram notification settings card */
    "<div class=\"card\" id=\"tg-card\">"
    "<h2>&#x1F4F1; Telegram Notifications</h2>"
    "<div class=\"row\"><span class=\"label\">Bot Token</span>"
    "<input type=\"password\" id=\"tg-token\" placeholder=\"Not configured\""
    " style=\"width:100%%;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.85rem\"></div>"
    "<div class=\"row\"><span class=\"label\">Chat ID</span>"
    "<input type=\"text\" id=\"tg-chatid\" placeholder=\"e.g. -100123456\""
    " style=\"width:120px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-en\">"
    "<span class=\"slider\"></span></label></div>"
    "<button class=\"refresh\" onclick=\"testTg()\">Send Test Message</button>"
    "<h2 style=\"margin-top:1rem;font-size:1rem\">&#x1F321;&#xFE0F; Temperature Alarms</h2>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-talm\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">High &#xB0;C</span>"
    "<input type=\"number\" id=\"tg-thigh\" step=\"0.5\" min=\"-10\" max=\"50\""
    " style=\"width:80px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Low &#xB0;C</span>"
    "<input type=\"number\" id=\"tg-tlow\" step=\"0.5\" min=\"-10\" max=\"50\""
    " style=\"width:80px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<h2 style=\"margin-top:1rem;font-size:1rem\">&#x1F4A7; Water Change</h2>"
    "<div class=\"row\"><span class=\"label\">Reminder</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-wc\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Interval (days)</span>"
    "<input type=\"number\" id=\"tg-wcdays\" min=\"1\" max=\"90\""
    " style=\"width:80px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Last change</span>"
    "<span class=\"value\" id=\"tg-wclast\">--</span></div>"
    "<button class=\"refresh\" onclick=\"resetWc()\">&#x2705; Record Water Change</button>"
    "<h2 style=\"margin-top:1rem;font-size:1rem\">&#x1F33F; Fertilizer</h2>"
    "<div class=\"row\"><span class=\"label\">Reminder</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-fert\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Interval (days)</span>"
    "<input type=\"number\" id=\"tg-fertdays\" min=\"1\" max=\"90\""
    " style=\"width:80px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<div class=\"row\"><span class=\"label\">Last dose</span>"
    "<span class=\"value\" id=\"tg-fertlast\">--</span></div>"
    "<button class=\"refresh\" onclick=\"resetFert()\">&#x2705; Record Fertilizer Dose</button>"
    "<h2 style=\"margin-top:1rem;font-size:1rem\">&#x1F4CA; Daily Summary</h2>"
    "<div class=\"row\"><span class=\"label\">Enabled</span>"
    "<label class=\"toggle\"><input type=\"checkbox\" id=\"tg-sum\">"
    "<span class=\"slider\"></span></label></div>"
    "<div class=\"row\"><span class=\"label\">Send at hour</span>"
    "<input type=\"number\" id=\"tg-sumhr\" min=\"0\" max=\"23\""
    " style=\"width:80px;background:#334155;color:#e2e8f0;border:1px solid #475569;"
    "border-radius:6px;padding:.3rem .5rem;font-size:.9rem;text-align:right\"></div>"
    "<button class=\"refresh\" onclick=\"saveTg()\">Save Settings</button>"
    "</div>"
    /* Scripts */
    "<script>"
    "function hexToRgb(h){"
    "  var r=parseInt(h.slice(1,3),16),"
    "      g=parseInt(h.slice(3,5),16),"
    "      b=parseInt(h.slice(5,7),16);"
    "  return{r:r,g:g,b:b};}"
    "function rgbToHex(r,g,b){"
    "  return'#'+[r,g,b].map(function(x){"
    "    var h=x.toString(16);return h.length===1?'0'+h:h;}).join('');}"
    "function updatePreview(){"
    "  var on=document.getElementById('led-on').checked;"
    "  var br=parseInt(document.getElementById('led-br').value);"
    "  var c=hexToRgb(document.getElementById('led-color').value);"
    "  if(!on){document.getElementById('led-preview').style.background='#1e293b';return;}"
    "  var s=br/255;"
    "  document.getElementById('led-preview').style.background="
    "    'rgb('+Math.round(c.r*s)+','+Math.round(c.g*s)+','+Math.round(c.b*s)+')';}"
    "function sendLed(){"
    "  updatePreview();"
    "  document.getElementById('led-scene').value='off';"
    "  var on=document.getElementById('led-on').checked;"
    "  var br=parseInt(document.getElementById('led-br').value);"
    "  var c=hexToRgb(document.getElementById('led-color').value);"
    "  fetch('/api/leds',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({on:on,brightness:br,r:c.r,g:c.g,b:c.b})"
    "  }).then(function(r){return r.json();}).then(function(d){"
    "    console.log('LED updated',d);}).catch(function(e){"
    "    console.error('LED error',e);});}"
    "fetch('/api/leds').then(function(r){return r.json();})"
    ".then(function(d){"
    "  document.getElementById('led-on').checked=d.on;"
    "  document.getElementById('led-br').value=d.brightness;"
    "  document.getElementById('br-val').textContent=d.brightness;"
    "  document.getElementById('led-color').value=rgbToHex(d.r,d.g,d.b);"
    "  updatePreview();});"
    "function sendScene(){"
    "  var s=document.getElementById('led-scene').value;"
    "  fetch('/api/scenes',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({scene:s})"
    "  }).then(function(r){return r.json();}).then(function(d){"
    "    console.log('Scene:',d);}).catch(function(e){"
    "    console.error('Scene err',e);});}"
    "fetch('/api/scenes').then(function(r){return r.json();})"
    ".then(function(d){"
    "  document.getElementById('led-scene').value=d.active_scene;});"
    "function loadGeo(){"
    "  fetch('/api/geolocation').then(function(r){return r.json();})"
    "  .then(function(d){"
    "    document.getElementById('geo-lat').value=d.latitude;"
    "    document.getElementById('geo-lng').value=d.longitude;"
    "    document.getElementById('geo-utc').value=d.utc_offset_min;"
    "    document.getElementById('geo-sunrise').textContent="
    "      d.sunrise||'--:--';"
    "    document.getElementById('geo-sunset').textContent="
    "      d.sunset||'--:--';"
    "  });}"
    "function sendGeo(){"
    "  var lat=parseFloat(document.getElementById('geo-lat').value);"
    "  var lng=parseFloat(document.getElementById('geo-lng').value);"
    "  var utc=parseInt(document.getElementById('geo-utc').value);"
    "  fetch('/api/geolocation',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({latitude:lat,longitude:lng,utc_offset_min:utc})"
    "  }).then(function(r){return r.json();}).then(function(d){"
    "    document.getElementById('geo-sunrise').textContent="
    "      d.sunrise||'--:--';"
    "    document.getElementById('geo-sunset').textContent="
    "      d.sunset||'--:--';"
    "    console.log('Geo updated',d);"
    "  }).catch(function(e){console.error('Geo error',e);});}"
    "loadGeo();"
    "function tgTs(id,ts){"
    "  var el=document.getElementById(id);"
    "  if(ts>0){var d=Math.floor((Date.now()/1000-ts)/86400);"
    "    el.textContent=d+' days ago';}"
    "  else{el.textContent='Never';}}"
    "function loadTg(){"
    "  fetch('/api/telegram').then(function(r){return r.json();})"
    "  .then(function(d){"
    "    if(d.bot_token_set){document.getElementById('tg-token').placeholder='Token configured \\u2713';}"
    "    document.getElementById('tg-chatid').value=d.chat_id||'';"
    "    document.getElementById('tg-en').checked=d.enabled;"
    "    document.getElementById('tg-talm').checked=d.temp_alarm_enabled;"
    "    document.getElementById('tg-thigh').value=d.temp_high_c;"
    "    document.getElementById('tg-tlow').value=d.temp_low_c;"
    "    document.getElementById('tg-wc').checked=d.water_change_enabled;"
    "    document.getElementById('tg-wcdays').value=d.water_change_days;"
    "    document.getElementById('tg-fert').checked=d.fertilizer_enabled;"
    "    document.getElementById('tg-fertdays').value=d.fertilizer_days;"
    "    document.getElementById('tg-sum').checked=d.daily_summary_enabled;"
    "    document.getElementById('tg-sumhr').value=d.daily_summary_hour;"
    "    tgTs('tg-wclast',d.last_water_change);"
    "    tgTs('tg-fertlast',d.last_fertilizer);"
    "  });}"
    "function saveTg(){"
    "  var data={"
    "    chat_id:document.getElementById('tg-chatid').value,"
    "    enabled:document.getElementById('tg-en').checked,"
    "    temp_alarm_enabled:document.getElementById('tg-talm').checked,"
    "    temp_high_c:parseFloat(document.getElementById('tg-thigh').value),"
    "    temp_low_c:parseFloat(document.getElementById('tg-tlow').value),"
    "    water_change_enabled:document.getElementById('tg-wc').checked,"
    "    water_change_days:parseInt(document.getElementById('tg-wcdays').value),"
    "    fertilizer_enabled:document.getElementById('tg-fert').checked,"
    "    fertilizer_days:parseInt(document.getElementById('tg-fertdays').value),"
    "    daily_summary_enabled:document.getElementById('tg-sum').checked,"
    "    daily_summary_hour:parseInt(document.getElementById('tg-sumhr').value)};"
    "  var tk=document.getElementById('tg-token').value;"
    "  if(tk.length>0){data.bot_token=tk;}"
    "  fetch('/api/telegram',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify(data)"
    "  }).then(function(r){return r.json();}).then(function(d){"
    "    if(d.bot_token_set){document.getElementById('tg-token').value='';"
    "      document.getElementById('tg-token').placeholder='Token configured \\u2713';}"
    "    console.log('Telegram saved',d);"
    "  }).catch(function(e){console.error('Telegram error',e);});}"
    "function testTg(){"
    "  fetch('/api/telegram_test',{method:'POST'})"
    "  .then(function(r){return r.json();}).then(function(d){"
    "    alert(d.ok?'Test message sent!':'Failed: '+(d.error||'unknown'));}"
    "  ).catch(function(e){alert('Error: '+e);});}"
    "function resetWc(){"
    "  fetch('/api/telegram_wc',{method:'POST'})"
    "  .then(function(r){return r.json();}).then(function(d){"
    "    tgTs('tg-wclast',d.last_water_change);});}"
    "function resetFert(){"
    "  fetch('/api/telegram_fert',{method:'POST'})"
    "  .then(function(r){return r.json();}).then(function(d){"
    "    tgTs('tg-fertlast',d.last_fertilizer);});}"
    "loadTg();"
    "function loadTemp(){"
    "  fetch('/api/temperature').then(function(r){return r.json();})"
    "  .then(function(d){"
    "    var el=document.getElementById('temp-val');"
    "    if(d.valid){el.textContent=d.temperature_c.toFixed(2)+' \\u00B0C';"
    "      el.className='value ok';}"
    "    else{el.textContent='No sensor';el.className='value err';}"
    "  }).catch(function(e){console.error('Temp error',e);});}"
    "loadTemp();setInterval(loadTemp,5000);"
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
                       ws.connected ? ws.ip : "—",
                       ws.connected ? ws.ssid : "—",
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

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 8192;
    config.max_uri_handlers = 20;
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
    httpd_register_uri_handler(s_server, &uri_api_tg_get);
    httpd_register_uri_handler(s_server, &uri_api_tg_post);
    httpd_register_uri_handler(s_server, &uri_api_tg_test);
    httpd_register_uri_handler(s_server, &uri_api_tg_wc);
    httpd_register_uri_handler(s_server, &uri_api_tg_fert);

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
