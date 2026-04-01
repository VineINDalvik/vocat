# TTS Audio Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix choppy, garbled, noisy TTS playback in host mode, and remove TTS text from host mode UI.

**Architecture:** Three independent tracks run in parallel: (A) fix `pipeline_ws.c` player_task with accumulation buffer + pre-roll to eliminate partial-frame zero-padding (root cause of noise/garbled audio) and ring-buffer starvation (root cause of choppiness); (B) pin `mp3_play_task` to core 1 to remove CPU contention with the I2S writer on core 0; (C) clean up `ws_session.c` host-mode message handler to suppress TTS text from UI and reset per-response latency counter.

**Tech Stack:** ESP-IDF 5.5, FreeRTOS, minimp3, esp_codec_dev, ES8311 DAC via I2S (16 kHz 32-bit stereo), LVGL 8

---

## File Map

| File | Track | Changes |
|------|-------|---------|
| `products/ws_meeting_demo/main/pipeline_ws.c` | A | ring-buffer 32 KB, pre-roll counter, player_task accumulation loop |
| `products/ws_meeting_demo/main/mp3_player.c` | B | pin mp3_play_task to core 1 |
| `products/ws_meeting_demo/main/ws_session.c` | C | drop UI update for answer_text; move chunk_count to file scope, reset on `done` |

No header changes required. All changes are internal to `.c` files.

---

## Background: Why the Audio Is Broken

### Garbled / mixed-sounding voices — partial-frame zero-padding

`player_task` (`pipeline_ws.c:286-301`) calls:

```c
int16_t *pcm = xRingbufferReceiveUpTo(s_play_rb, &rx_len, 20ms, 640);
```

`xRingbufferReceiveUpTo` returns **up to** 640 bytes — it may return 40 bytes if that is all that is available. The code then writes a **full 2560-byte I2S frame** regardless:

```c
int write_bytes = frames * 2 * sizeof(int32_t);   // e.g. 160 bytes for 20 mono samples
memset((uint8_t *)out_buf + write_bytes, 0, sizeof(out_buf) - write_bytes);  // zero-pad 2400 bytes
esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));   // 2560 bytes always
```

A signal that abruptly drops to zero mid-frame and resumes next frame creates audible clicks and makes speech unintelligible.

**Fix:** accumulate in a local 320-sample buffer; only emit a full 2560-byte frame when the buffer is complete.

### Choppy / silences — ring-buffer starvation with no pre-roll

When `xRingbufferReceiveUpTo` returns NULL (ring buffer empty), the task immediately writes 2560 bytes of silence to the codec. This happens:
- at the very start of playback (first PCM arrives before the player pre-buffers)
- between consecutive MP3 chunks (WS network jitter)

**Fix:** track how many PCM bytes have been written since `open()`; hold in silence until a 150 ms pre-roll is accumulated; re-arm on every subsequent underrun.

### CPU contention — both tasks on core 0

`mp3_play_task` (decode) and `player_task` (I2S write) both run at priority 7 on core 0. Time-slicing between decode and I2S write creates jitter in PCM delivery.

**Fix:** pin `mp3_play_task` to core 1.

---

## Track A — `pipeline_ws.c`: Accumulation Buffer + Pre-Roll

**Files:**
- Modify: `products/ws_meeting_demo/main/pipeline_ws.c`

- [ ] **Step A-1: Increase ring buffer and add pre-roll constants**

In `pipeline_ws.c`, change the constants block (lines 50-55):

```c
// Playback ring buffer: 1000ms of 16kHz 16bit mono PCM = 32000 bytes
#define PLAY_RB_SIZE        (32000)
#define PLAY_PRE_ROLL_BYTES (4800)    // 150ms pre-roll at 16kHz 16bit mono
#define PLAY_TASK_STACK     (8 * 1024)
#define PLAY_TASK_PRIORITY  (7)
#define PLAY_TASK_CORE      (0)
```

- [ ] **Step A-2: Add pre-roll counter to state section**

After the existing state variables block (around line 76, after `s_play_done_sem`), add:

```c
static volatile size_t   s_pre_roll_written = 0; // bytes written since last open/underrun
```

- [ ] **Step A-3: Reset counter in `pipeline_ws_player_open()`**

In `pipeline_ws_player_open()` (line 312), before `s_play_task_run = true;`, add:

```c
s_pre_roll_written = 0;
```

- [ ] **Step A-4: Increment counter in `pipeline_ws_player_write_pcm()`**

Replace `pipeline_ws_player_write_pcm()` (lines 342-349) with:

```c
esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)
{
    if (!s_play_open || !s_play_rb) return ESP_ERR_INVALID_STATE;
    s_pre_roll_written += (size_t)frames * sizeof(int16_t);
    BaseType_t ok = xRingbufferSend(s_play_rb, pcm,
                                     (size_t)frames * sizeof(int16_t),
                                     pdMS_TO_TICKS(200));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
```

- [ ] **Step A-5: Replace `player_task` with accumulation-buffer version**

