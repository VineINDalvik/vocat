# RTC Meeting Demo 实现方案

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 基于 meeting_demo 创建新产品 `rtc_meeting_demo`，从 volc_conv_ai SDK（旧版硬件对话智能体）升级到 VolcEngineRTCLite SDK（实时对话式 AI），音频质量从 G711A 8kHz 提升到 Opus 16kHz。

**Architecture:** ESP32-S3 客户端通过 HTTP 调用业务服务器获取 RTC 房间凭证，然后直接使用 VolcEngineRTCLite SDK 加入房间、发送/接收 Opus 音频。业务服务器（Python）调用火山 `StartVoiceChat` API 创建云端 AI Agent。UI 与 meeting_demo 完全一致（3 状态按钮机）。

**Tech Stack:** ESP-IDF 5.5 + VolcEngineRTCLite SDK (byte_rtc_*) + esp_http_client + Opus codec + Python Flask 服务端

---

## 架构对比

| 维度 | meeting_demo (旧) | rtc_meeting_demo (新) |
|------|-------------------|----------------------|
| SDK | volc_conv_ai_sdk (ConversationalAI-Embedded-Kit-2.0) | VolcEngineRTCLite (byte_rtc_*) |
| 音频编码 | G711A 8kHz | Opus 16kHz |
| 连接模式 | 客户端直连 IoT 云 | 客户端 ← HTTP → 业务服务器 → StartVoiceChat API |
| Agent 管理 | 客户端创建 (bot_id) | 服务端创建 (StartVoiceChat) |
| AEC | 无（软件静音替代） | SDK 内置支持 |
| 服务端 | 不需要 | Python Flask 服务器 |

## 系统流程

```
[ESP32-S3 客户端]                    [Python 业务服务器]              [火山云]
     |                                     |                            |
     |-- HTTP POST /start_voice_chat ----->|                            |
     |                                     |-- StartVoiceChat API ----->|
     |                                     |<-- room_id, token, uid ----|
     |<-- {room_id, uid, token} -----------|                            |
     |                                     |                            |
     |-- byte_rtc_create/init --------                                  |
     |-- byte_rtc_set_audio_codec(OPUS)                                |
     |-- byte_rtc_join_room(room_id, uid, token) ---> RTC 连接 ------->|
     |                                     |                            |
     |-- byte_rtc_send_audio_data(mic PCM) --Opus 编码--> RTC --------->|
     |<-- on_audio_data(agent PCM) <-----Opus 解码---- RTC ------------|
     |                                     |                            |
     |-- byte_rtc_leave_room -------->                                  |
     |-- HTTP POST /stop_voice_chat ----->|-- StopVoiceChat API ------> |
```

## 文件结构

```
products/rtc_meeting_demo/
├── CMakeLists.txt                      # 根构建文件
├── partitions.csv                      # 分区表（同 meeting_demo）
├── sdkconfig.defaults                  # 默认配置
├── sdkconfig.local.example             # 凭证模板（gitignored）
├── main/
│   ├── CMakeLists.txt                  # 组件注册
│   ├── idf_component.yml              # IDF 依赖
│   ├── Kconfig.projbuild              # 配置菜单
│   ├── main.c                         # 入口
│   ├── wifi_init.c / wifi_init.h      # WiFi（复用 meeting_demo）
│   ├── ui_meeting.c / ui_meeting.h    # UI（复用 meeting_demo，微调）
│   ├── pipeline_gmf.c / pipeline_gmf.h # 音频管线（升级到 16kHz 双向）
│   ├── bot_client.c / bot_client.h    # HTTP 客户端（新增：调用业务服务器）
│   └── rtc_session.c / rtc_session.h  # RTC 会话（新增：替代 volc_rtc_session）
├── components/
│   └── volc_rtc_engine_lite/           # VolcEngineRTCLite SDK 组件（新增）
│       ├── CMakeLists.txt
│       ├── idf_component.yml
│       ├── include/
│       │   └── VolcEngineRTCLite.h     # SDK 头文件
│       └── libs/
│           └── esp32s3/
│               └── libVolcEngineRTCLite.a  # 预编译库
└── server/
    ├── requirements.txt                # Python 依赖
    ├── config.py                       # 服务器配置
    ├── app.py                          # Flask 主入口
    ├── rtc_api.py                      # 火山 RTC API 调用（签名）
    └── token_generator.py              # RTC Token 生成
```

