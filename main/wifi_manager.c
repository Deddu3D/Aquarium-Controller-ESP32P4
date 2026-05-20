/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - WiFi Manager implementation
 * Handles WiFi STA connection lifecycle via ESP32-C6 coprocessor.
 * Falls back to a captive-portal Access Point for initial setup.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_manager.h"
#include "mdns.h"
#include "esp_system.h"

/* ── Configuration ───────────────────────────────────────────────── */

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "your_ssid"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "your_password"
#endif

/* AP provisioning network name */
#define AP_SSID        "AquariumSetup"
#define AP_CHANNEL     1
#define AP_MAX_CONN    4

/* ── Private constants ───────────────────────────────────────────── */

static const char *TAG = "wifi_mgr";

/* FreeRTOS event-group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Reconnection back-off parameters */
#define WIFI_BACKOFF_INITIAL_MS   1000
#define WIFI_BACKOFF_MAX_MS       60000
#define WIFI_INIT_TIMEOUT_MS      30000

/* NVS credential storage */
#define NVS_NAMESPACE  "wifi_creds"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "password"
#define NVS_KEY_MDNS   "mdns_host"

/* Default mDNS hostname */
#define MDNS_HOST_DEFAULT "aquarium"

/* ── Private state ───────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_count;
static bool               s_is_connected;
static bool               s_ap_mode;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static uint32_t           s_backoff_ms;
static httpd_handle_t     s_portal_server  = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

/* Deferred provisioning timer: fires after the HTTP response is delivered
 * and calls esp_restart() to reconnect with new credentials from NVS. */
static esp_timer_handle_t s_prov_timer = NULL;

/* Stored credentials (loaded from NVS, falling back to Kconfig) */
static char s_ssid[WIFI_SSID_MAX];
static char s_password[WIFI_PASSWORD_MAX];
static char s_mdns_host[WIFI_MDNS_HOST_MAX];

/* ── NVS credential helpers ──────────────────────────────────────── */

static void nvs_load_credentials(void)
{
    /* Seed with Kconfig defaults */
    strncpy(s_ssid,     CONFIG_WIFI_SSID,     sizeof(s_ssid)     - 1);
    strncpy(s_password, CONFIG_WIFI_PASSWORD, sizeof(s_password) - 1);
    s_ssid[sizeof(s_ssid) - 1]         = '\0';
    s_password[sizeof(s_password) - 1] = '\0';

    /* Seed mDNS hostname with default */
    strncpy(s_mdns_host, MDNS_HOST_DEFAULT, sizeof(s_mdns_host) - 1);
    s_mdns_host[sizeof(s_mdns_host) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No stored credentials – using Kconfig defaults");
        return;
    }

    size_t len = sizeof(s_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_ssid, &len) != ESP_OK) {
        /* Keep default */
    }
    len = sizeof(s_password);
    if (nvs_get_str(h, NVS_KEY_PASS, s_password, &len) != ESP_OK) {
        /* Keep default */
    }
    len = sizeof(s_mdns_host);
    if (nvs_get_str(h, NVS_KEY_MDNS, s_mdns_host, &len) != ESP_OK) {
        /* Keep default */
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded credentials for SSID: %s  mDNS: %s.local",
             s_ssid, s_mdns_host);
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, password);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Reconnect timer ─────────────────────────────────────────────── */

/**
 * @brief One-shot timer callback that retries esp_wifi_connect().
 *
 * Using a timer instead of vTaskDelay() in the event handler avoids
 * blocking the esp_event_loop_task, which would starve all other
 * WiFi / IP event processing.
 */
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* ── Event handler ───────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* In APSTA provisioning mode the STA interface starts but
             * must NOT try to connect – we only use it for scanning. */
            if (!s_ap_mode) {
                ESP_LOGI(TAG, "WiFi STA started – connecting to %s …", s_ssid);
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "APSTA: STA iface ready for scanning");
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_is_connected = false;
            s_retry_count++;
            ESP_LOGW(TAG, "Disconnected – retry %d (backoff %" PRIu32 " ms)",
                     s_retry_count, s_backoff_ms);
            /* Schedule reconnect via one-shot timer so we never block
             * the esp_event_loop_task with vTaskDelay(). */
            if (s_reconnect_timer != NULL) {
                esp_timer_stop(s_reconnect_timer);   /* cancel any pending */
                esp_timer_start_once(s_reconnect_timer,
                                     (uint64_t)s_backoff_ms * 1000ULL);
            } else {
                /* Timer not yet created – connect immediately */
                esp_wifi_connect();
            }
            s_backoff_ms *= 2;
            if (s_backoff_ms > WIFI_BACKOFF_MAX_MS) {
                s_backoff_ms = WIFI_BACKOFF_MAX_MS;
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event =
                (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " joined AP, AID=%d",
                     MAC2STR(event->mac), event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event =
                (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " left AP, AID=%d",
                     MAC2STR(event->mac), event->aid);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected – IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count  = 0;
        s_backoff_ms   = WIFI_BACKOFF_INITIAL_MS;
        s_is_connected = true;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        /* ── mDNS: announce <hostname>.local on the LAN ──────────── */
        esp_err_t mdns_err = mdns_init();
        if (mdns_err == ESP_OK) {
            mdns_hostname_set(s_mdns_host);
            mdns_instance_name_set("Aquarium Controller");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "mDNS started – device reachable at http://%s.local",
                     s_mdns_host);
        } else {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
        }
    }
}

