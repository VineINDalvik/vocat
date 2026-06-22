// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "esp_err.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif

// Message callback: type is the JSON "type" field, root is the full parsed object (read-only)
typedef void (*host_ws_msg_cb_t)(const char *type, cJSON *root, void *ctx);

void      host_ws_set_callback(host_ws_msg_cb_t cb, void *ctx);

// Called (from a FreeRTOS task context) when server rejects the session with 403.
typedef void (*host_ws_rejected_cb_t)(void *ctx);
void host_ws_set_rejected_cb(host_ws_rejected_cb_t cb, void *ctx);

// Called from feed_task when AI finishes speaking and audio playback is complete
typedef void (*host_ws_tts_done_cb_t)(void *ctx);
void host_ws_set_tts_done_cb(host_ws_tts_done_cb_t cb, void *ctx);

// Connect wss://.../ws/host/{session_id}, open recorder, start feed task with VAD
esp_err_t host_ws_connect(const char *session_id);

// Send {"type":"stop"}, stop feed task, close recorder, disconnect WS
esp_err_t host_ws_disconnect(void);

// Force VAD to resume listening (used after server error when no "done" arrives)
void host_ws_force_resume(void);

// Send {"type":"interrupt"} to server — stop current AI response generation
esp_err_t host_ws_send_interrupt(void);

#ifdef __cplusplus
}
#endif
