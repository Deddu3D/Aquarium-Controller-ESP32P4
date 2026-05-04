/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Voice Control via Groq Cloud API
 *
 * Pipeline:
 *   INMP441 I2S mic  →  16-bit WAV  →  Groq Whisper STT (Italian)
 *        →  Groq LLM (llama-3.1-8b-instant)  →  JSON command  →  execute
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2s_std.h"
#include "cJSON.h"

#include "voice_control.h"
#include "relay_controller.h"
#include "led_controller.h"
#include "led_scenes.h"
#include "feeding_mode.h"
#include "daily_cycle.h"

static const char *TAG = "voice";

/* ── Constants ───────────────────────────────────────────────────── */

#define NVS_NAMESPACE       "voice"
#define NVS_KEY_API_KEY     "api_key"
#define NVS_KEY_ENABLED     "enabled"
#define NVS_KEY_RECORD_MS   "record_ms"
#define NVS_KEY_STT_MODEL   "stt_model"
#define NVS_KEY_LLM_MODEL   "llm_model"
#define NVS_KEY_SCK_IO      "sck_io"
#define NVS_KEY_WS_IO       "ws_io"
#define NVS_KEY_SD_IO       "sd_io"

#define GROQ_WHISPER_URL    "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_CHAT_URL       "https://api.groq.com/openai/v1/chat/completions"
#define GROQ_TIMEOUT_MS     30000

#define SAMPLE_RATE         16000   /* Hz – Whisper optimal rate            */
#define I2S_DMA_BUF_COUNT   8       /* DMA descriptor count                 */
#define I2S_DMA_BUF_LEN    1024    /* Samples per DMA buffer               */

#define VOICE_TASK_STACK    20480   /* 20 KB: TLS + cJSON + PSRAM alloc     */
#define VOICE_TASK_PRIO     3

#define MULTIPART_BOUNDARY  "----AquariumVoiceBoundary0x7a3f"
#define RESPONSE_BUF_SIZE   4096   /* Buffer for Groq JSON responses       */
#define TRANSCRIPT_MAX      512
#define COMMAND_MAX         512
#define RESULT_MAX          128

/* ── System prompt for the LLM ──────────────────────────────────── */

/*
 * The system prompt is stored in flash (string literal) to save RAM.
 * It instructs the LLM to produce only valid JSON with one of the
 * known action types understood by voice_execute_command().
 */