---

### Task 1: 创建产品目录骨架

**Files:**
- Create: `products/rtc_meeting_demo/CMakeLists.txt`
- Create: `products/rtc_meeting_demo/partitions.csv`
- Create: `products/rtc_meeting_demo/sdkconfig.defaults`
- Create: `products/rtc_meeting_demo/sdkconfig.local.example`

- [ ] **Step 1: 创建目录结构**

```bash
mkdir -p products/rtc_meeting_demo/main
mkdir -p products/rtc_meeting_demo/components/volc_rtc_engine_lite/include
mkdir -p products/rtc_meeting_demo/components/volc_rtc_engine_lite/libs/esp32s3
mkdir -p products/rtc_meeting_demo/server
```

- [ ] **Step 2: 创建根 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.5)

# 复用 speaker 的 BSP 和 managed components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/common_components"
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/managed_components"
)

# sdkconfig.local 存放私有凭证（gitignored）
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtc_meeting_demo)
```

- [ ] **Step 3: 复制 partitions.csv（同 meeting_demo）**

```
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     ,         0x4000,
otadata,  data, ota,     ,         0x2000,
phy_init, data, phy,     ,         0x1000,
factory,  app,  factory, 0xb0000,  8000K,
```

- [ ] **Step 4: 创建 sdkconfig.defaults**

```ini
# ESP-IDF 5.5 — rtc_meeting_demo defaults
CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
CONFIG_APP_PROJECT_VER="0.1.0"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y

# TLS
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y

# PSRAM (ESP32-S3 Octal)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y

# CPU / System
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_INT_WDT_TIMEOUT_MS=2000
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10

# WiFi
CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER=y
CONFIG_ESP_WIFI_TX_BA_WIN=16
CONFIG_ESP_WIFI_RX_BA_WIN=6

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY=y
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096

# LwIP
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=10
CONFIG_LWIP_SNTP_MAX_SERVERS=2
CONFIG_LWIP_DHCP_GET_NTP_SRV=y
CONFIG_LWIP_FALLBACK_DNS_SERVER_SUPPORT=y

# mbedTLS — reduce RAM
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n

# Codec chip support
CONFIG_CODEC_ES8311_SUPPORT=y
CONFIG_CODEC_ES7210_SUPPORT=y

# ESP-VoCat PCB version
CONFIG_BSP_PCB_VERSION_V1_2=y
CONFIG_BSP_I2C_NUM=0
CONFIG_BSP_LCD_DRAW_BUF_HEIGHT=15
CONFIG_BSP_LCD_DRAW_BUF_DOUBLE=y

# LVGL
CONFIG_LV_OS_FREERTOS=y
CONFIG_LV_DEF_REFR_PERIOD=10
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_PRINTF=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_24=y

# HTTP client (for bot_server calls)
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y

# Opus codec support
CONFIG_AUDIO_DECODER_OPUS_SUPPORT=y
CONFIG_AUDIO_ENCODER_OPUS_SUPPORT=y
```

- [ ] **Step 5: 创建 sdkconfig.local.example**

```ini
# WiFi credentials
CONFIG_MEETING_WIFI_SSID="your_wifi_ssid"
CONFIG_MEETING_WIFI_PASSWORD="your_wifi_password"

# Business server URL
CONFIG_RTC_BOT_SERVER_URL="http://192.168.1.100:8080"

# Volcengine App ID (for RTC SDK)
CONFIG_RTC_VOLC_APP_ID="your_volc_app_id"
```

- [ ] **Step 6: 提交骨架**

```bash
git add products/rtc_meeting_demo/
git commit -m "feat(rtc_meeting_demo): scaffold product directory structure"
```

---

### Task 2: 创建 VolcEngineRTCLite SDK 组件

**Files:**
- Create: `products/rtc_meeting_demo/components/volc_rtc_engine_lite/CMakeLists.txt`
- Create: `products/rtc_meeting_demo/components/volc_rtc_engine_lite/idf_component.yml`
- Copy: `VolcEngineRTCLite.h` → `include/`
- Copy: `libVolcEngineRTCLite.a` → `libs/esp32s3/`

- [ ] **Step 1: 从 meeting_demo 的 volc_conv_ai_sdk 中复制 SDK 文件**

```bash
# 复制头文件
cp products/meeting_demo/components/volc_conv_ai_sdk/volc_conv_ai/src/transports/high_quality/third_party/volc_rtc_engine_lite/inc/VolcEngineRTCLite.h \
   products/rtc_meeting_demo/components/volc_rtc_engine_lite/include/

