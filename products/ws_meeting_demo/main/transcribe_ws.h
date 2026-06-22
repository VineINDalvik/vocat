// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// Connect wss://.../ws/transcribe/{session_id}, open recorder, start feed task
esp_err_t transcribe_ws_connect(const char *session_id);

// Send {"type":"end"} to signal end of meeting recording
esp_err_t transcribe_ws_send_end(void);

// Stop feed task, close recorder, disconnect WS
esp_err_t transcribe_ws_disconnect(void);

// Wake word callback: called when "clare" is detected in a final transcript
typedef void (*transcribe_wake_word_cb_t)(void *ctx);
void transcribe_ws_set_wake_word_cb(transcribe_wake_word_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