static const char SYSTEM_PROMPT[] =
    "Sei il controller di un acquario intelligente. "
    "Il tuo compito e' convertire un comando vocale in italiano in un JSON strutturato. "
    "Rispondi SOLO con JSON valido, senza testo aggiuntivo. "
    "Azioni disponibili:\n"
    "1. Controlla un rele' (0=canale1, 1=canale2, 2=canale3, 3=canale4):\n"
    "   {\"action\":\"relay_set\",\"index\":0,\"on\":true}\n"
    "2. Controlla la striscia LED (brightness 0-255):\n"
    "   {\"action\":\"led_set\",\"on\":true,\"brightness\":200}\n"
    "3. Avvia una scena LED (sunrise/sunset/moonlight/storm/clouds/none):\n"
    "   {\"action\":\"led_scene\",\"scene\":\"moonlight\"}\n"
    "4. Avvia la pausa alimentazione:\n"
    "   {\"action\":\"feeding_start\"}\n"
    "5. Abilita o disabilita il ciclo giornaliero automatico:\n"
    "   {\"action\":\"daily_cycle\",\"enabled\":true}\n"
    "Se il comando non e' riconoscibile rispondi:\n"
    "   {\"action\":\"unknown\",\"reason\":\"motivo\"}\n"
    "Esempi: "
    "'accendi il filtro' -> relay 0 ON; "
    "'spegni le luci' -> led_set ON false; "
    "'avvia alba' -> led_scene sunrise; "
    "'pasto pesci' -> feeding_start; "
    "'luna piena' -> led_scene moonlight.";

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex       = NULL;
static voice_status_t    s_status      = VOICE_STATUS_IDLE;
static voice_config_t    s_config;
static char              s_transcript[TRANSCRIPT_MAX];
static char              s_last_command[COMMAND_MAX];
static char              s_last_result[RESULT_MAX];

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    memset(&s_config, 0, sizeof(s_config));

    /* Kconfig defaults */
    s_config.enabled   = false;
    s_config.record_ms = CONFIG_VOICE_RECORD_MS;
    strncpy(s_config.stt_model, CONFIG_VOICE_GROQ_STT_MODEL,
            sizeof(s_config.stt_model) - 1);
    strncpy(s_config.llm_model, CONFIG_VOICE_GROQ_LLM_MODEL,
            sizeof(s_config.llm_model) - 1);
    s_config.i2s_sck_io = CONFIG_VOICE_I2S_SCK_IO;
    s_config.i2s_ws_io  = CONFIG_VOICE_I2S_WS_IO;
    s_config.i2s_sd_io  = CONFIG_VOICE_I2S_SD_IO;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_config.groq_api_key);
    nvs_get_str(h, NVS_KEY_API_KEY, s_config.groq_api_key, &len);

    uint8_t u8;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8) == ESP_OK) s_config.enabled = u8;

    int32_t i32;
    if (nvs_get_i32(h, NVS_KEY_RECORD_MS, &i32) == ESP_OK) s_config.record_ms = i32;
    if (nvs_get_i32(h, NVS_KEY_SCK_IO,    &i32) == ESP_OK) s_config.i2s_sck_io = i32;
    if (nvs_get_i32(h, NVS_KEY_WS_IO,     &i32) == ESP_OK) s_config.i2s_ws_io  = i32;
    if (nvs_get_i32(h, NVS_KEY_SD_IO,     &i32) == ESP_OK) s_config.i2s_sd_io  = i32;

    len = sizeof(s_config.stt_model);
    nvs_get_str(h, NVS_KEY_STT_MODEL, s_config.stt_model, &len);
    len = sizeof(s_config.llm_model);
    nvs_get_str(h, NVS_KEY_LLM_MODEL, s_config.llm_model, &len);

    nvs_close(h);
}

static esp_err_t nvs_save_config(const voice_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, NVS_KEY_API_KEY,   cfg->groq_api_key);
    nvs_set_u8 (h, NVS_KEY_ENABLED,   (uint8_t)cfg->enabled);
    nvs_set_i32(h, NVS_KEY_RECORD_MS, (int32_t)cfg->record_ms);
    nvs_set_i32(h, NVS_KEY_SCK_IO,    (int32_t)cfg->i2s_sck_io);
    nvs_set_i32(h, NVS_KEY_WS_IO,     (int32_t)cfg->i2s_ws_io);
    nvs_set_i32(h, NVS_KEY_SD_IO,     (int32_t)cfg->i2s_sd_io);
    nvs_set_str(h, NVS_KEY_STT_MODEL, cfg->stt_model);
    nvs_set_str(h, NVS_KEY_LLM_MODEL, cfg->llm_model);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Status helpers ──────────────────────────────────────────────── */

static void set_status(voice_status_t st)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status = st;
    xSemaphoreGive(s_mutex);
}

static void set_result(const char *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_last_result, msg, sizeof(s_last_result) - 1);
    s_last_result[sizeof(s_last_result) - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

/* ── WAV header builder ──────────────────────────────────────────── */

/*
 * Fills a 44-byte WAV header for 16-bit PCM mono audio.
 * wav_data_bytes = number of audio data bytes (num_samples × 2).
 */
static void build_wav_header(uint8_t hdr[44], uint32_t wav_data_bytes,
                             uint32_t sample_rate)
{
    uint32_t chunk_size    = 36 + wav_data_bytes;
    uint32_t byte_rate     = sample_rate * 2;   /* 1 channel × 2 bytes */
    uint16_t block_align   = 2;
    uint16_t bits_per_samp = 16;
    uint16_t audio_format  = 1;   /* PCM */
    uint16_t num_channels  = 1;

    /* RIFF chunk */
    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &chunk_size,    4);
    memcpy(hdr + 8,  "WAVE",         4);
    /* fmt sub-chunk */
    memcpy(hdr + 12, "fmt ",         4);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size,      4);
    memcpy(hdr + 20, &audio_format,  2);
    memcpy(hdr + 22, &num_channels,  2);
    memcpy(hdr + 24, &sample_rate,   4);
    memcpy(hdr + 28, &byte_rate,     4);
    memcpy(hdr + 30, &block_align,   2);
    memcpy(hdr + 32, &bits_per_samp, 2);
    /* data sub-chunk */
    memcpy(hdr + 36, "data",         4);
    memcpy(hdr + 40, &wav_data_bytes, 4);
}