# 复制预编译库
cp products/meeting_demo/components/volc_conv_ai_sdk/volc_conv_ai/src/transports/high_quality/third_party/volc_rtc_engine_lite/libs/esp32s3/libVolcEngineRTCLite.a \
   products/rtc_meeting_demo/components/volc_rtc_engine_lite/libs/esp32s3/
```

- [ ] **Step 2: 创建 CMakeLists.txt**

```cmake
# VolcEngineRTCLite SDK - ESP-IDF component wrapper
# Wraps the prebuilt static library for ESP32-S3

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES mbedtls espressif__zlib json lwip
)

# Link prebuilt static library for ESP32-S3
if(CONFIG_IDF_TARGET STREQUAL "esp32s3")
    add_prebuilt_library(volc_engine_rtc_lite
        "${CMAKE_CURRENT_LIST_DIR}/libs/esp32s3/libVolcEngineRTCLite.a"
        REQUIRES mbedtls espressif__zlib json lwip
    )
    target_link_libraries(${COMPONENT_LIB} INTERFACE volc_engine_rtc_lite)
else()
    message(FATAL_ERROR "VolcEngineRTCLite: unsupported target ${CONFIG_IDF_TARGET}")
endif()
```

- [ ] **Step 3: 创建 idf_component.yml**

```yaml
dependencies:
  idf:
    version: '>=5.0.0'
  espressif/zlib:
    version: '>=1.0.0'
```

- [ ] **Step 4: 提交 SDK 组件**

```bash
git add products/rtc_meeting_demo/components/volc_rtc_engine_lite/
git commit -m "feat(rtc_meeting_demo): add VolcEngineRTCLite SDK component"
```

---

### Task 3: 创建 WiFi 和 UI 模块（复用 meeting_demo）

**Files:**
- Create: `products/rtc_meeting_demo/main/wifi_init.c`
- Create: `products/rtc_meeting_demo/main/wifi_init.h`
- Create: `products/rtc_meeting_demo/main/ui_meeting.c`
- Create: `products/rtc_meeting_demo/main/ui_meeting.h`

- [ ] **Step 1: 复制 wifi_init.c/h（完全复用）**

```bash
cp products/meeting_demo/main/wifi_init.c products/rtc_meeting_demo/main/
cp products/meeting_demo/main/wifi_init.h products/rtc_meeting_demo/main/
```

- [ ] **Step 2: 复制 ui_meeting.c/h（微调：将 volc_rtc_session 替换为 rtc_session）**

复制 ui_meeting.h（无需修改）：
```bash
cp products/meeting_demo/main/ui_meeting.h products/rtc_meeting_demo/main/
```

复制 ui_meeting.c 后修改：
- `#include "volc_rtc_session.h"` → `#include "rtc_session.h"`
- `volc_rtc_session_start(CONFIG_MEETING_VOLC_BOT_ID, ...)` → `rtc_session_start(CONFIG_RTC_VOLC_APP_ID, ...)`
- `volc_rtc_session_stop()` → `rtc_session_stop()`
- 状态枚举：`VOLC_SESSION_*` → `RTC_SESSION_*`
- 按钮文字从 "Host Mode - Clary" 改为 "Host Mode - RTC AI"

- [ ] **Step 3: 提交**

```bash
git add products/rtc_meeting_demo/main/wifi_init.* products/rtc_meeting_demo/main/ui_meeting.*
git commit -m "feat(rtc_meeting_demo): add WiFi and UI modules (based on meeting_demo)"
```

---

### Task 4: 创建音频管线（pipeline_gmf 升级版）

**Files:**
- Create: `products/rtc_meeting_demo/main/pipeline_gmf.h`
- Create: `products/rtc_meeting_demo/main/pipeline_gmf.c`

