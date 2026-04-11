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
#include <string.h>
#include <inttypes.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "web_srv";

static httpd_handle_t s_server = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

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
    "       display: flex; justify-content: center; align-items: center; }"
    ".card { background: #1e293b; border-radius: 16px; padding: 2rem;"
    "        max-width: 420px; width: 90%%; box-shadow: 0 8px 32px rgba(0,0,0,.4); }"
    "h1 { font-size: 1.4rem; text-align: center; margin-bottom: 1.2rem;"
    "     color: #38bdf8; }"
    ".row { display: flex; justify-content: space-between; padding: .6rem 0;"
    "       border-bottom: 1px solid #334155; }"
    ".row:last-child { border-bottom: none; }"
    ".label { color: #94a3b8; }"
    ".value { font-weight: 600; }"
    ".ok  { color: #4ade80; }"
    ".err { color: #f87171; }"
    ".refresh { display: block; margin: 1.2rem auto 0; padding: .5rem 1.5rem;"
    "           background: #38bdf8; color: #0f172a; border: none;"
    "           border-radius: 8px; font-size: 1rem; cursor: pointer; }"
    ".refresh:hover { background: #7dd3fc; }"
    "</style>"
    "</head>"
    "<body>"
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
    "<button class=\"refresh\" onclick=\"location.reload()\">Refresh</button>"
    "</div>"
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

    /* Render HTML */
    char buf[2048];
    int len = snprintf(buf, sizeof(buf), STATUS_HTML_TEMPLATE,
                       ws.connected ? "ok" : "err",
                       ws.connected ? "Connected" : "Disconnected",
                       ws.connected ? ws.ip : "—",
                       ws.connected ? ws.ssid : "—",
                       ws.connected ? ws.rssi : 0,
                       esp_get_free_heap_size(),
                       uptime);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, len);
}

/* ── JSON status endpoint (/api/status  GET) ─────────────────────── */

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    get_wifi_status(&ws);

    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,"
        "\"ip\":\"%s\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"free_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRId64 "}",
        ws.connected ? "true" : "false",
        ws.connected ? ws.ip : "",
        ws.connected ? ws.ssid : "",
        ws.connected ? ws.rssi : 0,
        esp_get_free_heap_size(),
        uptime_s);

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

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_api_status);

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