/* ── JSON helper for portal API handlers ─────────────────────────── */

/* Delay between sending the provisioning response and calling esp_restart().
 * 500 ms is enough for TCP to deliver the HTTP reply to the client. */
#define PROV_RESTART_DELAY_US  500000ULL
#define MAX_SCAN_NETWORKS      24
/* Response buffer for the scan JSON (24 networks × ~80 chars each) */
#define SCAN_RESPONSE_BUF_SIZE 2048
/* Maximum SSID length as per IEEE 802.11 */
#define WIFI_SSID_RAW_MAX      32
/* Maximum escaped SSID length (each byte can become 2 chars + NUL) */
#define WIFI_SSID_ESC_MAX      (WIFI_SSID_RAW_MAX * 2 + 4)

/**
 * @brief Extract a JSON string value for @p key from a flat JSON object.
 *
 * Handles basic \" and \\ escape sequences. Returns the number of bytes
 * written to @p out (0 if the key is not found or the value is not a
 * string).
 */
static int prov_json_str(const char *json, const char *key,
                          char *out, size_t out_size)
{
    /* needle = '"' + key + '"'  (max key we ever pass is 8 chars) */
    char needle[16];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) p++; /* consume backslash */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (int)i;
}

/* ── Portal: WiFi scan endpoint (GET /api/wifi_scan) ─────────────── */

static esp_err_t portal_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    /* Blocking scan (~2 s). The httpd task is blocked but that is
     * acceptable here – we are in provisioning mode with a single client. */
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"networks\":[]}", -1);
    }

    /* Allocate large buffers on the heap to avoid overflowing the httpd
     * task stack (default ~4 KB).  records[] alone is ~2.3 KB and buf is
     * 2 KB, which together exceed the stack budget. */
    wifi_ap_record_t *records = calloc(MAX_SCAN_NETWORKS, sizeof(wifi_ap_record_t));
    char             *buf     = calloc(1, SCAN_RESPONSE_BUF_SIZE);
    if (!records || !buf) {
        free(records);
        free(buf);
        ESP_LOGE(TAG, "portal_scan_handler: out of memory");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"networks\":[]}", -1);
    }

    uint16_t count = MAX_SCAN_NETWORKS;
    esp_wifi_scan_get_ap_records(&count, records);

    /* Build JSON: {"networks":[{"ssid":"...","rssi":-55,"open":false},...]} */
    int  pos = 0;
    int  added = 0;
    pos += snprintf(buf + pos, SCAN_RESPONSE_BUF_SIZE - pos, "{\"networks\":[");
    for (int i = 0; i < (int)count && pos < (int)(SCAN_RESPONSE_BUF_SIZE - 100); i++) {
        if (records[i].ssid[0] == '\0') continue;
        /* Escape SSID: replace \ → \\ and " → \" */
        char ssid_esc[WIFI_SSID_ESC_MAX];
        memset(ssid_esc, 0, sizeof(ssid_esc));
        int  si = 0;
        for (int j = 0;
             records[i].ssid[j] && j < WIFI_SSID_RAW_MAX && si < WIFI_SSID_RAW_MAX * 2;
             j++) {
            char c = (char)records[i].ssid[j];
            if (c == '"' || c == '\\') ssid_esc[si++] = '\\';
            ssid_esc[si++] = c;
        }
        ssid_esc[si] = '\0';
        bool is_open = (records[i].authmode == WIFI_AUTH_OPEN);
        if (added > 0) pos += snprintf(buf + pos, SCAN_RESPONSE_BUF_SIZE - pos, ",");
        pos += snprintf(buf + pos, SCAN_RESPONSE_BUF_SIZE - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
            ssid_esc, (int)records[i].rssi, is_open ? "true" : "false");
        added++;
    }
    pos += snprintf(buf + pos, SCAN_RESPONSE_BUF_SIZE - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, buf, pos);
    free(records);
    free(buf);
    return ret;
}

