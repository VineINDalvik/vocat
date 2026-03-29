// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_gmf.h — Audio pipeline for rtc_meeting_demo.
//
// Recording: ES7210 → I2S RX (16kHz 32bit 2ch) → downmix+truncate → 16kHz 16bit mono
// Playback:  Opus packet (from RTC on_audio_data) → Opus decode → 16kHz 16bit mono → 16kHz 32bit stereo → ES8311
//
// The Opus encode/decode is done in THIS module for playback only.
// The recorder outputs raw PCM; the caller (rtc_session) encodes it before sending to RTC.

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bytes per 20ms frame of 16kHz 16bit mono PCM.
#define PIPELINE_FRAME_BYTES  640   // 16000 * 0.020 * 2

esp_err_t pipeline_gmf_hw_init(void);
esp_err_t pipeline_gmf_hw_deinit(void);

// Recorder: outputs 16kHz 16bit mono PCM
esp_err_t pipeline_gmf_recorder_open(void);
int       pipeline_gmf_recorder_read(void *buf, size_t size);
esp_err_t pipeline_gmf_recorder_close(void);

// Player: accepts raw Opus packets (as received in on_audio_data callback).
// The 2-byte length prefix expected by the ADF raw_opus_decoder is added internally.
esp_err_t pipeline_gmf_player_open(void);
esp_err_t pipeline_gmf_player_write(const void *opus_data, size_t opus_len);
esp_err_t pipeline_gmf_player_close(void);

esp_err_t pipeline_gmf_set_volume(int volume);

#ifdef __cplusplus
}
#endif
