// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "vad.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>
#include <math.h>

static const char *TAG = "vad";

#define FRAME_MS              20
#define SPEECH_START_FRAMES   2   // 2 consecutive frames (40ms) to enter SPEECH
#define SPEECH_MIN_FRAMES     (CONFIG_VAD_SPEECH_MIN_MS / FRAME_MS)
#define SILENCE_FRAMES        (CONFIG_VAD_SILENCE_MS    / FRAME_MS)
#define HANGOVER_FRAMES       10  // 200ms: stay in SPEECH after energy dips (catches soft consonants, brief pauses)

static int compute_rms(const int16_t *pcm, int frames)
{
    int64_t sum = 0;
    for (int i = 0; i < frames; i++) {
        int32_t s = pcm[i];
        sum += s * s;
    }
    if (frames <= 0) return 0;
    return (int)sqrt((double)(sum / frames));
}

void vad_reset(vad_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ESP_LOGD(TAG, "reset");
}

vad_result_t vad_process_frame(vad_ctx_t *ctx, const int16_t *pcm, int frames)
{
    int rms       = compute_rms(pcm, frames);
    int threshold = CONFIG_VAD_ENERGY_THRESHOLD;

    switch (ctx->state) {
    case VAD_STATE_WAITING:
        if (rms > threshold) {
            ctx->speech_frames++;
            if (ctx->speech_frames >= SPEECH_START_FRAMES) {
                ctx->state    = VAD_STATE_SPEECH;
                ctx->hangover = HANGOVER_FRAMES;
                ESP_LOGI(TAG, "speech start (rms=%d)", rms);
                return VAD_RESULT_SPEECH_START;
            }
        } else {
            ctx->speech_frames = 0;
        }
        break;

    case VAD_STATE_SPEECH:
        ctx->speech_frames++;
        if (rms > threshold) {
            ctx->hangover = HANGOVER_FRAMES;
        } else {
            ctx->hangover--;
            if (ctx->hangover <= 0) {
                ctx->state          = VAD_STATE_SILENCE_AFTER_SPEECH;
                ctx->silence_frames = 1;
                ESP_LOGI(TAG, "silence start after %dms speech (rms=%d thr=%d)",
                         ctx->speech_frames * FRAME_MS, rms, threshold);
            }
        }
        break;

    case VAD_STATE_SILENCE_AFTER_SPEECH:
        if (rms > threshold) {
            ESP_LOGI(TAG, "speech resumed after %dms silence (rms=%d)",
                     ctx->silence_frames * FRAME_MS, rms);
            ctx->state          = VAD_STATE_SPEECH;
            ctx->silence_frames = 0;
            ctx->hangover       = HANGOVER_FRAMES;
        } else {
            ctx->silence_frames++;
            if (ctx->silence_frames >= SILENCE_FRAMES) {
                int speech_ms  = ctx->speech_frames  * FRAME_MS;
                int silence_ms = ctx->silence_frames * FRAME_MS;
                if (ctx->speech_frames >= SPEECH_MIN_FRAMES) {
                    ESP_LOGI(TAG,
                             "[OK] end_of_speech (speech=%dms silence=%dms rms=%d)",
                             speech_ms, silence_ms, rms);
                    vad_reset(ctx);
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
