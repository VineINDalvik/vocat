// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

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

void         vad_reset(vad_ctx_t *ctx);
vad_result_t vad_process_frame(vad_ctx_t *ctx, const int16_t *pcm, int frames);

#ifdef __cplusplus
}
#endif