/* ── I2S audio capture ───────────────────────────────────────────── */

/*
 * Allocates size bytes, preferring PSRAM (MALLOC_CAP_SPIRAM) and
 * falling back to internal DRAM.  Equivalent to a PSRAM-aware malloc.
 */
static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

/*
 * Opens an I2S RX channel, records for record_ms milliseconds, and
 * converts the raw int32 INMP441 output to int16 PCM samples.
 *
 * Returns a heap_caps_malloc'd int16_t buffer (caller must free) on
 * success, or NULL on any error.  *n_samples receives the sample count.
 */
static int16_t *i2s_record(const voice_config_t *cfg, int *n_samples)
{
    int total_samples = (cfg->record_ms * SAMPLE_RATE) / 1000;
    *n_samples = 0;

    /* Allocate raw (int32) buffer – prefer PSRAM for large buffers */
    int32_t *raw = psram_alloc((size_t)total_samples * sizeof(int32_t));
    if (!raw) {
        ESP_LOGE(TAG, "Cannot allocate %d bytes for raw audio",
                 total_samples * (int)sizeof(int32_t));
        return NULL;
    }

    /* Allocate int16 output buffer – prefer PSRAM */
    int16_t *pcm = psram_alloc((size_t)total_samples * sizeof(int16_t));
    if (!pcm) {
        ESP_LOGE(TAG, "Cannot allocate %d bytes for PCM audio",
                 total_samples * (int)sizeof(int16_t));
        free(raw);
        return NULL;
    }

    /* Open I2S channel */
    i2s_chan_handle_t rx_chan = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    if (i2s_new_channel(&chan_cfg, NULL, &rx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        free(raw);
        free(pcm);
        return NULL;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)cfg->i2s_sck_io,
            .ws   = (gpio_num_t)cfg->i2s_ws_io,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)cfg->i2s_sd_io,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* INMP441 left channel (L/R pin = GND → left) */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    if (i2s_channel_init_std_mode(rx_chan, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        i2s_del_channel(rx_chan);
        free(raw);
        free(pcm);
        return NULL;
    }
    if (i2s_channel_enable(rx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        i2s_del_channel(rx_chan);
        free(raw);
        free(pcm);
        return NULL;
    }

    ESP_LOGI(TAG, "Recording %d ms (%d samples) …", cfg->record_ms, total_samples);

    /* Read audio in DMA-sized chunks */
    int samples_read = 0;
    while (samples_read < total_samples) {
        int remaining = total_samples - samples_read;
        int want = (remaining < I2S_DMA_BUF_LEN) ? remaining : I2S_DMA_BUF_LEN;
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            rx_chan,
            raw + samples_read,
            (size_t)want * sizeof(int32_t),
            &bytes_read,
            pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "i2s_channel_read error: %s", esp_err_to_name(err));
            break;
        }
        samples_read += (int)(bytes_read / sizeof(int32_t));
    }

    i2s_channel_disable(rx_chan);
    i2s_del_channel(rx_chan);

    ESP_LOGI(TAG, "Captured %d samples", samples_read);

    /*
     * INMP441 outputs 24-bit audio MSB-justified in 32-bit I2S slots.
     * Shift right by 14 bits to place the 18 most-significant audio bits
     * into a signed int16, effectively giving ~18-bit precision clipped
     * to int16 range.
     */
    for (int i = 0; i < samples_read; i++) {
        int32_t s32 = raw[i] >> 14;
        if (s32 >  32767) s32 =  32767;
        if (s32 < -32768) s32 = -32768;
        pcm[i] = (int16_t)s32;
    }

    free(raw);
    *n_samples = samples_read;
    return pcm;
}

/* ── HTTP helpers ────────────────────────────────────────────────── */

/*
 * Reads the full response body from an already-opened esp_http_client
 * into a heap-allocated NUL-terminated string.  Caller must free().
 * Returns NULL on allocation failure or read error.
 */