**关键改动：**
- 录音：保持 16kHz 16bit mono（不再降采样到 8kHz）—— 因为 RTC SDK 支持 Opus 16kHz
- 播放：接收的音频从 8kHz 升级到 16kHz，不再需要 8k→16k 上采样
- 帧大小：16kHz 20ms = 640 bytes（录音和播放统一）

- [ ] **Step 1: 创建 pipeline_gmf.h**

```c
// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_gmf.h — Audio pipeline for rtc_meeting_demo.
//
// Recording: ES7210 → I2S RX (16kHz 32bit 2ch) → downmix+truncate → 16kHz 16bit mono
// Playback:  16kHz 16bit mono (from RTC) → 16kHz 32bit stereo → ES8311
//
// No 8kHz downsample/up-sample needed — both directions use 16kHz.

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bytes per 20ms frame of 16kHz 16bit mono PCM.
#define PIPELINE_FRAME_BYTES  640   // 16000 * 0.020 * 2 bytes

esp_err_t pipeline_gmf_hw_init(void);
esp_err_t pipeline_gmf_hw_deinit(void);

// Recorder: outputs 16kHz 16bit mono
esp_err_t pipeline_gmf_recorder_open(void);
int  pipeline_gmf_recorder_read(void *buf, size_t size);
esp_err_t pipeline_gmf_recorder_close(void);

// Player: accepts 16kHz 16bit mono input
esp_err_t pipeline_gmf_player_open(void);
esp_err_t pipeline_gmf_player_write(const void *buf, size_t size);
esp_err_t pipeline_gmf_player_close(void);

esp_err_t pipeline_gmf_set_volume(int volume);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 创建 pipeline_gmf.c**

基于 meeting_demo 的 pipeline_gmf.c，核心改动：

1. **录音路径不变**：ES7210 → I2S RX 32bit stereo → `conv_32s_to_16m()` → 16kHz 16bit mono（已经是 16kHz，不需要再降采样）
2. **播放路径简化**：删除 `expand_8k16m_to_16k32s()` 和 `s_play_prev_sample`，改为新的 `conv_16m_to_32s()`：
   - 输入 16kHz 16bit mono → 输出 16kHz 32bit stereo（直接位宽扩展 + 通道复制）
   - 不再需要线性插值上采样
3. **帧大小统一**：录音和播放都是 `PIPELINE_FRAME_BYTES` (640 bytes)

```c
// 16kHz 16bit mono → 16kHz 32bit stereo (direct expand, no resampling)
static void conv_16m_to_32s(const int16_t *src, int32_t *dst, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t v = (int32_t)src[i] << 16;
        dst[i * 2]     = v;  // L
        dst[i * 2 + 1] = v;  // R
    }
}
```

4. **player_task 改动**：直接 `conv_16m_to_32s()` 后写 I2S，不需要上采样
5. **ring buffer 大小调整**：`PLAY_RB_SIZE = 640 * 25`（16kHz 帧更大）
6. **其余 hw_init/deinit/recorder 完全复用**（ES8311 + ES7210 初始化代码相同）

- [ ] **Step 3: 提交**

```bash
git add products/rtc_meeting_demo/main/pipeline_gmf.*
git commit -m "feat(rtc_meeting_demo): add 16kHz bidirectional audio pipeline"
```

---

### Task 5: 创建 HTTP Bot 客户端（bot_client）

**Files:**
- Create: `products/rtc_meeting_demo/main/bot_client.h`
- Create: `products/rtc_meeting_demo/main/bot_client.c`

**职责：** 调用业务服务器的 `/start_voice_chat` 和 `/stop_voice_chat` 接口，获取 RTC 房间凭证。

- [ ] **Step 1: 创建 bot_client.h**

```c
// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// bot_client — HTTP client for the RTC AI business server.
// Calls /start_voice_chat and /stop_voice_chat to manage cloud AI agent sessions.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Credentials returned by /start_voice_chat
typedef struct {
    char room_id[128];   // RTC room ID
    char uid[64];        // User ID in the room
    char token[512];     // RTC authentication token
} rtc_room_info_t;