Replace the entire `player_task` function (lines 279-306) with:

```c
static void player_task(void *arg)
{
    // Output frame: 320 stereo 32-bit samples = 2560 bytes = 20ms at 16kHz
    int32_t out_buf[320 * 2];
    // Accumulation buffer: collect exactly 320 mono 16-bit samples before emitting
    int16_t acc_buf[320];
    int     acc_frames = 0;
    bool    playing    = false;

    while (s_play_task_run) {

        // ---- Pre-roll gate ------------------------------------------------
        if (!playing) {
            if (s_pre_roll_written >= PLAY_PRE_ROLL_BYTES) {
                playing = true;
                ESP_LOGI(TAG, "[OK] pre-roll reached %u bytes, starting playback",
                         (unsigned)s_pre_roll_written);
            } else {
                // Keep I2S clock running with silence while buffering
                memset(out_buf, 0, sizeof(out_buf));
                esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
                continue;
            }
        }

        // ---- Drain ring buffer into accumulation buffer -------------------
        size_t need = (size_t)(320 - acc_frames) * sizeof(int16_t);
        size_t rx_len = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(
            s_play_rb, &rx_len, pdMS_TO_TICKS(5), need);

        if (pcm && rx_len > 0) {
            int got = (int)(rx_len / sizeof(int16_t));
            memcpy(acc_buf + acc_frames, pcm, (size_t)got * sizeof(int16_t));
            vRingbufferReturnItem(s_play_rb, pcm);
            acc_frames += got;
        }

        // ---- Emit only when accumulation buffer is full -------------------
        if (acc_frames >= 320) {
            conv_16m_to_32s(acc_buf, out_buf, 320);
            esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
            acc_frames = 0;
            continue;
        }

        // ---- True underrun: flush partial + re-arm pre-roll ---------------
        if (pcm == NULL || rx_len == 0) {
            if (acc_frames > 0) {
                // Flush what we have; zero-pad the rest of the I2S frame
                conv_16m_to_32s(acc_buf, out_buf, acc_frames);
                memset(out_buf + acc_frames * 2, 0,
                       (size_t)(320 - acc_frames) * 2 * sizeof(int32_t));
                esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
                acc_frames = 0;
            } else {
                memset(out_buf, 0, sizeof(out_buf));
                esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
            }
            // Re-arm pre-roll so the next audio burst buffers before playing
            s_pre_roll_written = 0;
            playing = false;
        }
    }

    if (s_play_done_sem) xSemaphoreGive(s_play_done_sem);
    vTaskDelete(NULL);
}
```

- [ ] **Step A-6: Build to verify no compile errors**

```bash
cd /Users/pdan/playground/lucy-chat/esp-brookesia/products/ws_meeting_demo
source /Users/pdan/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -15
```

Expected: `Project build complete.` with no errors.

- [ ] **Step A-7: Commit**

```bash
git add products/ws_meeting_demo/main/pipeline_ws.c
git commit -m "fix(pipeline_ws): accumulation buffer + pre-roll to fix garbled/choppy TTS audio

- Ring buffer 16 KB → 32 KB (1 s jitter headroom)
- player_task accumulates 320 mono samples before each I2S write;
  eliminates partial-frame zero-padding that caused garbled speech
- Pre-roll: hold I2S in silence until 150 ms of PCM is buffered;
  re-arms on every underrun to prevent initial and mid-stream stutter"
```

---

## Track B — `mp3_player.c`: Pin Decode Task to Core 1

**Files:**
- Modify: `products/ws_meeting_demo/main/mp3_player.c`

- [ ] **Step B-1: Change task core from 0 to 1**

In `mp3_player_open()`, replace the `xTaskCreatePinnedToCoreWithCaps` call (lines 163-167):

```c
    s_task_run = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        mp3_play_task, "mp3_play", 32 * 1024, NULL, 7,
        &s_task, 1,                                      // ← core 1 (was 0)
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

- [ ] **Step B-2: Build to verify**

```bash
cd /Users/pdan/playground/lucy-chat/esp-brookesia/products/ws_meeting_demo
source /Users/pdan/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step B-3: Commit**

```bash
git add products/ws_meeting_demo/main/mp3_player.c
git commit -m "fix(mp3_player): pin decode task to core 1 to separate from I2S writer"
```

---

## Track C — `ws_session.c`: Host Mode UI + Chunk Count Reset

**Files:**
- Modify: `products/ws_meeting_demo/main/ws_session.c`

### Problem 1: `answer_text` pushes TTS transcript to UI in host mode (unwanted)

Line 94: `if (tlen > 0) ws_session_update_ui_status(text_j->valuestring);` — remove.

### Problem 2: `chunk_count` is `static` inside a nested block — never resets between Q&A turns

The variable at line 112 (`static uint32_t chunk_count = 0;`) persists forever. After the first turn the latency log `[LATENCY] first_answer_audio_recv` never fires again. It should reset each time the server signals `done` (end of one Q&A cycle).