static char *http_read_response(esp_http_client_handle_t client)
{
    char *buf = malloc(RESPONSE_BUF_SIZE);
    if (!buf) return NULL;

    int total = 0;
    int to_read = RESPONSE_BUF_SIZE - 1;
    while (to_read > 0) {
        int got = esp_http_client_read(client, buf + total, to_read);
        if (got <= 0) break;
        total += got;
        to_read -= got;
    }
    buf[total] = '\0';
    return buf;
}

/* ── Groq Whisper STT ────────────────────────────────────────────── */

/*
 * Uploads 16-bit PCM audio as a WAV multipart/form-data POST to the
 * Groq Whisper API and returns the Italian transcription.
 *
 * @param api_key      Groq API key.
 * @param model        Whisper model name.
 * @param pcm          int16_t PCM samples.
 * @param n_samples    Number of samples.
 * @param out_text     Buffer to receive the transcript.
 * @param out_len      Size of out_text buffer.
 * @return ESP_OK on success.
 */
static esp_err_t groq_whisper(const char *api_key, const char *model,
                               const int16_t *pcm, int n_samples,
                               char *out_text, size_t out_len)
{
    /* ── Build multipart parts ─────────────────────────────────── */
    const char *boundary = MULTIPART_BOUNDARY;
    uint32_t wav_data_bytes = (uint32_t)n_samples * 2;
    uint8_t  wav_hdr[44];
    build_wav_header(wav_hdr, wav_data_bytes, SAMPLE_RATE);

    char file_part_hdr[256];
    int fph_len = snprintf(file_part_hdr, sizeof(file_part_hdr),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary);
    if (fph_len < 0 || fph_len >= (int)sizeof(file_part_hdr)) {
        return ESP_ERR_NO_MEM;
    }

    char model_part[192];
    int mp_len = snprintf(model_part, sizeof(model_part),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s",
        boundary, model);
    if (mp_len < 0 || mp_len >= (int)sizeof(model_part)) {
        return ESP_ERR_NO_MEM;
    }

    char lang_part[128];
    int lp_len = snprintf(lang_part, sizeof(lang_part),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "it",
        boundary);
    if (lp_len < 0 || lp_len >= (int)sizeof(lang_part)) {
        return ESP_ERR_NO_MEM;
    }

    char fmt_part[128];
    int fp2_len = snprintf(fmt_part, sizeof(fmt_part),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
        "json",
        boundary);
    if (fp2_len < 0 || fp2_len >= (int)sizeof(fmt_part)) {
        return ESP_ERR_NO_MEM;
    }

    char terminator[80];
    int term_len = snprintf(terminator, sizeof(terminator),
        "\r\n--%s--\r\n", boundary);
    if (term_len < 0 || term_len >= (int)sizeof(terminator)) {
        return ESP_ERR_NO_MEM;
    }

    int total_len = fph_len + 44 + (int)wav_data_bytes
                  + mp_len + lp_len + fp2_len + term_len;

    /* ── Configure HTTP client ─────────────────────────────────── */
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    char content_type[96];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t http_cfg = {
        .url               = GROQ_WHISPER_URL,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = GROQ_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Whisper: http_client_init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    /* ── Open and stream body ──────────────────────────────────── */
    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Whisper open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_write(client, file_part_hdr, fph_len);
    esp_http_client_write(client, (const char *)wav_hdr, 44);
    esp_http_client_write(client, (const char *)pcm, (int)wav_data_bytes);
    esp_http_client_write(client, model_part, mp_len);
    esp_http_client_write(client, lang_part,  lp_len);
    esp_http_client_write(client, fmt_part,   fp2_len);
    esp_http_client_write(client, terminator, term_len);

    int64_t resp_len = esp_http_client_fetch_headers(client);
    int     status   = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Whisper HTTP %d  content-length=%" PRId64, status, resp_len);

    char *body = http_read_response(client);
    esp_http_client_cleanup(client);

    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Whisper error response: %s", body);
        free(body);
        return ESP_FAIL;
    }

    /* Parse JSON: {"text":"..."} */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "Whisper: JSON parse error");
        return ESP_FAIL;
    }

    cJSON *text_node = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_node) || text_node->valuestring[0] == '\0') {
        ESP_LOGE(TAG, "Whisper: no 'text' field in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(out_text, text_node->valuestring, out_len - 1);
    out_text[out_len - 1] = '\0';
    ESP_LOGI(TAG, "Transcript: \"%s\"", out_text);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Groq LLM NLU ────────────────────────────────────────────────── */

/*
 * Sends the Italian transcript to the Groq chat-completions API
 * with a structured system prompt and returns the JSON command string.
 *
 * @param api_key     Groq API key.
 * @param model       LLM model name.
 * @param transcript  Italian text from Whisper.
 * @param out_cmd     Buffer to receive the JSON command string.
 * @param out_len     Size of out_cmd buffer.
 * @return ESP_OK on success.
 */
static esp_err_t groq_llm(const char *api_key, const char *model,
                           const char *transcript,
                           char *out_cmd, size_t out_len)
{
    /* Escape transcript for JSON embedding – allocate from heap to
     * avoid consuming stack in this already large task.             */
    size_t escaped_size = TRANSCRIPT_MAX * 2 + 1;
    char *escaped = malloc(escaped_size);
    if (!escaped) return ESP_ERR_NO_MEM;

    size_t j = 0;
    for (size_t i = 0; transcript[i] && j + 3 < escaped_size; i++) {
        switch (transcript[i]) {
        case '"':  escaped[j++] = '\\'; escaped[j++] = '"'; break;
        case '\\': escaped[j++] = '\\'; escaped[j++] = '\\'; break;
        case '\n': escaped[j++] = '\\'; escaped[j++] = 'n'; break;
        case '\r': break;
        default:   escaped[j++] = transcript[i]; break;
        }
    }
    escaped[j] = '\0';

    /* Build JSON request body */
    /* System prompt contains apostrophes (e') which are fine in JSON */
    size_t body_size = strlen(SYSTEM_PROMPT) + escaped_size + 512;
    char *body = malloc(body_size);
    if (!body) {
        free(escaped);
        return ESP_ERR_NO_MEM;
    }

    int written = snprintf(body, body_size,
        "{"
          "\"model\":\"%s\","
          "\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"%s\"}"
          "],"
          "\"response_format\":{\"type\":\"json_object\"},"
          "\"max_tokens\":150,"
          "\"temperature\":0.1"
        "}",
        model, SYSTEM_PROMPT, escaped);
    free(escaped);

    if (written < 0 || (size_t)written >= body_size) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    /* Configure HTTP client */
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    esp_http_client_config_t http_cfg = {
        .url               = GROQ_CHAT_URL,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = GROQ_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(body);
        return err;
    }

    esp_http_client_write(client, body, (int)strlen(body));
    free(body);

    int64_t resp_len = esp_http_client_fetch_headers(client);
    int     status   = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "LLM HTTP %d  content-length=%" PRId64, status, resp_len);

    char *resp = http_read_response(client);
    esp_http_client_cleanup(client);
    if (!resp) return ESP_ERR_NO_MEM;

    if (status != 200) {
        ESP_LOGE(TAG, "LLM error response: %s", resp);
        free(resp);
        return ESP_FAIL;
    }

    /* Parse: {"choices":[{"message":{"content":"<json_string>"}},...]} */
    err = ESP_FAIL;
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGE(TAG, "LLM: JSON parse error");
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice  = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            strncpy(out_cmd, content->valuestring, out_len - 1);
            out_cmd[out_len - 1] = '\0';
            ESP_LOGI(TAG, "Command JSON: %s", out_cmd);
            err = ESP_OK;
        }
    }

    cJSON_Delete(root);
    return err;
}