/**
 * @brief  Call business server to start a voice chat session.
 *         Blocks until HTTP response received.
 *
 * @param[in]  server_url  Base URL (e.g. "http://192.168.1.100:8080")
 * @param[out] info        Filled with room credentials on success.
 * @return  ESP_OK on success.
 */
esp_err_t bot_client_start_chat(const char *server_url, rtc_room_info_t *info);

/**
 * @brief  Call business server to stop a voice chat session.
 *
 * @param[in]  server_url  Base URL.
 * @param[in]  room_id     The room to stop.
 * @return  ESP_OK on success.
 */
esp_err_t bot_client_stop_chat(const char *server_url, const char *room_id);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 创建 bot_client.c**

核心实现：
- 使用 `esp_http_client` 发送 POST 请求
- `/start_voice_chat`：POST JSON `{"app_id": "xxx"}` → 解析 JSON 响应 `{"room_id": "...", "uid": "...", "token": "..."}`
- `/stop_voice_chat`：POST JSON `{"room_id": "xxx"}`
- JSON 解析使用 cJSON（ESP-IDF 内置组件）
- 超时设置：连接 5s，总请求 10s
- TLS 跳过证书验证（dev 环境）

```c
#include "bot_client.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "bot_client";

esp_err_t bot_client_start_chat(const char *server_url, rtc_room_info_t *info)
{
    // 1. Build URL: server_url + "/start_voice_chat"
    // 2. POST JSON: {"app_id": CONFIG_RTC_VOLC_APP_ID}
    // 3. Parse response JSON: room_id, uid, token
    // 4. Copy into info struct, return ESP_OK
    // Error handling: log and return ESP_FAIL
}

esp_err_t bot_client_stop_chat(const char *server_url, const char *room_id)
{
    // 1. Build URL: server_url + "/stop_voice_chat"
    // 2. POST JSON: {"room_id": room_id}
    // 3. Check response for success
    // Error handling: log and return ESP_FAIL
}
```

- [ ] **Step 3: 提交**

```bash
git add products/rtc_meeting_demo/main/bot_client.*
git commit -m "feat(rtc_meeting_demo): add HTTP bot client for business server"
```

---

### Task 6: 创建 RTC 会话管理（rtc_session）

**Files:**
- Create: `products/rtc_meeting_demo/main/rtc_session.h`
- Create: `products/rtc_meeting_demo/main/rtc_session.c`

**核心改动：** 用 `byte_rtc_*` API 替代 `volc_*` API。

- [ ] **Step 1: 创建 rtc_session.h**

