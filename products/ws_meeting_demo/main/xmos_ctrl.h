// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// xmos_ctrl.h — XMOS XVF3800 I2C control interface.
// Controls volume, mute, LED, DoA, VAD via I2C at address 0x2C.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t xmos_ctrl_init(i2c_master_bus_handle_t bus);
esp_err_t xmos_ctrl_deinit(void);

// Volume control (0-100)
esp_err_t xmos_ctrl_set_volume(int volume);

// Mute control
esp_err_t xmos_ctrl_set_mute(bool mute);
bool      xmos_ctrl_get_mute(void);

// DoA (Direction of Arrival) — angle in degrees, 0-360
esp_err_t xmos_ctrl_get_doa(int *angle);

// VAD (Voice Activity Detection)
esp_err_t xmos_ctrl_get_vad(bool *active);

#ifdef __cplusplus
}
#endif