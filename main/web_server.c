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
#include "sd_card.h"
#include "sd_logger.h"

#include <sys/stat.h>
#include <dirent.h>

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

/* Chunk size for streaming HTML from SD card */
#define HTML_SD_CHUNK_SIZE     2048

/* JSON response buffer sizes */
#define JSON_STATUS_BUF_SIZE   512   /* enlarged from 384: now includes ntp_ok + partition */
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
#define JSON_SD_STATUS_BUF_SIZE 256
#define SD_DOWNLOAD_CHUNK       4096

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

/* HTTP server configuration */
#define HTTP_STACK_SIZE        8192
#define HTTP_MAX_URI_HANDLERS  61   /* +1 for the /www/* static file handler */

/* ── Static file server (/www/* → /sdcard/www/*) ─────────────────── */

/**
 * @brief Return the MIME type string for a file path based on its extension.
 */
static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".gif")  == 0) return "image/gif";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    if (strcmp(ext, ".woff2")== 0) return "font/woff2";
    if (strcmp(ext, ".ttf")  == 0) return "font/ttf";
    return "application/octet-stream";
}

/**
 * @brief Serve any static file under /sdcard/www/ for GET /www/* requests.
 *
 * The URI /www/style.css maps to /sdcard/www/style.css, etc.
 * Files are streamed in 2 KB chunks with Cache-Control: max-age=3600.
 * Allows the web UI to reference separate CSS, JS, image and font files
 * that live alongside index.html on the SD card.
 */
static esp_err_t static_file_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;   /* e.g. "/www/style.css" */

    /* Build SD card path: /sdcard + uri → /sdcard/www/style.css */
    char sd_path[128];
    int n = snprintf(sd_path, sizeof(sd_path), "%s%s", SD_MOUNT_POINT, uri);
    if (n <= 0 || (size_t)n >= sizeof(sd_path)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }

    /* Reject path traversal attempts */
    if (strstr(sd_path, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

#ifdef CONFIG_SD_CARD_ENABLED
    if (sd_card_is_mounted()) {
        FILE *f = fopen(sd_path, "r");
        if (f != NULL) {
            httpd_resp_set_type(req, get_mime_type(sd_path));
            httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

            char *chunk = malloc(HTML_SD_CHUNK_SIZE);
            if (chunk == NULL) {
                fclose(f);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "Out of memory");
                return ESP_FAIL;
            }
            size_t bytes;
            while ((bytes = fread(chunk, 1, HTML_SD_CHUNK_SIZE, f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)bytes) != ESP_OK) {
                    break;
                }
            }
            fclose(f);
            free(chunk);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
        ESP_LOGD(TAG, "Static file not found: %s", sd_path);
    }
#endif

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
}


 *
 * Reads /sdcard/www/index.html in chunks and streams it to the client.
 * If the SD card is not mounted or the file is not found, a minimal
 * fallback page is sent asking the user to copy www/ to the card.
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