```c
// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// rtc_session — RTC session manager using VolcEngineRTCLite SDK.
// Replaces volc_rtc_session (which used volc_conv_ai SDK).

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_SESSION_IDLE,        // Not started
    RTC_SESSION_CONNECTING,  // HTTP + RTC join in progress
    RTC_SESSION_CONNECTED,   // In room, audio flowing
    RTC_SESSION_ERROR,       // Connection or runtime error
} rtc_session_state_t;

typedef void (*rtc_session_state_cb_t)(rtc_session_state_t state, void *ctx);

/**
 * @brief  Start an RTC voice chat session.
 *
 * Flow: HTTP /start_voice_chat → get credentials → byte_rtc_create/init/join_room → audio loop
 *
 * @param app_id  Volcengine App ID.
 * @param cb      State-change callback (may be NULL).
 * @param ctx     Passed to @p cb.
 * @return  ESP_OK if start sequence kicked off.
 */
esp_err_t rtc_session_start(const char *app_id,
                             rtc_session_state_cb_t cb,
                             void *ctx);

/**
 * @brief  Stop the current session and release all resources.
 */
esp_err_t rtc_session_stop(void);

/**
 * @brief  Query current session state.
 */
rtc_session_state_t rtc_session_get_state(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 创建 rtc_session.c**

核心实现（约 350 行）：

1. **状态管理**：同 meeting_demo 的 session_t 模式，但 engine 类型从 `volc_engine_t` 改为 `byte_rtc_engine_t`
2. **Session start 流程**：
   ```
   pipeline_gmf_hw_init() + recorder_open() + player_open()
   → bot_client_start_chat(server_url, &room_info)  // HTTP 获取凭证
   → byte_rtc_create(app_id, &handlers)              // 创建引擎
   → byte_rtc_init(engine)                           // 初始化
   → byte_rtc_set_audio_codec(engine, AUDIO_CODEC_TYPE_OPUS)  // 设置 Opus
   → byte_rtc_join_room(engine, room_id, uid, token, &opts)   // 加入房间
   → 启动 audio_feed_task
   ```
3. **SDK 回调**：
   - `on_join_room_success` → set_state(RTC_SESSION_CONNECTED)
   - `on_room_error` → set_state(RTC_SESSION_ERROR)
   - `on_audio_data` → `pipeline_gmf_player_write()` （16kHz PCM 直接播放）
   - `on_message_received` → 解析字幕/状态 JSON，更新 UI
   - `on_quota_exceeded` → set_state(RTC_SESSION_ERROR)
4. **Audio feed task**：
   - 读 16kHz 16bit mono (640 bytes/20ms)
   - 直接 `byte_rtc_send_audio_data(engine, room, buf, len, &info)`
   - data_type = `AUDIO_DATA_TYPE_PCM`
   - 不再需要 16kHz→8kHz 降采样
5. **Session stop 流程**：
   ```
   stop feed task → byte_rtc_leave_room → byte_rtc_fini → byte_rtc_destroy
   → bot_client_stop_chat()
   → pipeline close/deinit
   ```

回调结构体初始化（全部 NULL 后按需赋值）：
```c
static byte_rtc_event_handler_t s_handlers = {
    .on_join_room_success = on_join_room_success,
    .on_room_error        = on_room_error,
    .on_audio_data        = on_audio_data,
    .on_message_received  = on_message_received,
    .on_quota_exceeded    = on_quota_exceeded,
    .on_fini_notify       = on_fini_notify,
};
```

- [ ] **Step 3: 提交**

```bash
git add products/rtc_meeting_demo/main/rtc_session.*
git commit -m "feat(rtc_meeting_demo): add RTC session manager with VolcEngineRTCLite SDK"
```

---

### Task 7: 创建入口和构建配置

**Files:**
- Create: `products/rtc_meeting_demo/main/main.c`
- Create: `products/rtc_meeting_demo/main/Kconfig.projbuild`
- Create: `products/rtc_meeting_demo/main/CMakeLists.txt`
- Create: `products/rtc_meeting_demo/main/idf_component.yml`

- [ ] **Step 1: 创建 Kconfig.projbuild**

```menuconfig
menu "RTC Meeting Demo Configuration"

    config MEETING_WIFI_SSID
        string "WiFi SSID"
        default "myssid"

    config MEETING_WIFI_PASSWORD
        string "WiFi Password"
        default "mypassword"

    comment "Business server (Python Flask)"

    config RTC_BOT_SERVER_URL
        string "Business Server URL"
        default "http://192.168.1.100:8080"
        help
            Base URL for the business server that calls StartVoiceChat API.

    comment "Volcengine RTC credentials"

    config RTC_VOLC_APP_ID
        string "Volcengine App ID"
        default ""
        help
            App ID for VolcEngineRTCLite SDK (obtained from Volcengine console).

endmenu
```

- [ ] **Step 2: 创建 main.c**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp_vocat.h"
#include "VolcEngineRTCLite.h"
#include "wifi_init.h"
#include "ui_meeting.h"

esp_err_t bsp_display_backlight_on(void);

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "RTC Meeting Demo starting (VolcEngineRTCLite v%s)",
             byte_rtc_get_version());

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Display
    lv_disp_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_lock(0);
    ui_meeting_create();
    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display ready, free heap: %lu", esp_get_free_heap_size());

    // WiFi
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — voice features disabled");
    } else {
        ESP_LOGI(TAG, "WiFi connected, ready");
    }
}
```

- [ ] **Step 3: 创建 main/CMakeLists.txt**

```cmake
file(GLOB_RECURSE MAIN_SRCS ./*.c ./*.cpp)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        driver
        esp_driver_i2s
        esp_driver_i2c
        espressif__esp_codec_dev
        esp_vocat
        espressif__esp_lvgl_port
        lvgl__lvgl
        volc_rtc_engine_lite
        esp_http_client
        json
        esp-tls
        mbedtls
        esp_timer
)
```