- [ ] **Step C-1: Promote chunk_count to file scope**

At the top of `ws_session.c`, after the existing static declarations (around line 34), add:

```c
static uint32_t s_answer_chunk_count = 0;
```

- [ ] **Step C-2: Rewrite `on_host_msg` answer_text and answer_audio handlers**

Replace the `answer_text` handler (lines 87-97):

```c
    } else if (strcmp(type, "answer_text") == 0) {
        cJSON *done_j = cJSON_GetObjectItemCaseSensitive(root, "done");
        cJSON *text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        bool done = cJSON_IsTrue(done_j);
        int tlen = cJSON_IsString(text_j) ? (int)strlen(text_j->valuestring) : 0;
        ESP_LOGI(TAG, "recv answer_text (done=%s) len=%d",
                 done ? "true" : "false", tlen);
        // answer_text is not shown on UI in host mode
```

Replace the `answer_audio` handler inner block (lines 111-122) — remove the nested `static uint32_t chunk_count = 0;` and use `s_answer_chunk_count` instead:

```c
                if (rc == 0 && out_len > 0) {
                    s_answer_chunk_count++;
                    ESP_LOGI(TAG, "recv answer_audio chunk #%lu len=%u",
                             (unsigned long)s_answer_chunk_count, (unsigned)out_len);
                    if (s_answer_chunk_count == 1) {
                        ESP_LOGI(TAG, "[LATENCY] first_answer_audio_recv ts=%lldms",
                                 (long long)(esp_timer_get_time() / 1000));
                    }
                    mp3_player_enqueue(mp3, out_len);
                }
```

- [ ] **Step C-3: Reset chunk counter when server signals `done`**

Add a `done` handler at the end of `on_host_msg`, after the `error` handler (before the closing `}`):

```c
    } else if (strcmp(type, "done") == 0) {
        s_answer_chunk_count = 0;
        ESP_LOGI(TAG, "recv done — resetting answer chunk counter");
```

- [ ] **Step C-4: Build to verify**

```bash
cd /Users/pdan/playground/lucy-chat/esp-brookesia/products/ws_meeting_demo
source /Users/pdan/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step C-5: Commit**

```bash
git add products/ws_meeting_demo/main/ws_session.c
git commit -m "fix(ws_session): remove TTS text from host mode UI; reset per-turn chunk counter on done"
```

---

## Integration Build + Hardware Test

After all three tracks are committed:

- [ ] **Step INT-1: Full build**

```bash
cd /Users/pdan/playground/lucy-chat/esp-brookesia/products/ws_meeting_demo
source /Users/pdan/esp/esp-idf/export.sh
idf.py build 2>&1 | grep -E "(error|warning|complete|binary size)"
```

Expected: `Project build complete.` with binary size logged.

- [ ] **Step INT-2: Flash**

```bash
idf.py -p /dev/cu.usbmodem2101 flash
```

- [ ] **Step INT-3: Serial monitor to temp file**

```bash
stty -f /dev/cu.usbmodem2101 115200 && cat /dev/cu.usbmodem2101 > /tmp/ws_tts_test.log &
```

- [ ] **Step INT-4: Hardware test checklist**

On device:
1. Press **Start** → wait for "● Meeting" (WiFi + WS must connect, verify log: `[OK] connected wss://...`)
2. Press **Host** → UI shows "Listening..."
3. Ask a question out loud (e.g. "刚才说了什么？")
4. Verify: log shows `[OK] pre-roll reached XXXX bytes, starting playback` before audio starts
5. Listen: audio should be clear, no clicks, no robotic/mixed garbling
6. Verify: UI does NOT show answer text during response (only the transcription "Q: ..." appears)
7. After response ends, verify log shows `recv done — resetting answer chunk counter`
8. Ask a second question; verify `[LATENCY] first_answer_audio_recv` fires again (confirms chunk counter reset)
9. Press **Exit** → **Stop** — no crash

- [ ] **Step INT-5: Check serial log for underrun rate**

```bash
grep -c "pre-roll reached" /tmp/ws_tts_test.log   # should fire once per response start
grep -c "re-arm" /tmp/ws_tts_test.log              # ideally 0 mid-response (ring buffer full enough)
```

---

## Self-Review

**Spec coverage:**
- ✅ Garbled/mixed audio → partial-frame zero-padding fixed by accumulation buffer (Track A)
- ✅ Choppy audio → pre-roll + 32 KB ring buffer (Track A)
- ✅ CPU contention → mp3 decode on core 1 (Track B)
- ✅ TTS text on UI in host mode → removed (Track C)
- ✅ Latency counter reset per Q&A turn → fixed (Track C)

**Placeholder scan:** None — all code blocks are complete and concrete.

**Type consistency:**
- `s_pre_roll_written`: declared `volatile size_t`, read in `player_task` with no cast issues
- `s_answer_chunk_count`: `uint32_t`, used consistently in `on_host_msg`
- `acc_buf[320]` / `acc_frames` local to `player_task` — no external dependencies
