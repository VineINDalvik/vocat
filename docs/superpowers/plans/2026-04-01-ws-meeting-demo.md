# ws_meeting_demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `products/ws_meeting_demo` — replace VolcEngine RTC with WebSocket-based meeting transcription (single-direction) and host Q&A (bidirectional) backed by ClarityX Voice API.

**Architecture:** 8 new/modified C files coordinated by `ws_session.c` state machine (IDLE→MEETING→HOST). Audio hardware layer reused verbatim from `rtc_meeting_demo`, only player path changes (Opus→raw PCM). MP3 answer audio decoded by minimp3 in a dedicated task, resampled 24kHz→16kHz before writing to I2S.

**Tech Stack:** ESP-IDF 5.5, esp_websocket_client, esp_http_client, cJSON, mbedtls/base64, minimp3 (single-header MIT), LVGL 8, esp_codec_dev (ES8311 + ES7210), FreeRTOS queues/ring buffers, PSRAM

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `products/ws_meeting_demo/CMakeLists.txt` | Modify | Rename project, remove volc_rtc_engine_lite |
| `main/CMakeLists.txt` | Modify | Remove volc_rtc_engine_lite, add esp_websocket_client |
| `main/Kconfig.projbuild` | Replace | WiFi + API URL + VAD params |
| `main/main.c` | Replace | Remove RTC references, init pipeline_ws + ws_session |
| `main/pipeline_ws.h/.c` | New (from pipeline_gmf) | Hardware I/O: recorder unchanged, player accepts raw PCM instead of Opus |
| `main/api_client.h/.c` | New | HTTP POST /api/session and /api/session/{id}/end |
| `main/vad.h/.c` | New | 3-state energy VAD (WAITING→SPEECH→SILENCE_AFTER_SPEECH) |
| `main/transcribe_ws.h/.c` | New | WS client + feed task: 100ms PCM frames → base64 JSON |
| `main/host_ws.h/.c` | New | WS client + VAD + feed task + message dispatch (transcription/answer_text/answer_audio/done/error) |
| `main/mp3_player.h/.c` | New | FreeRTOS queue + minimp3 decode + 24kHz→16kHz resample → pipeline_ws_player_write_pcm |
| `main/ws_session.h/.c` | New | State machine + cmd_task + shared globals (g_mic_muted) + lv_async_call UI updates |
| `main/ui_meeting.h/.c` | Modify | Add mic dot (12px green, 500ms blink in MEETING), wire button callbacks to ws_session |
| `main/minimp3.h` | Download | Single-header MP3 decoder (lieff/minimp3, MIT) |
| `sdkconfig.defaults` | Modify | Add CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y, remove RTC options |
| `wifi_init.h/.c` | Keep | Verbatim copy — no changes needed |

---

## Task 1 — Copy directory + clean up dependencies

**Status:** ✅ Done

**Files:**
- Modify: `products/ws_meeting_demo/CMakeLists.txt`
- Modify: `main/CMakeLists.txt`
- Modify: `main/Kconfig.projbuild`
- Replace: `main/main.c`
- Modify: `sdkconfig.defaults`
- Delete: `main/rtc_session.c/h`, `main/bot_client.c/h`, `components/volc_rtc_engine_lite/`, `server/`

- [x] **Step 1: Copy rtc_meeting_demo → ws_meeting_demo**

```bash
cp -r products/rtc_meeting_demo products/ws_meeting_demo
```

- [x] **Step 2: Remove RTC-specific files**

```bash
rm -rf products/ws_meeting_demo/components/volc_rtc_engine_lite
rm -rf products/ws_meeting_demo/server
rm products/ws_meeting_demo/main/rtc_session.c products/ws_meeting_demo/main/rtc_session.h
rm products/ws_meeting_demo/main/bot_client.c products/ws_meeting_demo/main/bot_client.h
```

- [x] **Step 3: Rename project in root CMakeLists.txt**

Change `project(rtc_meeting_demo)` → `project(ws_meeting_demo)`

- [x] **Step 4: Update main/CMakeLists.txt — swap volc for websocket**

Replace `volc_rtc_engine_lite` with `esp_websocket_client`. Remove `espressif__esp_audio_codec`.

- [ ] **Step 5: Replace Kconfig.projbuild**

```kconfig
menu "WS Meeting Demo Configuration"

    config MEETING_WIFI_SSID
        string "WiFi SSID"
        default "myssid"

    config MEETING_WIFI_PASSWORD
        string "WiFi Password"
        default "mypassword"

    config WS_MEETING_API_BASE_URL
        string "ClarityX Voice API base URL"
        default "https://ailabs.mayfair-inc.net/voice-api"

    config WS_MEETING_TOPIC
        string "Default meeting topic"
        default "产品评审会"

    config VAD_ENERGY_THRESHOLD
        int "VAD energy threshold (RMS int16)"
        default 800
        range 200 5000

    config VAD_SPEECH_MIN_MS
        int "Minimum speech duration (ms)"
        default 300
        range 100 2000

    config VAD_SILENCE_MS
        int "Silence duration (ms) to trigger end_of_speech"
        default 600
        range 200 3000

endmenu
```

- [ ] **Step 6: Update sdkconfig.defaults** — add `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` (already present in rtc version ✓), remove any RTC-specific options.

- [ ] **Step 7: Replace main.c**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp_vocat.h"
#include "wifi_init.h"
#include "ui_meeting.h"
#include "pipeline_ws.h"
#include "ws_session.h"

esp_err_t bsp_display_backlight_on(void);
static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ws_meeting_demo starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lv_disp_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed — halting");
        return;
    }
    bsp_display_lock(0);
    ui_meeting_create();
    bsp_display_unlock();
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display ready, free heap: %lu", esp_get_free_heap_size());

    ESP_ERROR_CHECK(pipeline_ws_hw_init());

    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — voice features disabled");
    }
    // LVGL runs its own task; app_main can return.
}
```

- [ ] **Step 8: Build**

```bash
cd products/ws_meeting_demo
idf.py set-target esp32s3
idf.py build
```

Expected: zero errors (warnings OK). No `byte_rtc_` / `volc_rtc_` undefined symbols.

- [ ] **Step 9: Flash + verify**

```bash
idf.py -p $PORT flash monitor
```

Expected serial log:
```
I (...) main: ws_meeting_demo starting
I (...) pipeline_ws: [OK] hw_init: I2S ready 16000Hz 32bit 2ch
I (...) wifi: connected, ip=...
```
No `Guru Meditation Error`.

- [ ] **Step 10: Commit**

```bash
git add products/ws_meeting_demo
git commit -m "feat: add ws_meeting_demo skeleton (Task 1)"
```

---

## Task 2 — pipeline_ws.c: Audio hardware I/O

**Status:** ✅ Done (pipeline_ws.h/c implemented, pipeline_gmf.h/c deleted, build verified)

**Files:**
- Create: `main/pipeline_ws.h`
- Create: `main/pipeline_ws.c`
- Delete: `main/pipeline_gmf.h`, `main/pipeline_gmf.c`

**Changes from pipeline_gmf:**
- Remove: `#include "esp_opus_dec.h"`, `espressif__esp_audio_codec` dep, `s_opus_dec`, all opus decode code
- Change: ring buffer type `RINGBUF_TYPE_NOSPLIT` → `RINGBUF_TYPE_BYTEBUF`, size 16000 bytes
- Change: `player_task` reads PCM bytes from ring buffer → `conv_16m_to_32s()` → `esp_codec_dev_write()`
- Change: recorder raw buffer → heap alloc on PSRAM (up to 3200 samples = 25600 raw bytes)
- Add: `pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)`
- Rename: all `pipeline_gmf_*` → `pipeline_ws_*`
- Add OK/FAIL log markers