注意：`volc_conv_ai` 被替换为 `volc_rtc_engine_lite`，新增 `esp_http_client` 和 `json`（cJSON）。

- [ ] **Step 4: 创建 main/idf_component.yml**

```yaml
dependencies:
  idf:
    version: '>=5.0.0'
  espressif/zlib:
    version: '>=1.0.0'
```

- [ ] **Step 5: 提交**

```bash
git add products/rtc_meeting_demo/main/main.c products/rtc_meeting_demo/main/Kconfig.projbuild \
       products/rtc_meeting_demo/main/CMakeLists.txt products/rtc_meeting_demo/main/idf_component.yml
git commit -m "feat(rtc_meeting_demo): add entry point and build configuration"
```

---

### Task 8: 创建 Python 业务服务器

**Files:**
- Create: `products/rtc_meeting_demo/server/requirements.txt`
- Create: `products/rtc_meeting_demo/server/config.py`
- Create: `products/rtc_meeting_demo/server/rtc_api.py`
- Create: `products/rtc_meeting_demo/server/token_generator.py`
- Create: `products/rtc_meeting_demo/server/app.py`

**职责：** 接收 ESP32 客户端请求，调用火山 StartVoiceChat API，返回 RTC 房间凭证。

- [ ] **Step 1: 创建 requirements.txt**

```
flask>=3.0
requests>=2.31
cryptography>=42.0
```

- [ ] **Step 2: 创建 config.py**

```python
# Business server configuration
# Copy to config_local.py and fill in your credentials (gitignored)

# Volcengine credentials
VOLC_ACCESS_KEY_ID = ""
VOLC_SECRET_ACCESS_KEY = ""

# RTC app ID (same as CONFIG_RTC_VOLC_APP_ID on ESP32)
VOLC_APP_ID = ""

# Volcengine API endpoint
VOLC_API_BASE = "https://open.volcengineapi.com"
VOLC_SERVICE = "rtc"
VOLC_REGION = "cn-north-1"

# StartVoiceChat API
START_VOICE_CHAT_ACTION = "StartVoiceChat"
STOP_VOICE_CHAT_ACTION = "StopVoiceChat"

# Server config
SERVER_HOST = "0.0.0.0"
SERVER_PORT = 8080
```

- [ ] **Step 3: 创建 rtc_api.py**

实现火山签名 V4 + StartVoiceChat/StopVoiceChat API 调用：
- HMAC-SHA256 签名（Authorization: HMAC-SHA256 Credential=...）
- 请求体：`{"AppId": "...", "RoomId": "...", "UserId": "...", "BotId": "..."}`
- 解析响应获取 room_id, uid, token

```python
import json
import hashlib
import hmac
import datetime
import requests
from config import *

def _sign(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()

def _get_auth_headers(body: str) -> dict:
    """Generate Volcengine HMAC-SHA256 signed headers."""
    # Implementation follows Volcengine API signing V4 spec
    # ... (standard signing implementation)

def start_voice_chat(room_id: str, user_id: str) -> dict:
    """Call StartVoiceChat API, return {room_id, uid, token}."""
    body = json.dumps({
        "AppId": VOLC_APP_ID,
        "RoomId": room_id,
        "UserId": user_id,
    })
    headers = _get_auth_headers(body)
    resp = requests.post(
        f"{VOLC_API_BASE}/?Action={START_VOICE_CHAT_ACTION}",
        headers=headers,
        data=body,
        timeout=10
    )
    resp.raise_for_status()
    return resp.json()

def stop_voice_chat(room_id: str) -> dict:
    """Call StopVoiceChat API."""
    # Similar to start_voice_chat but with STOP_VOICE_CHAT_ACTION
```

- [ ] **Step 4: 创建 token_generator.py**

生成 RTC Token 用于客户端加入房间鉴权：
- 使用火山 RTC TokenBuilder 或 AccessToken 算法
- 基于 AppID + AppKey + RoomID + UID 生成

- [ ] **Step 5: 创建 app.py**

