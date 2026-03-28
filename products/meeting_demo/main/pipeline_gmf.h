// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_gmf.h — Thin audio pipeline for meeting_demo using esp_codec_dev.
//
// Recording path: ES7210 (16kHz 32bit 2ch) → downmix+convert → 16kHz 16bit mono PCM
// Playback path:  8kHz 16bit mono PCM (from volc RTC) → ES8311
//
// No ESP-ADF dependency. Uses esp_codec_dev directly via the speaker BSP.

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bytes per 20ms frame of 16kHz 16bit mono PCM (what volc RTC expects).
#define PIPELINE_FRAME_BYTES  640   // 16000 * 0.020 * 2 bytes

/**
 * @brief  Initialize codec hardware (I2C, I2S, ES7210 + ES8311).
 *         Must be called once before open/close.
 */
esp_err_t pipeline_gmf_hw_init(void);

/**
 * @brief  Deinitialize codec hardware.
 */
esp_err_t pipeline_gmf_hw_deinit(void);

/**
 * @brief  Open recorder: start I2S capture from ES7210.
 */
esp_err_t pipeline_gmf_recorder_open(void);

/**
 * @brief  Read one PCM frame from the recorder.
 *
 * Blocks until `size` bytes are available.
 * Output format: 16kHz, 16bit, mono (host byte order).
 *
 * @param[out]  buf   Caller-allocated buffer.
 * @param[in]   size  Number of bytes to read (use PIPELINE_FRAME_BYTES).
 * @return      Number of bytes read, or -1 on error.
 */
int  pipeline_gmf_recorder_read(void *buf, size_t size);

/**
 * @brief  Close recorder and release I2S.
 */
esp_err_t pipeline_gmf_recorder_close(void);

/**
 * @brief  Open player: start I2S output to ES8311.
 */
esp_err_t pipeline_gmf_player_open(void);

/**
 * @brief  Write PCM data to the player (non-blocking, queued).
 *
 * Input format: 8kHz, 16bit, mono (volc RTC output).
 * The pipeline upsample 8kHz → 16kHz before writing to codec.
 *
 * @param[in]  buf   PCM data.
 * @param[in]  size  Byte count.
 * @return     ESP_OK or error code.
 */
esp_err_t pipeline_gmf_player_write(const void *buf, size_t size);

/**
 * @brief  Close player.
 */
esp_err_t pipeline_gmf_player_close(void);

/**
 * @brief  Set output volume (0–100).
 */
esp_err_t pipeline_gmf_set_volume(int volume);

#ifdef __cplusplus
}
#endif