- [ ] **Step 1: Create pipeline_ws.h**

```c
#pragma once
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PIPELINE_FRAME_BYTES  640   // 16kHz 16bit mono, 20ms

esp_err_t pipeline_ws_hw_init(void);
esp_err_t pipeline_ws_hw_deinit(void);

esp_err_t pipeline_ws_recorder_open(void);
int       pipeline_ws_recorder_read(void *buf, size_t size);
esp_err_t pipeline_ws_recorder_close(void);

esp_err_t pipeline_ws_player_open(void);
esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames);
esp_err_t pipeline_ws_player_close(void);

esp_err_t pipeline_ws_set_volume(int volume);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create pipeline_ws.c** (copy pipeline_gmf.c as base, then apply these diffs)

Remove from includes:
```c
// REMOVE: #include "esp_opus_dec.h"
```

Change ring buffer constants:
```c
#define PLAY_RB_SIZE      (16000)      // 500ms of 16kHz 16bit mono PCM
#define PLAY_TASK_STACK   (8 * 1024)   // No Opus decoder — smaller stack ok
```

Remove from state vars:
```c
// REMOVE: static void *s_opus_dec = NULL;
```

Change recorder to use PSRAM heap buffer:
```c
static int32_t *s_rec_raw_buf  = NULL;
#define REC_RAW_BUF_FRAMES 3200  // max 100ms at 16kHz
```

In `pipeline_ws_recorder_open()`:
```c
esp_err_t pipeline_ws_recorder_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited, ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_rec_open, ESP_ERR_INVALID_STATE, TAG, "Recorder already open");
    s_rec_raw_buf = heap_caps_malloc(REC_RAW_BUF_FRAMES * 2 * sizeof(int32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_rec_raw_buf, ESP_ERR_NO_MEM, TAG, "rec raw buf alloc failed");
    s_rec_open = true;
    ESP_LOGI(TAG, "[OK] recorder opened");
    return ESP_OK;
}
```

In `pipeline_ws_recorder_read()`:
```c
int pipeline_ws_recorder_read(void *buf, size_t size)
{
    if (!s_rec_open || !s_rec_dev || !s_rec_raw_buf) return -1;
    int out_frames = (int)(size / sizeof(int16_t));
    if (out_frames > REC_RAW_BUF_FRAMES) {
        ESP_LOGE(TAG, "recorder_read: requested %d frames > max %d", out_frames, REC_RAW_BUF_FRAMES);
        return -1;
    }
    size_t raw_bytes = (size_t)out_frames * 2 * sizeof(int32_t);
    int ret = esp_codec_dev_read(s_rec_dev, s_rec_raw_buf, (int)raw_bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec_dev_read error %d", ret);
        return -1;
    }
    conv_32s_to_16m(s_rec_raw_buf, (int16_t *)buf, out_frames);
    return (int)size;
}
```

In `pipeline_ws_recorder_close()`:
```c
esp_err_t pipeline_ws_recorder_close(void)
{
    s_rec_open = false;
    if (s_rec_raw_buf) { heap_caps_free(s_rec_raw_buf); s_rec_raw_buf = NULL; }
    ESP_LOGI(TAG, "[OK] recorder closed");
    return ESP_OK;
}
```

Replace `player_task` (new version, no Opus):
```c
static void player_task(void *arg)
{
    // 20ms output frame: 320 samples × 2ch × 4bytes = 2560 bytes
    int32_t out_buf[320 * 2];

    while (s_play_task_run) {
        size_t rx_len = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(
            s_play_rb, &rx_len, pdMS_TO_TICKS(20), 640);
        if (pcm == NULL || rx_len == 0) {
            memset(out_buf, 0, sizeof(out_buf));
            esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
            continue;
        }
        int frames = (int)(rx_len / sizeof(int16_t));
        conv_16m_to_32s(pcm, out_buf, frames);
        vRingbufferReturnItem(s_play_rb, pcm);
        int write_bytes = frames * 2 * (int)sizeof(int32_t);
        if (write_bytes < (int)sizeof(out_buf)) {
            memset((uint8_t *)out_buf + write_bytes, 0, sizeof(out_buf) - write_bytes);
        }
        esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
    }
    if (s_play_done_sem) xSemaphoreGive(s_play_done_sem);
    vTaskDelete(NULL);
}
```

Replace `pipeline_ws_player_open()` (remove Opus init):
```c
esp_err_t pipeline_ws_player_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited, ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_play_open, ESP_ERR_INVALID_STATE, TAG, "Player already open");

    s_play_rb = xRingbufferCreate(PLAY_RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(s_play_rb, ESP_ERR_NO_MEM, TAG, "ring buffer alloc failed");

    s_play_done_sem = xSemaphoreCreateBinary();
    if (!s_play_done_sem) { vRingbufferDelete(s_play_rb); s_play_rb = NULL; return ESP_ERR_NO_MEM; }

    s_play_task_run = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        player_task, "play_task", PLAY_TASK_STACK, NULL,
        PLAY_TASK_PRIORITY, &s_play_task, PLAY_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        vRingbufferDelete(s_play_rb); s_play_rb = NULL;
        vSemaphoreDelete(s_play_done_sem); s_play_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_play_open = true;
    ESP_LOGI(TAG, "[OK] player opened (PCM ring buffer)");
    return ESP_OK;
}
```

Add `pipeline_ws_player_write_pcm()`:
```c
esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)
{
    if (!s_play_open || !s_play_rb) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xRingbufferSend(s_play_rb, pcm,
                                     frames * sizeof(int16_t), pdMS_TO_TICKS(5));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
```

Update `pipeline_ws_player_close()` — remove Opus close:
```c
esp_err_t pipeline_ws_player_close(void)
{
    if (!s_play_open) return ESP_OK;
    s_play_task_run = false;
    if (s_play_done_sem) {
        xSemaphoreTake(s_play_done_sem, pdMS_TO_TICKS(3000));
        vSemaphoreDelete(s_play_done_sem); s_play_done_sem = NULL;
    }
    s_play_task = NULL;
    if (s_play_rb) { vRingbufferDelete(s_play_rb); s_play_rb = NULL; }
    s_play_open = false;
    ESP_LOGI(TAG, "[OK] player closed");
    return ESP_OK;
}
```

Update `hw_init` log:
```c
ESP_LOGI(TAG, "[OK] hw_init: I2S ready %dHz %dbit %dch",
         CODEC_SAMPLE_RATE, CODEC_BITS, CODEC_CHANNELS);
// ...
ESP_LOGI(TAG, "[OK] ES8311 (DAC) ready");
// ...
ESP_LOGI(TAG, "[OK] ES7210 (ADC) ready, gain=42dB");
```

- [ ] **Step 3: Delete pipeline_gmf.c and pipeline_gmf.h from ws_meeting_demo**

```bash
rm products/ws_meeting_demo/main/pipeline_gmf.c products/ws_meeting_demo/main/pipeline_gmf.h
```

- [ ] **Step 4: Build**

```bash
cd products/ws_meeting_demo && idf.py build
```

Expected: no undefined symbols, no Opus-related errors.

- [ ] **Step 5: Flash + verify recorder RMS**

Add a quick test loop in main.c temporarily (remove after verification):
```c
// TEMP: recorder test — remove after Task 2 verified
pipeline_ws_recorder_open();
pipeline_ws_player_open();
int16_t buf[320]; // 20ms
for (int i = 0; i < 250; i++) { // 5 seconds
    pipeline_ws_recorder_read(buf, sizeof(buf));
    if (i % 50 == 0) {
        int32_t sum = 0;
        for (int j = 0; j < 320; j++) sum += (buf[j] < 0 ? -buf[j] : buf[j]);
        ESP_LOGI("test", "rms=#%d val=%ld", i, (long)(sum / 320));
    }
}
pipeline_ws_recorder_close();
pipeline_ws_player_close();
```

Expected serial log:
```
I (...) pipeline_ws: [OK] hw_init: I2S ready 16000Hz 32bit 2ch
I (...) pipeline_ws: [OK] ES8311 (DAC) ready
I (...) pipeline_ws: [OK] ES7210 (ADC) ready, gain=42dB
I (...) pipeline_ws: [OK] recorder opened
I (...) pipeline_ws: [OK] player opened (PCM ring buffer)
I (...) test: rms=#0 val=<N>   ← silent: N < 200; speaking: N > 1000
```

- [ ] **Step 6: Remove temp test code from main.c, commit**

```bash
git add products/ws_meeting_demo/main/pipeline_ws.h products/ws_meeting_demo/main/pipeline_ws.c products/ws_meeting_demo/main/main.c
git commit -m "feat: add pipeline_ws (raw PCM player, no Opus) (Task 2)"
```

---

## Task 3 — api_client.c: HTTP session management

**Status:** ✅ Done

**Files:**
- Create: `main/api_client.h`
- Create: `main/api_client.c`

- [ ] **Step 1: Create api_client.h**

```c
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// POST /api/session → session_id written to out_session_id (max len bytes)
esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len);