/* ── Portal: provision endpoint (POST /api/provision) ───────────── */

/** Timer callback: restart the device after credentials have been saved.
 *
 * We restart instead of performing a live APSTA→STA transition because:
 *  - The live switch involves blocking calls (httpd_stop, esp_wifi_stop,
 *    esp_wifi_set_mode …) inside the ESP_TIMER_TASK, which has a small
 *    stack (~3 KB) and must not block.
 *  - On ESP32-P4 with esp_hosted (SDIO), the transition can race with
 *    the C6 coprocessor, causing ESP_ERROR_CHECK failures → abort.
 *  - Credentials are already persisted to NVS before this timer fires,
 *    so the device reconnects to the new network on the next boot.
 */
static void prov_apply_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Provisioning complete – restarting to connect to new network …");
    esp_restart();
}

static esp_err_t portal_provision_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int  received  = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"msg\":\"empty body\"}", -1);
    }
    body[received] = '\0';

    char new_ssid[WIFI_SSID_MAX]         = {0};
    char new_password[WIFI_PASSWORD_MAX] = {0};
    char new_mdns[WIFI_MDNS_HOST_MAX]    = {0};

    prov_json_str(body, "ssid",     new_ssid,     sizeof(new_ssid));
    prov_json_str(body, "password", new_password, sizeof(new_password));
    prov_json_str(body, "mdns",     new_mdns,     sizeof(new_mdns));

    if (new_ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"msg\":\"ssid required\"}", -1);
    }

    ESP_LOGI(TAG, "Provision: SSID='%s'  mDNS='%s'", new_ssid, new_mdns);

    /* Persist credentials to NVS BEFORE sending the response so they are
     * guaranteed to survive the imminent restart even if the TCP connection
     * drops immediately after we send the reply. */
    esp_err_t save_err = nvs_save_credentials(new_ssid, new_password);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(save_err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"msg\":\"storage error\"}", -1);
    }

    if (new_mdns[0] != '\0') {
        wifi_manager_set_mdns_hostname(new_mdns);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_err = httpd_resp_send(req, "{\"status\":\"ok\"}", -1);

    /* Schedule a restart (500 ms) so TCP has time to deliver the reply.
     * prov_apply_cb will call esp_restart(); no WiFi mode switch is needed
     * because the device simply reboots and reconnects via the new credentials
     * it just saved to NVS. */
    if (s_prov_timer != NULL) {
        esp_timer_start_once(s_prov_timer, PROV_RESTART_DELAY_US);
    }

    return send_err;
}

/* ── Captive portal HTML ─────────────────────────────────────────── */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html lang=\"it\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Aquarium Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:sans-serif;background:#0b1121;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;"
    "min-height:100vh;padding:1rem}"
    ".card{background:#131c31;border:1px solid #1a2540;border-radius:12px;"
    "padding:2rem;max-width:420px;width:100%%}"
    "h1{color:#38bdf8;font-size:1.4rem;margin-bottom:.5rem}"
    "p{color:#64748b;font-size:.9rem;margin-bottom:1.5rem}"
    "label{display:block;color:#94a3b8;font-size:.85rem;margin-bottom:.25rem}"
    "input{width:100%%;background:#1e293b;color:#e2e8f0;"
    "border:1px solid #334155;border-radius:8px;padding:.6rem .8rem;"
    "font-size:1rem;margin-bottom:1rem}"
    "input:focus{outline:none;border-color:#38bdf8}"
    "button{width:100%%;background:#38bdf8;color:#0f172a;border:none;"
    "border-radius:8px;padding:.75rem;font-size:1rem;font-weight:700;"
    "cursor:pointer}"
    "button:active{background:#7dd3fc}"
    ".msg{margin-top:1rem;font-size:.85rem;color:#4ade80;display:none}"
    ".err{color:#f87171}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>&#x1F420; Aquarium Setup</h1>"
    "<p>Inserisci le credenziali WiFi per collegare il controller alla rete.</p>"
    "<form id=\"f\" onsubmit=\"sub(event)\">"
    "<label>SSID (nome rete)</label>"
    "<input type=\"text\" id=\"s\" name=\"ssid\" required placeholder=\"MyNetwork\">"
    "<label>Password</label>"
    "<input type=\"password\" id=\"p\" name=\"password\" placeholder=\"(vuota se rete aperta)\">"
    "<button type=\"submit\">Connetti</button>"
    "</form>"
    "<div class=\"msg\" id=\"m\"></div>"
    "</div>"
    "<script>"
    "function sub(e){"
    "  e.preventDefault();"
    "  var m=document.getElementById('m');"
    "  m.style.display='block';m.className='msg';m.textContent='Connessione...';"
    "  fetch('/wifi_save',{method:'POST',"
    "    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "    body:'ssid='+encodeURIComponent(document.getElementById('s').value)"
    "         +'&password='+encodeURIComponent(document.getElementById('p').value)"
    "  }).then(function(r){return r.text()}).then(function(t){"
    "    m.textContent=t;"
    "    if(t.indexOf('OK')===0){"
    "      m.className='msg';"
    "      setTimeout(function(){window.location.reload()},5000)}"
    "    else{m.className='msg err'}"
    "  }).catch(function(){"
    "    m.className='msg err';m.textContent='Errore di rete'})"
    "}"
    "</script>"
    "</body></html>";

