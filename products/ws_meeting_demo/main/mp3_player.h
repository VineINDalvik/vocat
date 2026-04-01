// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mp3_player_open(void);
esp_err_t mp3_player_enqueue(const uint8_t *mp3, size_t len);
void      mp3_player_signal_done(void);
esp_err_t mp3_player_flush_and_wait(void);
esp_err_t mp3_player_close(void);
bool      mp3_player_is_busy(void);
bool      mp3_player_pending(void);

#ifdef __cplusplus
}
#endif