/* ── Command executor ────────────────────────────────────────────── */

/*
 * Parses the JSON command produced by the LLM and calls the appropriate
 * aquarium-controller module API.
 */
static void voice_execute_command(const char *cmd_json)
{
    cJSON *root = cJSON_Parse(cmd_json);
    if (!root) {
        set_result("Errore: JSON comando non valido");
        ESP_LOGE(TAG, "execute: JSON parse failed");
        return;
    }

    cJSON *action_node = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_node)) {
        set_result("Errore: campo 'action' mancante");
        cJSON_Delete(root);
        return;
    }

    const char *action = action_node->valuestring;
    ESP_LOGI(TAG, "Executing action: %s", action);
    char result[RESULT_MAX];

    if (strcmp(action, "relay_set") == 0) {
        cJSON *idx_node = cJSON_GetObjectItem(root, "index");
        cJSON *on_node  = cJSON_GetObjectItem(root, "on");
        int index       = cJSON_IsNumber(idx_node) ? (int)idx_node->valuedouble : 0;
        bool on         = cJSON_IsBool(on_node) ? cJSON_IsTrue(on_node) : false;
        if (index >= 0 && index < RELAY_COUNT) {
            esp_err_t err = relay_controller_set(index, on);
            if (err == ESP_OK) {
                snprintf(result, sizeof(result),
                         "Relè %d %s", index, on ? "acceso" : "spento");
            } else {
                snprintf(result, sizeof(result), "Errore relè %d", index);
            }
        } else {
            snprintf(result, sizeof(result), "Indice relè %d non valido", index);
        }

    } else if (strcmp(action, "led_set") == 0) {
        cJSON *on_node  = cJSON_GetObjectItem(root, "on");
        cJSON *br_node  = cJSON_GetObjectItem(root, "brightness");
        bool on = cJSON_IsBool(on_node) ? cJSON_IsTrue(on_node) : true;
        int br  = cJSON_IsNumber(br_node) ? (int)br_node->valuedouble : 128;
        if (br < 0) br = 0;
        if (br > 255) br = 255;

        esp_err_t err;
        if (!on) {
            err = led_controller_off();
            snprintf(result, sizeof(result), "LED spento");
        } else {
            err = led_controller_set_brightness((uint8_t)br);
            if (err == ESP_OK) err = led_controller_on();
            snprintf(result, sizeof(result), "LED acceso (luminosità %d)", br);
        }
        if (err != ESP_OK) {
            snprintf(result, sizeof(result), "Errore LED");
        }

    } else if (strcmp(action, "led_scene") == 0) {
        cJSON *scene_node = cJSON_GetObjectItem(root, "scene");
        const char *scene = cJSON_IsString(scene_node)
                            ? scene_node->valuestring : "none";
        led_scene_t sc = LED_SCENE_NONE;
        if      (strcmp(scene, "sunrise")   == 0) sc = LED_SCENE_SUNRISE;
        else if (strcmp(scene, "sunset")    == 0) sc = LED_SCENE_SUNSET;
        else if (strcmp(scene, "moonlight") == 0) sc = LED_SCENE_MOONLIGHT;
        else if (strcmp(scene, "storm")     == 0) sc = LED_SCENE_STORM;
        else if (strcmp(scene, "clouds")    == 0) sc = LED_SCENE_CLOUDS;
        led_scenes_start(sc);
        snprintf(result, sizeof(result), "Scena: %s", scene);

    } else if (strcmp(action, "feeding_start") == 0) {
        feeding_mode_start();
        snprintf(result, sizeof(result), "Pausa alimentazione avviata");

    } else if (strcmp(action, "daily_cycle") == 0) {
        cJSON *en_node = cJSON_GetObjectItem(root, "enabled");
        bool en = cJSON_IsBool(en_node) ? cJSON_IsTrue(en_node) : true;
        daily_cycle_config_t dc_cfg = daily_cycle_get_config();
        dc_cfg.enabled = en;
        daily_cycle_set_config(&dc_cfg);
        snprintf(result, sizeof(result),
                 "Ciclo giornaliero %s", en ? "abilitato" : "disabilitato");

    } else if (strcmp(action, "unknown") == 0) {
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        snprintf(result, sizeof(result), "Non capito: %s",
                 cJSON_IsString(reason) ? reason->valuestring : "?");

    } else {
        snprintf(result, sizeof(result), "Azione sconosciuta: %s", action);
    }

    set_result(result);
    ESP_LOGI(TAG, "Result: %s", result);
    cJSON_Delete(root);
}