// POST /api/session/{session_id}/end
esp_err_t api_client_end_session(const char *session_id);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create api_client.c**

```c
#include "api_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "api_client";
#define RESP_BUF_SIZE 256

esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session", CONFIG_WS_MEETING_API_BASE_URL);

    char body[128];
    snprintf(body, sizeof(body), "{\"topic\":\"%s\"}",
             topic ? topic : CONFIG_WS_MEETING_TOPIC);

    char resp_buf[RESP_BUF_SIZE] = {0};
    int  resp_len = 0;

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 10000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        resp_len   = esp_http_client_get_content_length(client);
        if (status == 200 && resp_len > 0 && resp_len < RESP_BUF_SIZE) {
            esp_http_client_read(client, resp_buf, resp_len);
        } else {
            ESP_LOGE(TAG, "[FAIL] create_session http_status=%d", status);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "[FAIL] create_session http_err=%d", (int)err);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_http_client_cleanup(client);

    // Parse session_id
    cJSON *root = cJSON_ParseWithLength(resp_buf, (size_t)resp_len);
    if (!root) { ESP_LOGE(TAG, "[FAIL] create_session: JSON parse error"); return ESP_FAIL; }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsString(id) || !id->valuestring) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "[FAIL] create_session: no session_id in response");
        return ESP_FAIL;
    }
    strlcpy(out_session_id, id->valuestring, len);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "[OK] session created: %s", out_session_id);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t api_client_end_session(const char *session_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session/%s/end",
             CONFIG_WS_MEETING_API_BASE_URL, session_id);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_err_t result = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        ESP_LOGI(TAG, "[OK] session ended: %s", session_id);
        result = ESP_OK;
    } else {
        ESP_LOGE(TAG, "[FAIL] end_session http_err=%d status=%d",
                 (int)err, esp_http_client_get_status_code(client));
    }
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
    return result;
}
```

- [ ] **Step 3: Add temp test call in main.c (after wifi_init_sta succeeds)**

```c
// TEMP test — remove after Task 3 verified
#include "api_client.h"
// ...
char sess[64] = {0};
if (api_client_create_session(NULL, sess, sizeof(sess)) == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    api_client_end_session(sess);
}
```

- [ ] **Step 4: Build + flash + verify**

Expected:
```
I (...) api_client: [OK] session created: sess-<ID>
I (...) api_client: heap_free=<N>
I (...) api_client: [OK] session ended: sess-<ID>
```

- [ ] **Step 5: Remove temp test, commit**

```bash
git add products/ws_meeting_demo/main/api_client.h products/ws_meeting_demo/main/api_client.c products/ws_meeting_demo/main/main.c
git commit -m "feat: add api_client HTTP session management (Task 3)"
```

---

## Task 4 — transcribe_ws.c: Meeting mode WebSocket

**Status:** ✅ Done

**Files:**
- Create: `main/transcribe_ws.h`
- Create: `main/transcribe_ws.c`

- [ ] **Step 1: Create transcribe_ws.h**

```c
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t transcribe_ws_connect(const char *session_id);
esp_err_t transcribe_ws_send_audio(const void *pcm, size_t size);
esp_err_t transcribe_ws_send_end(void);
esp_err_t transcribe_ws_disconnect(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create transcribe_ws.c**

```c
#include "transcribe_ws.h"
#include "pipeline_ws.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "transcribe_ws";

#define FEED_FRAME_BYTES  3200   // 100ms at 16kHz 16bit mono
#define B64_OUT_SIZE      ((FEED_FRAME_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE     (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_feed_run = false;
static TaskHandle_t  s_feed_task_handle = NULL;

static void build_ws_uri(char *out, size_t len, const char *path, const char *session_id)
{
    const char *base = CONFIG_WS_MEETING_API_BASE_URL;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(out, len, "wss://%s%s/%s", host, path, session_id);
}

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");
    static uint8_t pcm_buf[FEED_FRAME_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char json_buf[JSON_BUF_SIZE];
    uint32_t frame_count = 0;

    while (s_feed_run) {
        int got = pipeline_ws_recorder_read(pcm_buf, FEED_FRAME_BYTES);
        if (got != FEED_FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (!esp_websocket_client_is_connected(s_ws)) {
            vTaskDelay(pdMS_TO_TICKS(50)); continue;
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                              pcm_buf, FEED_FRAME_BYTES);
        b64_buf[b64_len] = '\0';
        int jlen = snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
        esp_websocket_client_send_text(s_ws, json_buf, jlen, pdMS_TO_TICKS(100));
        frame_count++;
        if (frame_count % 100 == 0) {
            ESP_LOGI(TAG, "sent #%lu frames total", (unsigned long)frame_count);
        }
    }
    ESP_LOGI(TAG, "[OK] feed task stopped, heap_free=%lu", esp_get_free_heap_size());
    s_feed_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[OK] WS connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        break;
    default: break;
    }
}