/* ── Portal HTTP handlers ────────────────────────────────────────── */

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, (int)strlen(PORTAL_HTML));
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Parse URL-encoded form: ssid=...&password=... */
    char new_ssid[WIFI_SSID_MAX]         = {0};
    char new_password[WIFI_PASSWORD_MAX] = {0};

    /* Simple URL-form parser */
    const char *p = body;
    while (p && *p) {
        /* key */
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t klen = (size_t)(eq - p);
        const char *amp = strchr(eq + 1, '&');
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);

        /* URL-decode value inline (handle %xx) */
        char val[WIFI_PASSWORD_MAX] = {0};
        size_t vi = 0;
        const char *vp = eq + 1;
        for (size_t i = 0; i < vlen && vi < sizeof(val) - 1; i++) {
            if (vp[i] == '%' && i + 2 < vlen) {
                char hex[3] = {vp[i+1], vp[i+2], 0};
                val[vi++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else if (vp[i] == '+') {
                val[vi++] = ' ';
            } else {
                val[vi++] = vp[i];
            }
        }
        val[vi] = '\0';

        if (klen == 4 && strncmp(p, "ssid", 4) == 0) {
            strncpy(new_ssid, val, sizeof(new_ssid) - 1);
        } else if (klen == 8 && strncmp(p, "password", 8) == 0) {
            strncpy(new_password, val, sizeof(new_password) - 1);
        }

        p = amp ? amp + 1 : NULL;
    }

    if (new_ssid[0] == '\0') {
        httpd_resp_send(req, "ERR: SSID vuoto", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Portal: saving credentials for SSID '%s'", new_ssid);

    /* Save credentials to NVS.  Do NOT call wifi_manager_set_credentials()
     * here: that function calls httpd_stop() which would deadlock because we
     * are currently running inside the very HTTP handler of that server. */
    esp_err_t err = nvs_save_credentials(new_ssid, new_password);
    if (err != ESP_OK) {
        httpd_resp_send(req, "ERR: salvataggio fallito", -1);
        return ESP_OK;
    }

    /* Send the response, then schedule a restart so TCP can deliver it. */
    httpd_resp_send(req,
        "OK: Credenziali salvate. Il controller si riavvierà e si connetterà...", -1);

    if (s_prov_timer != NULL) {
        esp_timer_start_once(s_prov_timer, PROV_RESTART_DELAY_US);
    }

    return ESP_OK;
}

/* ── STA connection helpers ──────────────────────────────────────── */

static esp_err_t start_sta_mode(void)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,     s_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_password,
            sizeof(wifi_config.sta.password) - 1);

    /* Allow open networks */
    if (s_password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start() failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t start_ap_mode(void)
{
    s_ap_mode = true;

    wifi_config_t ap_config = {
        .ap = {
            .ssid            = AP_SSID,
            .ssid_len        = strlen(AP_SSID),
            .channel         = AP_CHANNEL,
            .password        = "",           /* open network */
            .max_connection  = AP_MAX_CONN,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* APSTA: AP active + STA available for scanning */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP mode started – SSID: %s  IP: 192.168.4.1", AP_SSID);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count      = 0;
    s_is_connected     = false;
    s_ap_mode          = false;
    s_backoff_ms       = WIFI_BACKOFF_INITIAL_MS;

    /* Load stored credentials (overrides Kconfig) */
    nvs_load_credentials();

    /* TCP/IP & event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Create the one-shot timer used for back-off reconnect */
    esp_timer_create_args_t timer_args = {
        .callback        = reconnect_timer_cb,
        .arg             = NULL,
        .name            = "wifi_reconnect",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    /* Create the one-shot timer used to apply provisioning credentials
     * after the HTTP response has been delivered to the Android app. */
    esp_timer_create_args_t prov_timer_args = {
        .callback        = prov_apply_cb,
        .arg             = NULL,
        .name            = "prov_apply",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&prov_timer_args, &s_prov_timer));

    esp_event_handler_instance_t inst_any_wifi;
    esp_event_handler_instance_t inst_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &inst_any_wifi));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &inst_got_ip));

    /* If SSID is set, attempt STA mode first */
    if (s_ssid[0] != '\0') {
        start_sta_mode();

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_INIT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to SSID: %s", s_ssid);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "STA connection timed out after %d ms", WIFI_INIT_TIMEOUT_MS);
        /* Stop STA so we can switch to AP */
        esp_wifi_stop();
    } else {
        ESP_LOGW(TAG, "No SSID configured – entering AP provisioning mode");
    }

    /* Fallback: start AP provisioning portal */
    start_ap_mode();
    wifi_manager_start_portal();
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}