/* ── Voice pipeline task ─────────────────────────────────────────── */

static void voice_pipeline_task(void *arg)
{
    (void)arg;

    /* Read config snapshot under mutex */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    voice_config_t cfg = s_config;
    xSemaphoreGive(s_mutex);

    /* ── Stage 1: Record audio ─────────────────────────────────── */
    set_status(VOICE_STATUS_RECORDING);
    ESP_LOGI(TAG, "Stage 1: recording audio");

    int n_samples = 0;
    int16_t *pcm = i2s_record(&cfg, &n_samples);
    if (!pcm || n_samples == 0) {
        set_result("Errore: acquisizione audio fallita");
        set_status(VOICE_STATUS_ERROR);
        if (pcm) free(pcm);
        vTaskDelete(NULL);
        return;
    }

    /* ── Stage 2: Whisper transcription ────────────────────────── */
    set_status(VOICE_STATUS_TRANSCRIBING);
    ESP_LOGI(TAG, "Stage 2: Groq Whisper transcription");

    char transcript[TRANSCRIPT_MAX];
    transcript[0] = '\0';
    esp_err_t err = groq_whisper(cfg.groq_api_key, cfg.stt_model,
                                 pcm, n_samples,
                                 transcript, sizeof(transcript));
    free(pcm);

    if (err != ESP_OK || transcript[0] == '\0') {
        set_result("Errore: trascrizione fallita");
        set_status(VOICE_STATUS_ERROR);
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_transcript, transcript, sizeof(s_transcript) - 1);
    s_transcript[sizeof(s_transcript) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    /* ── Stage 3: LLM command parsing ──────────────────────────── */
    set_status(VOICE_STATUS_PROCESSING);
    ESP_LOGI(TAG, "Stage 3: Groq LLM command parsing");

    char cmd_json[COMMAND_MAX];
    cmd_json[0] = '\0';
    err = groq_llm(cfg.groq_api_key, cfg.llm_model, transcript,
                   cmd_json, sizeof(cmd_json));

    if (err != ESP_OK || cmd_json[0] == '\0') {
        set_result("Errore: interpretazione comando fallita");
        set_status(VOICE_STATUS_ERROR);
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_last_command, cmd_json, sizeof(s_last_command) - 1);
    s_last_command[sizeof(s_last_command) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    /* ── Stage 4: Execute command ──────────────────────────────── */
    voice_execute_command(cmd_json);

    set_status(VOICE_STATUS_DONE);
    ESP_LOGI(TAG, "Voice pipeline complete");
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t voice_control_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;   /* already initialised */
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_transcript,   0, sizeof(s_transcript));
    memset(s_last_command, 0, sizeof(s_last_command));
    memset(s_last_result,  0, sizeof(s_last_result));

    nvs_load_config();

    ESP_LOGI(TAG, "Voice control initialised (enabled=%d, key_set=%d)",
             s_config.enabled, s_config.groq_api_key[0] != '\0');
    return ESP_OK;
}

esp_err_t voice_control_start_record(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy    = (s_status >= VOICE_STATUS_RECORDING &&
                    s_status <= VOICE_STATUS_PROCESSING);
    bool enabled = s_config.enabled;
    bool has_key = (s_config.groq_api_key[0] != '\0');
    xSemaphoreGive(s_mutex);

    if (busy) {
        ESP_LOGW(TAG, "Pipeline already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        ESP_LOGW(TAG, "Voice control is disabled");
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_key) {
        ESP_LOGW(TAG, "Groq API key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(
        voice_pipeline_task, "voice_pipeline",
        VOICE_TASK_STACK, NULL,
        VOICE_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice pipeline task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

voice_status_t voice_control_get_status(void)
{
    if (!s_mutex) return VOICE_STATUS_IDLE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    voice_status_t st = s_status;
    xSemaphoreGive(s_mutex);
    return st;
}

void voice_control_get_transcript(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_transcript, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

void voice_control_get_last_command(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_last_command, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

void voice_control_get_last_result(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_last_result, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

voice_config_t voice_control_get_config(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    voice_config_t cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t voice_control_set_config(const voice_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Clamp record duration */
    voice_config_t tmp = *cfg;
    if (tmp.record_ms < 1000)  tmp.record_ms = 1000;
    if (tmp.record_ms > 10000) tmp.record_ms = 10000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = tmp;
    xSemaphoreGive(s_mutex);

    return nvs_save_config(&tmp);
}
