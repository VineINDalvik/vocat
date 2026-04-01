// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_ws.h — Audio pipeline for ws_meeting_demo.
//
// Recording: ES7210 → I2S RX (16kHz 32bit 2ch) → downmix+truncate → 16kHz 16bit mono
// Playback:  16kHz 16bit mono PCM → ring buffer → 16kHz 32bit stereo → ES8311

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bytes per 20ms frame of 16kHz 16bit mono PCM.
#define PIPELINE_FRAME_BYTES  640   // 16000 * 0.020 * 2

esp_err_t pipeline_ws_hw_init(void);
esp_err_t pipeline_ws_hw_deinit(void);

// Recorder: outputs 16kHz 16bit mono PCM
esp_err_t pipeline_ws_recorder_open(void);
int       pipeline_ws_recorder_read(void *buf, size_t size);
esp_err_t pipeline_ws_recorder_close(void);

// Player: accepts 16kHz 16bit mono PCM frames, writes to I2S via ring buffer
esp_err_t pipeline_ws_player_open(void);
esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames);
esp_err_t pipeline_ws_player_close(void);
bool      pipeline_ws_player_is_drained(void);

esp_err_t pipeline_ws_set_volume(int volume);

#ifdef __cplusplus
}
#endif
