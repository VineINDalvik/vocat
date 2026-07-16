// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// POST /api/session → writes session_id into out_session_id (max len bytes)
esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len);
esp_err_t api_client_check_reachable(void);

// POST /api/session/{session_id}/end
esp_err_t api_client_end_session(const char *session_id);

#ifdef __cplusplus
}
#endif