#ifdef CONFIG_SD_CARD_ENABLED
    if (sd_card_is_mounted()) {
        FILE *f = fopen(SD_WWW_INDEX, "r");
        if (f != NULL) {
            char *chunk = malloc(HTML_SD_CHUNK_SIZE);
            if (chunk == NULL) {
                fclose(f);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "Failed to allocate buffer for streaming Web UI");
                return ESP_FAIL;
            }
            size_t n;
            while ((n = fread(chunk, 1, HTML_SD_CHUNK_SIZE, f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    break;
                }
            }
            fclose(f);
            free(chunk);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
        /* File not found on SD: fall through to fallback */
        ESP_LOGW(TAG, "Web UI file not found: " SD_WWW_INDEX);
    }
#endif

    /* Fallback: SD card not mounted or index.html missing */
    static const char FALLBACK_HTML[] =
        "<!DOCTYPE html><html lang='it'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Aquarium Controller</title>"
        "<style>body{font-family:sans-serif;background:#0b1121;color:#e2e8f0;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#131c31;border:1px solid #1a2540;border-radius:12px;"
        "padding:2rem;max-width:420px;text-align:center}"
        "h1{color:#38bdf8;margin-bottom:1rem;font-size:1.3rem}"
        "p{color:#94a3b8;font-size:.9rem;line-height:1.6;margin-bottom:.7rem}"
        "code{background:#1e293b;padding:.2rem .5rem;border-radius:4px;"
        "font-size:.85rem;color:#fbbf24}"
        "</style></head><body>"
        "<div class='box'>"
        "<h1>&#x1F4BE; Web UI non trovata</h1>"
        "<p>Copia la cartella <code>www/</code> sulla SD card:</p>"
        "<p><code>/sdcard/www/index.html</code></p>"
        "<p>La SD card deve essere montata e il file deve essere presente "
        "per accedere alla dashboard.</p>"
        "<p><a href='/api/status' style='color:#38bdf8'>Stato API JSON</a></p>"
        "</div></body></html>";
    return httpd_resp_send(req, FALLBACK_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ── JSON status endpoint (/api/status  GET) ─────────────────────── */

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    get_wifi_status(&ws);

    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char escaped_ssid[128];
    json_escape(ws.connected ? ws.ssid : "", escaped_ssid, sizeof(escaped_ssid));

    /* NTP status – time is valid once the year is >= 2024 */
    time_t now_t = time(NULL);
    struct tm ti;
    localtime_r(&now_t, &ti);
    bool ntp_ok = (ti.tm_year >= (2024 - 1900));

    /* Running OTA partition label */
    const esp_partition_t *part = esp_ota_get_running_partition();
    const char *part_label = part ? part->label : "unknown";

    char buf[JSON_STATUS_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,"
        "\"ip\":\"%s\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"free_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRId64 ","
        "\"ntp_ok\":%s,"
        "\"partition\":\"%s\"}",
        ws.connected ? "true" : "false",
        ws.connected ? ws.ip : "",
        escaped_ssid,
        ws.connected ? ws.rssi : 0,
        esp_get_free_heap_size(),
        uptime_s,
        ntp_ok ? "true" : "false",
        part_label);

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

/* Forward declarations for SD card and SD-OTA handlers defined later */
static esp_err_t api_sdcard_status_handler(httpd_req_t *req);
static esp_err_t api_sdcard_ls_handler(httpd_req_t *req);
static esp_err_t api_sdcard_download_handler(httpd_req_t *req);
static esp_err_t api_sdcard_delete_handler(httpd_req_t *req);
static esp_err_t api_sdcard_config_export_handler(httpd_req_t *req);
static esp_err_t api_sdcard_config_import_handler(httpd_req_t *req);
static esp_err_t api_ota_sd_post_handler(httpd_req_t *req);

static const httpd_uri_t uri_api_sdcard_status = {
    .uri      = "/api/sdcard",
    .method   = HTTP_GET,
    .handler  = api_sdcard_status_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_sdcard_ls = {
    .uri      = "/api/sdcard/ls",
    .method   = HTTP_GET,
    .handler  = api_sdcard_ls_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_sdcard_download = {
    .uri      = "/api/sdcard/download",
    .method   = HTTP_GET,
    .handler  = api_sdcard_download_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_sdcard_delete = {
    .uri      = "/api/sdcard/delete",
    .method   = HTTP_DELETE,
    .handler  = api_sdcard_delete_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_sdcard_config_export = {
    .uri      = "/api/sdcard/config/export",
    .method   = HTTP_POST,
    .handler  = api_sdcard_config_export_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_sdcard_config_import = {
    .uri      = "/api/sdcard/config/import",
    .method   = HTTP_POST,
    .handler  = api_sdcard_config_import_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_api_ota_sd = {
    .uri      = "/api/ota/sd",
    .method   = HTTP_POST,
    .handler  = api_ota_sd_post_handler,
    .user_ctx = NULL,
};

/* Wildcard handler: serve any static file from /sdcard/www/ */
static const httpd_uri_t uri_www_static = {
    .uri      = "/www/*",
    .method   = HTTP_GET,
    .handler  = static_file_get_handler,
    .user_ctx = NULL,
};

/* ── SD Card REST API handlers ───────────────────────────────────── */

static esp_err_t api_sdcard_status_handler(httpd_req_t *req)
{
    sd_card_info_t info;
    sd_card_get_info(&info);

    char buf[JSON_SD_STATUS_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"mounted\":%s,"
        "\"card_name\":\"%s\","
        "\"total_bytes\":%" PRIu64 ","
        "\"free_bytes\":%" PRIu64 ","
        "\"card_speed_khz\":%" PRIu32 "}",
        info.mounted ? "true" : "false",
        info.card_name,
        info.total_bytes,
        info.free_bytes,
        info.card_speed_khz);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t api_sdcard_ls_handler(httpd_req_t *req)
{
    /* Read ?path= query parameter */
    char path[128] = SD_MOUNT_POINT;
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[128];
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            /* Sanitise: must start with mount point and must not contain
             * path traversal sequences. */
            if (strncmp(val, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0 &&
                strstr(val, "..") == NULL) {
                strncpy(path, val, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        }
    }

    if (!sd_card_is_mounted()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"SD card not mounted\",\"entries\":[]}");
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"Cannot open directory\",\"entries\":[]}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    httpd_resp_sendstr_chunk(req, path);
    httpd_resp_sendstr_chunk(req, "\",\"entries\":[");

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Get file size */
        char full_path[192];
        int fp_len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        /* Reject if path overflowed or traverses outside mount point */
        if (fp_len >= (int)sizeof(full_path) ||
            strncmp(full_path, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) != 0) {
            continue;
        }
        struct stat st;
        long fsize = 0;
        bool is_dir = (entry->d_type == DT_DIR);
        if (!is_dir && stat(full_path, &st) == 0) {
            fsize = (long)st.st_size;
        }

        char chunk[256];
        /* Escape name for JSON */
        char ename[128];
        json_escape(entry->d_name, ename, sizeof(ename));
        snprintf(chunk, sizeof(chunk),
                 "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%ld}",
                 first ? "" : ",",
                 ename,
                 is_dir ? "true" : "false",
                 fsize);
        httpd_resp_sendstr_chunk(req, chunk);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_sdcard_download_handler(httpd_req_t *req)
{
    char query[256];
    char path[192] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[192];
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            if (strncmp(val, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0 &&
                strstr(val, "..") == NULL) {
                strncpy(path, val, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        }
    }

    if (path[0] == '\0' || !sd_card_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path or SD not mounted");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    /* Derive filename from path for Content-Disposition */
    const char *fname = strrchr(path, '/');
    fname = fname ? (fname + 1) : path;

    char disp[256];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = malloc(SD_DOWNLOAD_CHUNK);
    if (buf == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    while (!feof(f)) {
        size_t len = fread(buf, 1, SD_DOWNLOAD_CHUNK, f);
        if (len > 0) {
            if (httpd_resp_send_chunk(req, buf, (ssize_t)len) != ESP_OK) {
                ret = ESP_FAIL;
                break;
            }
        }
    }
    free(buf);
    fclose(f);

    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t api_sdcard_delete_handler(httpd_req_t *req)
{
    char query[256];
    char path[192] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[192];
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            if (strncmp(val, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0 &&
                strstr(val, "..") == NULL) {
                strncpy(path, val, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        }
    }

    if (path[0] == '\0' || !sd_card_is_mounted()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad path or SD not mounted\"}");
    }

    int r = remove(path);
    httpd_resp_set_type(req, "application/json");
    if (r == 0) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Delete failed\"}");
    }
}

static esp_err_t api_sdcard_config_export_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD card not mounted\"}");
    }
    esp_err_t err = sd_card_config_export(SD_CONFIG_FILE);
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req,
            "{\"ok\":true,\"path\":\"" SD_CONFIG_FILE "\"}");
    } else {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Export failed\"}");
    }
}

static esp_err_t api_sdcard_config_import_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD card not mounted\"}");
    }
    esp_err_t err = sd_card_config_import(SD_CONFIG_FILE);
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Import failed\"}");
    }
}

static esp_err_t api_ota_sd_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD card not mounted\"}");
    }
    if (ota_update_in_progress()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"OTA already in progress\"}");
    }
    esp_err_t err = ota_update_start_from_sd(SD_FIRMWARE_FILE);
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

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
    /* Enable wildcard URI matching so /www/* can serve SD card files */
    ssl_config.httpd.uri_match_fn     = httpd_uri_match_wildcard;

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
    /* Enable wildcard URI matching so /www/* can serve SD card files */
    config.uri_match_fn     = httpd_uri_match_wildcard;

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
    httpd_register_uri_handler(s_server, &uri_api_sdcard_status);
    httpd_register_uri_handler(s_server, &uri_api_sdcard_ls);
    httpd_register_uri_handler(s_server, &uri_api_sdcard_download);
    httpd_register_uri_handler(s_server, &uri_api_sdcard_delete);
    httpd_register_uri_handler(s_server, &uri_api_sdcard_config_export);
    httpd_register_uri_handler(s_server, &uri_api_sdcard_config_import);
    httpd_register_uri_handler(s_server, &uri_api_ota_sd);
    /* Wildcard handler registered last so exact-match API routes take priority */
    httpd_register_uri_handler(s_server, &uri_www_static);

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