void wifi_manager_get_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) return;

    if (s_ap_mode) {
        strncpy(buf, "192.168.4.1", len - 1);
        buf[len - 1] = '\0';
        return;
    }

    if (s_is_connected && s_sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }

    strncpy(buf, "0.0.0.0", len - 1);
    buf[len - 1] = '\0';
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_password, password ? password : "", sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    esp_err_t err = nvs_save_credentials(s_ssid, s_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Credentials saved for '%s' – restarting STA …", s_ssid);

    /* Stop AP, switch to STA */
    esp_wifi_stop();
    s_ap_mode      = false;
    s_is_connected = false;
    s_retry_count  = 0;
    s_backoff_ms   = WIFI_BACKOFF_INITIAL_MS;

    /* Cancel any pending reconnect timer */
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }

    /* Stop portal if running */
    if (s_portal_server) {
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
    }

    start_sta_mode();
    return ESP_OK;
}

esp_err_t wifi_manager_start_portal(void)
{
    if (s_portal_server != NULL) {
        return ESP_OK;   /* already running */
    }

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.max_uri_handlers = 6;
    config.stack_size      = 6144;   /* default 4096 is too small for scan handler */

    if (httpd_start(&s_portal_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start portal HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET,
        .handler = portal_root_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_save = {
        .uri = "/wifi_save", .method = HTTP_POST,
        .handler = portal_save_handler, .user_ctx = NULL,
    };
    /* Redirect all other URIs to root (captive portal behaviour) */
    static const httpd_uri_t uri_any = {
        .uri = "/*", .method = HTTP_GET,
        .handler = portal_root_handler, .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_portal_server, &uri_root);
    httpd_register_uri_handler(s_portal_server, &uri_save);
    httpd_register_uri_handler(s_portal_server, &uri_any);

    /* JSON API endpoints used by the Android provisioning wizard */
    static const httpd_uri_t uri_scan = {
        .uri = "/api/wifi_scan", .method = HTTP_GET,
        .handler = portal_scan_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_provision = {
        .uri = "/api/provision", .method = HTTP_POST,
        .handler = portal_provision_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_portal_server, &uri_scan);
    httpd_register_uri_handler(s_portal_server, &uri_provision);

    ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1");
    return ESP_OK;
}

void wifi_manager_get_mdns_hostname(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_mdns_host, len - 1);
    buf[len - 1] = '\0';
}

esp_err_t wifi_manager_set_mdns_hostname(const char *host)
{
    if (!host || host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Validate: only letters, digits, hyphens; no leading/trailing hyphen */
    size_t hlen = strlen(host);
    if (hlen >= WIFI_MDNS_HOST_MAX) return ESP_ERR_INVALID_ARG;
    if (host[0] == '-' || host[hlen - 1] == '-') return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < hlen; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Persist to NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_MDNS, host);
        nvs_commit(h);
        nvs_close(h);
    }

    strncpy(s_mdns_host, host, sizeof(s_mdns_host) - 1);
    s_mdns_host[sizeof(s_mdns_host) - 1] = '\0';

    /* Apply immediately if mDNS is running */
    mdns_hostname_set(s_mdns_host);
    ESP_LOGI(TAG, "mDNS hostname changed to %s.local", s_mdns_host);
    return ESP_OK;
}