```python
from flask import Flask, request, jsonify
from rtc_api import start_voice_chat, stop_voice_chat
import uuid

app = Flask(__name__)

@app.route("/start_voice_chat", methods=["POST"])
def handle_start():
    """ESP32 calls this to create an AI agent session."""
    data = request.get_json()
    app_id = data.get("app_id", "")

    room_id = f"room_{uuid.uuid4().hex[:12]}"
    user_id = f"user_{uuid.uuid4().hex[:8]}"

    try:
        result = start_voice_chat(room_id, user_id)
        return jsonify({
            "room_id": room_id,
            "uid": user_id,
            "token": result.get("Token", ""),
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route("/stop_voice_chat", methods=["POST"])
def handle_stop():
    """ESP32 calls this to end the session."""
    data = request.get_json()
    room_id = data.get("room_id", "")

    try:
        stop_voice_chat(room_id)
        return jsonify({"success": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == "__main__":
    from config import SERVER_HOST, SERVER_PORT
    app.run(host=SERVER_HOST, port=SERVER_PORT)
```

- [ ] **Step 6: 提交**

```bash
git add products/rtc_meeting_demo/server/
git commit -m "feat(rtc_meeting_demo): add Python business server for StartVoiceChat API"
```

---

### Task 9: 构建验证

- [ ] **Step 1: 尝试首次构建**

```bash
cd products/rtc_meeting_demo
idf.py set-target esp32s3
idf.py build 2>&1 | tee build.log
```

Expected: 构建成功。如果失败，根据错误日志修复：
- 缺少头文件 → 检查 INCLUDE_DIRS
- 链接错误 → 检查 REQUIRES 列表
- 组件找不到 → 检查 EXTRA_COMPONENT_DIRS

- [ ] **Step 2: 修复构建问题直到通过**

常见问题清单：
- cJSON 组件：ESP-IDF 内置，`REQUIRES json` 即可
- zlib 组件：通过 `idf_component.yml` 中的 `espressif__zlib` 引入
- Opus codec：speaker 的 managed_components 中已有 `espressif__esp_audio_codec`

- [ ] **Step 3: 提交修复**

```bash
git add -u products/rtc_meeting_demo/
git commit -m "fix(rtc_meeting_demo): resolve build issues"
```

---

### Task 10: 端到端测试

- [ ] **Step 1: 启动 Python 业务服务器**

```bash
cd products/rtc_meeting_demo/server
pip install -r requirements.txt
# 创建 config_local.py 填入火山凭证
python app.py
```

- [ ] **Step 2: 烧录 ESP32-S3**

```bash
cd products/rtc_meeting_demo
# 创建 sdkconfig.local 填入 WiFi 和服务器地址
idf.py -p /dev/ttyUSB0 flash monitor
```

- [ ] **Step 3: 功能验证**

1. 设备启动 → 显示 "Meeting Assistant" + [Start Meeting]
2. 点 [Start Meeting] → 进入 MEETING 状态
3. 点 [Host Mode] → HTTP 请求服务器 → 获取凭证 → RTC 连接
4. 状态变为 "*Connected" (绿色)
5. 对着麦克风说话 → Agent 回复（16kHz Opus 音质）
6. 点 [Exit Host Mode] → 离开房间 → 回到 MEETING 状态

---

## 自审清单

**1. Spec 覆盖：**
- ✅ 新产品目录 `products/rtc_meeting_demo/` — Task 1
- ✅ VolcEngineRTCLite SDK 替代 volc_conv_ai_sdk — Task 2, 6
- ✅ HTTP bot_client 调用业务服务器 — Task 5
- ✅ 音频升级到 16kHz Opus — Task 4, 6
- ✅ UI 保持一致 — Task 3
- ✅ Python 业务服务器 — Task 8
- ✅ 构建配置 — Task 7
- ✅ 不修改 meeting_demo — 所有文件都在新目录

**2. 占位符检查：**
- bot_client.c 和 rtc_session.c 给出了完整流程伪代码，实现时需按框架填充
- server/rtc_api.py 签名实现需参考火山官方 SDK 填充（标准 HMAC-SHA256 V4）

**3. 类型一致性：**
- `rtc_session_state_t` (IDLE/CONNECTING/CONNECTED/ERROR) 与 UI 的状态映射一致
- `rtc_room_info_t` 字段与 byte_rtc_join_room 参数一致
- `PIPELINE_FRAME_BYTES = 640` 与 byte_rtc_send_audio_data 的数据长度一致