esp_err_t transcribe_ws_connect(const char *session_id)
{
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/transcribe", session_id);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri         = uri,
        .buffer_size = 8192,
        .task_stack  = 8192,
        .task_prio   = 5,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);

    // Wait up to 5s for connection
    for (int i = 0; i < 50; i++) {
        if (esp_websocket_client_is_connected(s_ws)) {
            ESP_LOGI(TAG, "[OK] connected wss://.../ws/transcribe/%s", session_id);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGE(TAG, "[FAIL] connect timeout");
        return ESP_FAIL;
    }

    // Start feed task
    pipeline_ws_recorder_open();
    s_feed_run = true;
    xTaskCreatePinnedToCoreWithCaps(
        feed_task, "transcribe_feed", 8 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}

esp_err_t transcribe_ws_send_end(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;
    const char *msg = "{\"type\":\"end\"}";
    esp_websocket_client_send_text(s_ws, msg, (int)strlen(msg), pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "[OK] sent end signal");
    return ESP_OK;
}

esp_err_t transcribe_ws_disconnect(void)
{
    s_feed_run = false;
    // Wait for feed task to exit
    for (int i = 0; i < 30 && s_feed_task_handle; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pipeline_ws_recorder_close();
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    ESP_LOGI(TAG, "[OK] disconnected");
    return ESP_OK;
}
```

- [ ] **Step 3: Add temp test in main.c after WiFi connects**

```c
// TEMP: test transcribe_ws — remove after Task 4 verified
#include "transcribe_ws.h"
char sess[64] = {0};
api_client_create_session(NULL, sess, sizeof(sess));
transcribe_ws_connect(sess);
vTaskDelay(pdMS_TO_TICKS(30000));  // 30s recording
transcribe_ws_send_end();
transcribe_ws_disconnect();
api_client_end_session(sess);
```

- [ ] **Step 4: Build + flash + verify**

Expected:
```
I (...) transcribe_ws: [OK] connected wss://.../ws/transcribe/sess-<ID>
I (...) transcribe_ws: [OK] feed task started
I (...) transcribe_ws: sent #100 frames total
I (...) transcribe_ws: [OK] sent end signal
I (...) transcribe_ws: [OK] feed task stopped, heap_free=<N>
I (...) transcribe_ws: [OK] disconnected
```

- [ ] **Step 5: Remove temp test, commit**

```bash
git add products/ws_meeting_demo/main/transcribe_ws.h products/ws_meeting_demo/main/transcribe_ws.c
git commit -m "feat: add transcribe_ws meeting mode WebSocket (Task 4)"
```

---

## Task 5 — vad.c: Energy VAD

**Status:** ✅ Done

**Files:**
- Create: `main/vad.h`
- Create: `main/vad.c`

- [ ] **Step 1: Create vad.h**

```c
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VAD_STATE_WAITING,
    VAD_STATE_SPEECH,
    VAD_STATE_SILENCE_AFTER_SPEECH,
} vad_state_t;

typedef struct {
    vad_state_t state;
    int speech_frames;
    int silence_frames;
} vad_ctx_t;

typedef enum {
    VAD_RESULT_CONTINUE,
    VAD_RESULT_END_OF_SPEECH,
} vad_result_t;

void        vad_reset(vad_ctx_t *ctx);
vad_result_t vad_process_frame(vad_ctx_t *ctx, const int16_t *pcm, int frames);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create vad.c**

```c
#include "vad.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "vad";

#define FRAME_MS         20
#define SPEECH_START_FRAMES  2   // 40ms consecutive to start speech
#define SPEECH_MIN_FRAMES   (CONFIG_VAD_SPEECH_MIN_MS / FRAME_MS)
#define SILENCE_FRAMES      (CONFIG_VAD_SILENCE_MS / FRAME_MS)

static int compute_rms(const int16_t *pcm, int frames)
{
    int64_t sum = 0;
    for (int i = 0; i < frames; i++) {
        int32_t s = pcm[i];
        sum += s * s;
    }
    return (int)__builtin_sqrt((double)(sum / frames));
}

void vad_reset(vad_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ESP_LOGI(TAG, "reset");
}

vad_result_t vad_process_frame(vad_ctx_t *ctx, const int16_t *pcm, int frames)
{
    int rms = compute_rms(pcm, frames);
    int threshold = CONFIG_VAD_ENERGY_THRESHOLD;

    switch (ctx->state) {
    case VAD_STATE_WAITING:
        if (rms > threshold) {
            ctx->speech_frames++;
            if (ctx->speech_frames >= SPEECH_START_FRAMES) {
                ctx->state = VAD_STATE_SPEECH;
                ESP_LOGI(TAG, "speech start (rms=%d)", rms);
            }
        } else {
            ctx->speech_frames = 0;
        }
        break;

    case VAD_STATE_SPEECH:
        ctx->speech_frames++;
        if (rms <= threshold) {
            ctx->state = VAD_STATE_SILENCE_AFTER_SPEECH;
            ctx->silence_frames = 1;
            ESP_LOGI(TAG, "silence start after %dms speech",
                     ctx->speech_frames * FRAME_MS);
        }
        break;

    case VAD_STATE_SILENCE_AFTER_SPEECH:
        if (rms > threshold) {
            // Back to speech
            ctx->state = VAD_STATE_SPEECH;
            ctx->silence_frames = 0;
        } else {
            ctx->silence_frames++;
            if (ctx->silence_frames >= SILENCE_FRAMES) {
                int speech_ms  = ctx->speech_frames * FRAME_MS;
                int silence_ms = ctx->silence_frames * FRAME_MS;
                if (ctx->speech_frames >= SPEECH_MIN_FRAMES) {
                    ESP_LOGI(TAG, "[OK] end_of_speech triggered (speech=%dms silence=%dms)",
                             speech_ms, silence_ms);
                    return VAD_RESULT_END_OF_SPEECH;
                } else {
                    ESP_LOGI(TAG, "noise filtered (%dms < speech_min_ms)", speech_ms);
                    vad_reset(ctx);
                }
            }
        }
        break;
    }
    return VAD_RESULT_CONTINUE;
}
```

- [ ] **Step 3: Temp test in main.c**

```c
// TEMP: VAD unit test (synthetic data, no flash needed — compile test)
#include "vad.h"
vad_ctx_t vad = {0};
// Simulate 1s speech (1000ms / 20ms = 50 frames of high energy)
int16_t loud[320]; for(int i=0;i<320;i++) loud[i] = 2000;
int16_t quiet[320]; memset(quiet, 0, sizeof(quiet));
for (int i = 0; i < 50; i++) vad_process_frame(&vad, loud, 320);
// Simulate 700ms silence (35 frames)
vad_result_t r = VAD_RESULT_CONTINUE;
for (int i = 0; i < 35; i++) r = vad_process_frame(&vad, quiet, 320);
assert(r == VAD_RESULT_END_OF_SPEECH);
ESP_LOGI("vad_test", "PASS: end_of_speech triggered correctly");
```

- [ ] **Step 4: Build + flash + verify**

Expected:
```
I (...) vad: speech start (rms=2000)
I (...) vad: silence start after 1000ms speech
I (...) vad: [OK] end_of_speech triggered (speech=1000ms silence=600ms)
I (...) vad_test: PASS: end_of_speech triggered correctly
```

- [ ] **Step 5: Remove temp test, commit**

```bash
git add products/ws_meeting_demo/main/vad.h products/ws_meeting_demo/main/vad.c
git commit -m "feat: add energy VAD 3-state machine (Task 5)"
```

---

## Task 6 — host_ws.c: Host mode Q&A WebSocket

**Status:** ✅ Done

**Files:**
- Create: `main/host_ws.h`
- Create: `main/host_ws.c`

- [ ] **Step 1: Create host_ws.h**

```c
#pragma once
#include "esp_err.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*host_ws_msg_cb_t)(const char *type, cJSON *root, void *ctx);

esp_err_t host_ws_connect(const char *session_id);
esp_err_t host_ws_disconnect(void);
void      host_ws_set_callback(host_ws_msg_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create host_ws.c**

```c
#include "host_ws.h"
#include "pipeline_ws.h"
#include "vad.h"
#include "ws_session.h"   // g_mic_muted
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "host_ws";

#define FRAME_BYTES   640   // 20ms
#define B64_OUT_SIZE  ((FRAME_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_feed_run = false;
static TaskHandle_t  s_feed_task_handle = NULL;
static host_ws_msg_cb_t s_msg_cb = NULL;
static void *s_msg_cb_ctx = NULL;
static volatile bool s_got_done = false;
static volatile bool s_sending = false;

void host_ws_set_callback(host_ws_msg_cb_t cb, void *ctx)
{
    s_msg_cb     = cb;
    s_msg_cb_ctx = ctx;
}

static void build_ws_uri(char *out, size_t len, const char *path, const char *session_id)
{
    const char *base = CONFIG_WS_MEETING_API_BASE_URL;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(out, len, "wss://%s%s/%s", host, path, session_id);
}

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");
    static uint8_t pcm_buf[FRAME_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char json_buf[JSON_BUF_SIZE];
    vad_ctx_t vad = {0};
    uint32_t audio_frame_count = 0;

    while (s_feed_run) {
        int got = pipeline_ws_recorder_read(pcm_buf, FRAME_BYTES);
        if (got != FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (!esp_websocket_client_is_connected(s_ws)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (g_mic_muted) {
            vad_reset(&vad);
            continue;
        }

        vad_result_t result = vad_process_frame(&vad, (int16_t *)pcm_buf, FRAME_BYTES / 2);

        if (s_sending && vad.state == VAD_STATE_SPEECH) {
            size_t b64_len = 0;
            mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len, pcm_buf, FRAME_BYTES);
            b64_buf[b64_len] = '\0';
            int jlen = snprintf(json_buf, sizeof(json_buf),
                                "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
            esp_websocket_client_send_text(s_ws, json_buf, jlen, pdMS_TO_TICKS(100));
            audio_frame_count++;
            if (audio_frame_count % 50 == 0) {
                ESP_LOGI(TAG, "sending audio frame #%lu", (unsigned long)audio_frame_count);
            }
        }

        if (result == VAD_RESULT_END_OF_SPEECH) {
            const char *eos = "{\"type\":\"end_of_speech\"}";
            esp_websocket_client_send_text(s_ws, eos, (int)strlen(eos), pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[OK] sent end_of_speech");
            s_sending = false;
        }

        if (s_got_done) {
            s_got_done = false;
            s_sending  = true;
            vad_reset(&vad);
            audio_frame_count = 0;
            ESP_LOGI(TAG, "recv done, resuming VAD");
        }
    }
    s_feed_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (d->op_code != 1 || d->data_len <= 0) return; // text frames only

        // Null-terminate safely
        char *tmp = malloc(d->data_len + 1);
        if (!tmp) return;
        memcpy(tmp, d->data_ptr, d->data_len);
        tmp[d->data_len] = '\0';

        cJSON *root = cJSON_Parse(tmp);
        free(tmp);
        if (!root) return;

        cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (!cJSON_IsString(type_j)) { cJSON_Delete(root); return; }
        const char *type = type_j->valuestring;

        ESP_LOGI(TAG, "recv %s", type);

        if (strcmp(type, "done") == 0) {
            s_got_done = true;
        }

        if (s_msg_cb) {
            s_msg_cb(type, root, s_msg_cb_ctx);
        }
        cJSON_Delete(root);

    } else if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "[OK] WS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "WS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "WS error");
    }
}

esp_err_t host_ws_connect(const char *session_id)
{
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/host", session_id);

    esp_websocket_client_config_t cfg = {
        .uri         = uri,
        .buffer_size = 16384,
        .task_stack  = 8192,
        .task_prio   = 5,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);

    for (int i = 0; i < 50; i++) {
        if (esp_websocket_client_is_connected(s_ws)) {
            ESP_LOGI(TAG, "[OK] connected wss://.../ws/host/%s", session_id);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGE(TAG, "[FAIL] connect timeout");
        return ESP_FAIL;
    }

    pipeline_ws_recorder_open();
    s_feed_run = true;
    s_sending  = true;
    s_got_done = false;
    xTaskCreatePinnedToCoreWithCaps(
        feed_task, "host_feed", 8 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}

esp_err_t host_ws_disconnect(void)
{
    s_feed_run = false;
    for (int i = 0; i < 30 && s_feed_task_handle; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pipeline_ws_recorder_close();
    if (s_ws) {
        if (esp_websocket_client_is_connected(s_ws)) {
            const char *stop = "{\"type\":\"stop\"}";
            esp_websocket_client_send_text(s_ws, stop, (int)strlen(stop), pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[OK] sent stop");
        }
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    ESP_LOGI(TAG, "[OK] disconnected, heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}
```

- [ ] **Step 3: Build + flash — test live host mode (skip temp main.c test, test via ws_session in Task 8)**

```bash
cd products/ws_meeting_demo && idf.py build
```

Expected: zero errors.

- [ ] **Step 4: Commit**

```bash
git add products/ws_meeting_demo/main/host_ws.h products/ws_meeting_demo/main/host_ws.c products/ws_meeting_demo/main/vad.h products/ws_meeting_demo/main/vad.c
git commit -m "feat: add host_ws Q&A WebSocket + VAD integration (Tasks 5+6)"
```

---

## Task 7 — mp3_player.c: Streaming MP3 playback

**Status:** ✅ Done

**Files:**
- Download: `main/minimp3.h`
- Create: `main/mp3_player.h`
- Create: `main/mp3_player.c`

- [ ] **Step 1: Download minimp3.h**

```bash
curl -L https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h \
     -o products/ws_meeting_demo/main/minimp3.h
```

- [ ] **Step 2: Create mp3_player.h**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mp3_player_open(void);
esp_err_t mp3_player_enqueue(const uint8_t *mp3, size_t len);
esp_err_t mp3_player_flush_and_wait(void);
esp_err_t mp3_player_close(void);
bool      mp3_player_is_busy(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create mp3_player.c**

```c
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "mp3_player.h"
#include "pipeline_ws.h"
#include "ws_session.h"   // g_mic_muted
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "mp3_player";

#define QUEUE_DEPTH   8

typedef struct {
    uint8_t *data;
    size_t   len;
} mp3_chunk_t;

static QueueHandle_t s_queue      = NULL;
static TaskHandle_t  s_task       = NULL;
static volatile bool s_task_run   = false;
static volatile bool s_busy       = false;
static SemaphoreHandle_t s_done_sem = NULL;

// Resample 24kHz mono → 16kHz mono (ratio 2/3)
static int resample_24k_to_16k(const int16_t *in, int in_n, int16_t *out)
{
    int out_n = (in_n * 2) / 3;
    for (int i = 0; i < out_n; i++) {
        int p    = i * 3 / 2;
        int frac = (i * 3) % 2;
        if (frac == 0 || p + 1 >= in_n) {
            out[i] = in[p];
        } else {
            out[i] = (int16_t)(((int)in[p] + in[p + 1]) / 2);
        }
    }
    return out_n;
}

static void mp3_play_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] task started");
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    // Allocate PCM buffers on PSRAM
    int16_t *pcm_raw = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_resampled = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    uint32_t chunk_count = 0;

    while (s_task_run) {
        mp3_chunk_t chunk;
        if (xQueueReceive(s_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        chunk_count++;
        ESP_LOGI(TAG, "[OK] playing chunk #%lu len=%u",
                 (unsigned long)chunk_count, (unsigned)chunk.len);

        if (!s_busy) {
            s_busy = true;
            g_mic_muted = true;
            ESP_LOGI(TAG, "mic MUTED (playback started)");
        }

        // Decode all MP3 frames in this chunk
        int offset = 0;
        uint32_t mp3_frame_count = 0;
        uint32_t pcm_frame_total = 0;

        while (offset < (int)chunk.len) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&mp3d,
                            chunk.data + offset, (int)chunk.len - offset,
                            pcm_raw, &info);
            if (samples <= 0 || info.frame_bytes <= 0) break;
            offset += info.frame_bytes;
            mp3_frame_count++;

            // Resample if 24kHz → 16kHz
            int out_frames;
            if (info.hz == 24000) {
                out_frames = resample_24k_to_16k(pcm_raw, samples, pcm_resampled);
                pipeline_ws_player_write_pcm(pcm_resampled, out_frames);
            } else {
                pipeline_ws_player_write_pcm(pcm_raw, samples);
                out_frames = samples;
            }
            pcm_frame_total += out_frames;
        }

        ESP_LOGI(TAG, "chunk #%lu decoded: %lu mp3 frames -> %lu pcm frames (resampled)",
                 (unsigned long)chunk_count,
                 (unsigned long)mp3_frame_count,
                 (unsigned long)pcm_frame_total);
        free(chunk.data);

        // Check if queue is now empty
        if (uxQueueMessagesWaiting(s_queue) == 0) {
            s_busy = false;
            // Wait 300ms then unmute
            vTaskDelay(pdMS_TO_TICKS(300));
            g_mic_muted = false;
            ESP_LOGI(TAG, "[OK] all chunks played, queue empty");
            ESP_LOGI(TAG, "mic UNMUTED (300ms after last chunk)");
            if (s_done_sem) xSemaphoreGive(s_done_sem);
        }
    }

    heap_caps_free(pcm_raw);
    heap_caps_free(pcm_resampled);
    ESP_LOGI(TAG, "[OK] task stopped, heap_free=%lu", esp_get_free_heap_size());
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mp3_player_open(void)
{
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(mp3_chunk_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue create failed");

    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) { vQueueDelete(s_queue); s_queue = NULL; return ESP_ERR_NO_MEM; }

    pipeline_ws_player_open();

    s_task_run = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        mp3_play_task, "mp3_play", 32 * 1024, NULL, 7,
        &s_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        vQueueDelete(s_queue); s_queue = NULL;
        vSemaphoreDelete(s_done_sem); s_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mp3_player_enqueue(const uint8_t *mp3, size_t len)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    mp3_chunk_t chunk;
    chunk.data = malloc(len);
    if (!chunk.data) return ESP_ERR_NO_MEM;
    memcpy(chunk.data, mp3, len);
    chunk.len  = len;
    if (xQueueSend(s_queue, &chunk, 0) != pdTRUE) {
        free(chunk.data);
        ESP_LOGW(TAG, "queue full, dropping chunk len=%u", (unsigned)len);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mp3_player_flush_and_wait(void)
{
    if (!s_done_sem) return ESP_OK;
    xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(30000));
    return ESP_OK;
}

esp_err_t mp3_player_close(void)
{
    s_task_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    pipeline_ws_player_close();
    if (s_queue) {
        mp3_chunk_t chunk;
        while (xQueueReceive(s_queue, &chunk, 0) == pdTRUE) free(chunk.data);
        vQueueDelete(s_queue); s_queue = NULL;
    }
    if (s_done_sem) { vSemaphoreDelete(s_done_sem); s_done_sem = NULL; }
    g_mic_muted = false;
    s_busy = false;
    return ESP_OK;
}

bool mp3_player_is_busy(void) { return s_busy; }
```

- [ ] **Step 4: Build**

```bash
cd products/ws_meeting_demo && idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add products/ws_meeting_demo/main/minimp3.h products/ws_meeting_demo/main/mp3_player.h products/ws_meeting_demo/main/mp3_player.c
git commit -m "feat: add mp3_player with minimp3 + 24k->16k resample (Task 7)"
```

---

## Task 8 — ws_session.c: State machine integration

**Status:** ✅ Done

**Files:**
- Create: `main/ws_session.h`
- Create: `main/ws_session.c`

- [ ] **Step 1: Create ws_session.h**

```c
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WS_SESSION_IDLE,
    WS_SESSION_CONNECTING,
    WS_SESSION_MEETING,
    WS_SESSION_HOST,
    WS_SESSION_ERROR,
} ws_session_state_t;

// Shared flag: set by mp3_player, read by host_ws feed_task
extern volatile bool g_mic_muted;

// Called by ui_meeting button callbacks (fast, non-blocking)
esp_err_t ws_session_start_meeting(void);
esp_err_t ws_session_stop_meeting(void);
esp_err_t ws_session_enter_host(void);
esp_err_t ws_session_exit_host(void);

// Thread-safe UI text update (uses lv_async_call internally)
void ws_session_update_ui_status(const char *text);

ws_session_state_t ws_session_get_state(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create ws_session.c**

```c
#include "ws_session.h"
#include "api_client.h"
#include "transcribe_ws.h"
#include "host_ws.h"
#include "mp3_player.h"
#include "ui_meeting.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "bsp/esp_vocat.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_session";

volatile bool g_mic_muted = false;

typedef enum {
    CMD_START, CMD_STOP, CMD_ENTER_HOST, CMD_EXIT_HOST,
} cmd_type_t;

typedef struct { cmd_type_t type; } session_cmd_t;

static QueueHandle_t      s_cmd_queue = NULL;
static ws_session_state_t s_state     = WS_SESSION_IDLE;
static char               s_session_id[64] = {0};

static void set_state(ws_session_state_t st)
{
    s_state = st;
    const char *names[] = {"IDLE","CONNECTING","MEETING","HOST","ERROR"};
    ESP_LOGI(TAG, "state → %s", names[st]);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
}

// ---- UI async update ----
typedef struct { char text[128]; } ui_update_t;

static void ui_update_async(void *param)
{
    ui_update_t *u = (ui_update_t *)param;
    ui_meeting_set_status(u->text);
    ESP_LOGI(TAG, "ui status: \"%.40s\"", u->text);
    free(u);
}

void ws_session_update_ui_status(const char *text)
{
    ui_update_t *u = malloc(sizeof(ui_update_t));
    if (!u) return;
    strlcpy(u->text, text, sizeof(u->text));
    lv_async_call(ui_update_async, u);
}

// ---- Host WS message callback ----
static void on_host_msg(const char *type, cJSON *root, void *ctx)
{
    if (strcmp(type, "transcription") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Q: %s", text->valuestring);
            ws_session_update_ui_status(buf);
        }
    } else if (strcmp(type, "answer_text") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *done = cJSON_GetObjectItemCaseSensitive(root, "done");
        if (cJSON_IsString(text) && strlen(text->valuestring) > 0) {
            ws_session_update_ui_status(text->valuestring);
        }
        if (cJSON_IsTrue(done)) {
            ws_session_update_ui_status("");  // newline signal
        }
        ESP_LOGI(TAG, "recv answer_text (done=%s) len=%d",
                 cJSON_IsTrue(done) ? "true" : "false",
                 cJSON_IsString(text) ? (int)strlen(text->valuestring) : 0);
    } else if (strcmp(type, "answer_audio") == 0) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(data) && data->valuestring) {
            size_t b64_len = strlen(data->valuestring);
            size_t mp3_len = b64_len * 3 / 4 + 4;
            uint8_t *mp3   = malloc(mp3_len);
            if (mp3) {
                size_t out_len = 0;
                if (mbedtls_base64_decode(mp3, mp3_len, &out_len,
                        (const unsigned char *)data->valuestring, b64_len) == 0) {
                    static uint32_t chunk_count = 0;
                    chunk_count++;
                    ESP_LOGI(TAG, "recv answer_audio chunk #%lu len=%u",
                             (unsigned long)chunk_count, (unsigned)out_len);
                    mp3_player_enqueue(mp3, out_len);
                }
                free(mp3);
            }
        }
    } else if (strcmp(type, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Error: %s", msg->valuestring);
            ws_session_update_ui_status(buf);
        }
    }
}

// ---- State machine actions ----
static void do_start_meeting(void)
{
    set_state(WS_SESSION_CONNECTING);
    ui_meeting_set_state(UI_STATE_MEETING);

    if (api_client_create_session(NULL, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: session create failed");
        set_state(WS_SESSION_IDLE);
        ui_meeting_set_state(UI_STATE_IDLE);
        return;
    }

    mp3_player_open();

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: transcribe WS connect failed");
        set_state(WS_SESSION_ERROR);
        ws_session_update_ui_status("Connection Error");
        return;
    }

    set_state(WS_SESSION_MEETING);
}

static void do_stop_meeting(void)
{
    transcribe_ws_send_end();
    transcribe_ws_disconnect();
    api_client_end_session(s_session_id);
    mp3_player_close();
    memset(s_session_id, 0, sizeof(s_session_id));
    set_state(WS_SESSION_IDLE);
    ui_meeting_set_state(UI_STATE_IDLE);
}

static void do_enter_host(void)
{
    ESP_LOGI(TAG, "state → HOST (entering host mode)");
    transcribe_ws_disconnect();
    host_ws_set_callback(on_host_msg, NULL);

    if (host_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] enter_host: WS connect failed");
        // Re-connect transcribe WS and stay in MEETING
        transcribe_ws_connect(s_session_id);
        set_state(WS_SESSION_MEETING);
        return;
    }

    set_state(WS_SESSION_HOST);
    ui_meeting_set_state(UI_STATE_HOST);
    ws_session_update_ui_status("Listening...");
}

static void do_exit_host(void)
{
    ESP_LOGI(TAG, "state → MEETING (exit host mode)");
    host_ws_disconnect();
    mp3_player_close();
    mp3_player_open();
    set_state(WS_SESSION_MEETING);
    ui_meeting_set_state(UI_STATE_MEETING);

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] exit_host: transcribe WS reconnect failed");
        ws_session_update_ui_status("Connection Error");
    }
}

// ---- Command task ----
static void session_cmd_task(void *arg)
{
    while (1) {
        session_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.type) {
        case CMD_START:      do_start_meeting(); break;
        case CMD_STOP:       do_stop_meeting();  break;
        case CMD_ENTER_HOST: do_enter_host();    break;
        case CMD_EXIT_HOST:  do_exit_host();     break;
        }
    }
}

static void enqueue_cmd(cmd_type_t type)
{
    if (!s_cmd_queue) return;
    session_cmd_t cmd = {.type = type};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

// ---- Public API ----
__attribute__((constructor))
static void ws_session_init(void)
{
    s_cmd_queue = xQueueCreate(4, sizeof(session_cmd_t));
    xTaskCreatePinnedToCoreWithCaps(
        session_cmd_task, "sess_cmd", 8 * 1024, NULL, 4,
        NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

esp_err_t ws_session_start_meeting(void)  { enqueue_cmd(CMD_START);      return ESP_OK; }
esp_err_t ws_session_stop_meeting(void)   { enqueue_cmd(CMD_STOP);       return ESP_OK; }
esp_err_t ws_session_enter_host(void)     { enqueue_cmd(CMD_ENTER_HOST); return ESP_OK; }
esp_err_t ws_session_exit_host(void)      { enqueue_cmd(CMD_EXIT_HOST);  return ESP_OK; }

ws_session_state_t ws_session_get_state(void) { return s_state; }
```

Note: Add `#include "mbedtls/base64.h"` at the top of ws_session.c for the base64 decode in `on_host_msg`.

- [ ] **Step 3: Build**

```bash
cd products/ws_meeting_demo && idf.py build
```

- [ ] **Step 4: Flash + verify full positive flow**

Expected (complete log sequence):
```
I (...) ws_session: state → CONNECTING
I (...) api_client: [OK] session created: sess-<ID>
I (...) transcribe_ws: [OK] connected wss://.../ws/transcribe/sess-<ID>
I (...) ws_session: state → MEETING
I (...) ws_session: state → HOST (entering host mode)
I (...) transcribe_ws: [OK] disconnected
I (...) host_ws: [OK] connected wss://.../ws/host/sess-<ID>
I (...) vad: speech start (rms=<N>)
I (...) vad: [OK] end_of_speech triggered (...)
I (...) host_ws: [OK] sent end_of_speech
I (...) host_ws: recv transcription
I (...) host_ws: recv answer_audio chunk #1 len=<N>
I (...) mp3_player: [OK] playing chunk #1 len=<N>
I (...) mp3_player: mic MUTED (playback started)
I (...) mp3_player: [OK] all chunks played, queue empty
I (...) mp3_player: mic UNMUTED (300ms after last chunk)
```

- [ ] **Step 5: Commit**

```bash
git add products/ws_meeting_demo/main/ws_session.h products/ws_meeting_demo/main/ws_session.c
git commit -m "feat: add ws_session state machine + full module integration (Task 8)"
```

---

## Task 9 — ui_meeting.c: Mic dot + ws_session wiring

**Status:** ✅ Done (mic dot + FreeRTOS blink timer, ws_session wired, build verified)

**Files:**
- Modify: `main/ui_meeting.h`
- Modify: `main/ui_meeting.c`

- [ ] **Step 1: Add mic dot widget and blink timer to ui_meeting.h/c**

In `ui_meeting.h`, add no new public API (mic dot is internal). The state constants stay the same.

In `ui_meeting.c`, add:
```c
#include "ws_session.h"

// New widget handle
static lv_obj_t *s_mic_dot = NULL;
static lv_timer_t *s_blink_timer = NULL;
static bool s_dot_visible = false;

// Add in ui_meeting_create() after status_label:
s_mic_dot = lv_obj_create(screen);
lv_obj_set_size(s_mic_dot, 12, 12);
lv_obj_set_style_radius(s_mic_dot, LV_RADIUS_CIRCLE, 0);
lv_obj_set_style_bg_color(s_mic_dot, lv_color_make(0x00, 0xCC, 0x44), 0);
lv_obj_set_style_border_width(s_mic_dot, 0, 0);
lv_obj_align(s_mic_dot, LV_ALIGN_TOP_MID, 0, 225);
lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);

// Blink callback
static void blink_cb(lv_timer_t *t) {
    s_dot_visible = !s_dot_visible;
    if (s_dot_visible) lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(s_mic_dot,   LV_OBJ_FLAG_HIDDEN);
}
```

- [ ] **Step 2: Update apply_state() — add mic dot logic**

```c
// In apply_state(), after existing hide-all block, update MEETING and HOST cases:
case UI_STATE_MEETING:
    // existing code ...
    if (!s_blink_timer) {
        s_blink_timer = lv_timer_create(blink_cb, 500, NULL);
    }
    ESP_LOGI(TAG, "state → MEETING, mic dot visible");
    break;

case UI_STATE_HOST:
    // existing code ...
    if (s_blink_timer) { lv_timer_del(s_blink_timer); s_blink_timer = NULL; }
    lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "state → HOST, mic dot hidden");
    break;

case UI_STATE_IDLE:
    // existing code ...
    if (s_blink_timer) { lv_timer_del(s_blink_timer); s_blink_timer = NULL; }
    lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "state → IDLE");
    break;
```

- [ ] **Step 3: Wire button callbacks to ws_session**

Replace mock button callbacks:
```c
static void btn_start_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "[btn] Start Meeting");
    ws_session_start_meeting();
}
static void btn_host_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "[btn] Host Mode");
    ws_session_enter_host();
}
static void btn_stop_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "[btn] Stop Meeting");
    ws_session_stop_meeting();
}
static void btn_exit_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "[btn] Exit Host Mode");
    ws_session_exit_host();
}
```

Remove `session_state_cb` and `task_session_start/stop` (no longer needed).

- [ ] **Step 4: Build + flash + verify visually**

Expected:
```
I (...) ui_meeting: state → MEETING, mic dot visible   ← green dot blinks on screen
I (...) ui_meeting: state → HOST, mic dot hidden       ← dot disappears
I (...) ui_meeting: status updated: "Listening..."
I (...) ui_meeting: status updated: "Q: <text>"
```

- [ ] **Step 5: Commit**

```bash
git add products/ws_meeting_demo/main/ui_meeting.h products/ws_meeting_demo/main/ui_meeting.c
git commit -m "feat: add mic dot blink + wire ws_session to UI (Task 9)"
```

---

## Task 10 — Integration test + latency measurement

**Status:** 🔶 Partial — [LATENCY] markers added to host_ws.c, ws_session.c, mp3_player.c. Integration test (Steps 3–5) requires hardware.

**Files:**
- Modify: `main/ws_session.c` — add `[LATENCY]` log points
- Modify: `main/mp3_player.c` — add `[LATENCY]` log points

- [ ] **Step 1: Add latency timestamps to ws_session.c (in on_host_msg)**

```c
#include "esp_timer.h"

static int64_t s_eos_sent_ts = 0;

// In host_ws.c feed_task, after sending end_of_speech:
// (add to ws_session.c via a shared timestamp, or log directly in host_ws.c)

// In on_host_msg, case "answer_audio":
int64_t now = esp_timer_get_time() / 1000;
if (chunk_count == 1) {
    ESP_LOGI(TAG, "[LATENCY] first_answer_audio_recv ts=%lldms", now);
}

// After sending end_of_speech in host_ws.c:
ESP_LOGI(TAG, "[LATENCY] end_of_speech_sent ts=%lldms", esp_timer_get_time()/1000);
```

- [ ] **Step 2: Add latency timestamps to mp3_player.c**

```c
// In mp3_play_task, when receiving first frame of a chunk:
int64_t recv_ts = esp_timer_get_time() / 1000;
ESP_LOGI(TAG, "[LATENCY] answer_audio_recv ts=%lldms", recv_ts);
// After first pipeline_ws_player_write_pcm:
int64_t play_ts = esp_timer_get_time() / 1000;
ESP_LOGI(TAG, "[LATENCY] playback_start ts=%lldms delta=%lldms",
         play_ts, play_ts - recv_ts);
```

- [ ] **Step 3: Add 5-minute heap watchdog in ws_session.c**

```c
// In session_cmd_task, add a periodic timer:
static void heap_watchdog(void *arg)
{
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
}
// On CMD_START: create esp_timer with period 5*60*1000*1000 us
```

- [ ] **Step 4: Build + flash + run full integration test**

Test sequence:
1. Press Start → Meeting mode 30s (say a few sentences) → no crash
2. Press Host → ask a question → wait for answer audio → hear playback → no crash
3. Press Exit → back to Meeting → 30s more → Press Stop → IDLE
4. Repeat 3 times

Expected latency logs:
```
I (...) host_ws:    [LATENCY] end_of_speech_sent ts=<T1>ms
I (...) ws_session: [LATENCY] first_answer_audio_recv ts=<T2>ms  ← T2-T1 ≤ 5000ms
I (...) mp3_player: [LATENCY] answer_audio_recv ts=<T3>ms
I (...) mp3_player: [LATENCY] playback_start ts=<T4>ms delta=<N>ms  ← N ≤ 300ms
```

No `Guru Meditation Error`, no `stack overflow`, no `abort()`.

- [ ] **Step 5: Commit**

```bash
git add products/ws_meeting_demo/main/ws_session.c products/ws_meeting_demo/main/mp3_player.c products/ws_meeting_demo/main/host_ws.c
git commit -m "feat: add latency measurement + integration test complete (Task 10)"
```

---

## Acceptance Criteria Summary

| Metric | Target |
|--------|--------|
| end_of_speech → first_answer_audio | ≤ 5000ms |
| answer_audio_recv → playback_start | ≤ 300ms |
| 30min stability heap drift | < 4KB |
| Mode switch (Meeting↔Host) 20 times | No crash |
| VAD false trigger rate (quiet room) | < 5% / 20 tests |
