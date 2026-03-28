# Meeting Assistant App — 技术方案文档

> **产品:** ESP-VoCat (喵伴) · ESP32-S3
> **目标芯片:** ESP32-S3
> **文档版本:** v1.1 · 2026-03-28
> **关联分支:** `feat/meeting-assistant`

---

## 目录

1. [背景与目标](#一背景与目标)
2. [开发环境与构建规范](#二开发环境与构建规范)
3. [兼容性分析](#三兼容性分析)
4. [整体方案设计](#四整体方案设计)
5. [ClarityX API（会议监听后端）](#五clarityx-api会议监听后端)
6. [阶段一：Meeting RTC Demo（独立验证）](#六阶段一meeting-rtc-demo独立验证)
7. [阶段二：Meeting App（集成到 Speaker）](#七阶段二meeting-app集成到-speaker)
8. [UI 布局规范](#八ui-布局规范)
9. [执行路线图](#九执行路线图)
10. [关键风险与对策](#十关键风险与对策)
11. [各步骤测试验收用例](#十一各步骤测试验收用例)

---

## 一、背景与目标

### 功能描述

**Meeting Assistant App** 是面向 ESP-VoCat (喵伴) 的会议辅助应用，支持两种工作模式：

| 模式 | 描述 |
|------|------|
| **会议监听模式** | 接入会议音频流，监听并显示连接状态 |
| **会议主持人模式** | 切换至火山引擎硬件对话智能体（RTC 方案），支持实时 AI 语音问答 |

### 参考资料

| 资源 | 地址 |
|------|------|
| 火山引擎官方文档 | https://www.volcengine.com/docs/6348/1806623?lang=zh |
| 官方喵伴 Demo | https://www.volcengine.com/docs/6348/1806624?lang=zh |
| 官方 SDK 源码 | https://github.com/volcengine/ConversationalAI-Embedded-Kit-2.0 |

---

## 二、开发环境与构建规范

### 基本信息

| 项目 | 值 |
|------|-----|
| **目标芯片** | ESP32-S3 |
| **ESP-IDF 版本** | v5.5 |
| **IDF 路径** | `~/esp/esp-idf/` |
| **产品目录** | `products/speaker/` |
| **串口** | `/dev/cu.usbmodem*`（USB CDC，自动检测；reset 后端口名可能变化，如 usbmodem101） |
| **ccache** | `/opt/homebrew/bin/ccache`，IDF 5.5 自动启用（`IDF_CCACHE_ENABLE`） |

### 构建耗时参考

| 场景 | 耗时 |
|------|------|
| 无变化构建（build.sh 修复后） | ~0.1s |
| 单文件改动（1s 编译 + 26s 链接） | ~27s |
| app-flash（USB CDC） | ~58s |

### ⚠️ ESP-IDF 5.5 构建系统 Bug

> **CRITICAL:** ESP-IDF 5.5 存在 bug——`CUSTOM_COMMAND` stamp 文件被声明为 output 但从未创建，导致每次构建触发 50+ 个伪目标重新构建。
>
> 项目 `build.sh` 已包含针对此 bug 的修复，**始终通过 `build.sh` 构建，不要裸跑 `idf.py build`**。

### ⚠️ Flash 方式：OpenOCD JTAG（禁止用 esptool）

> **CRITICAL:** 固件崩溃（hang/WDT crash）时 USB-CDC 栈失效，`esptool` 无法连接。**始终使用 OpenOCD JTAG 烧录**，无论固件状态如何均可正常工作。

```bash
# OpenOCD 路径
OPENOCD=~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250422/openocd-esp32/bin/openocd

# 烧录命令（adapter: ESP32-S3 built-in USB-JTAG）
$OPENOCD -f board/esp32s3-builtin.cfg \
  -c "program_esp brookesia_speaker.bin 0xb0000 verify reset exit"
```

烧录+reset 后 USB-CDC 重新枚举，端口名可能变化，`build.sh` 会自动检测。

### 分区布局与烧录范围

> **开发期间只需烧录 `brookesia_speaker.bin`**，其他分区为静态资源，无需每次重刷。

| 分区 | 地址 | 是否频繁变化 |
|------|------|-------------|
| `brookesia_speaker.bin` | `0xb0000` | ✅ 是（每次编译） |
| `srmodels` | 静态 | ❌ 否 |
| `anim_*` | 静态 | ❌ 否 |
| `spiffs` | 静态 | ❌ 否 |

### 开发环境激活

```bash
export IDF_PATH=~/esp/esp-idf
source ~/.espressif/python_env/idf5.5_py3.12_env/bin/activate
export PATH="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin:\
$HOME/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/xtensa-esp-elf-gdb/bin:\
$HOME/.espressif/tools/esp-rom-elfs/20241011/:$IDF_PATH/tools:$PATH"
```

> **注意:** `source $IDF_PATH/export.sh` 在 Claude Code 子 shell 中无法正确设置 PATH，需手动激活 venv 并设置 PATH。

---

## 三、兼容性分析（音频框架）

### 两套音频框架对比

| 维度 | 当前架构（ESP-GMF） | 火山高性能方案（ESP-ADF） |
|------|---------------------|--------------------------|
| **音频框架** | esp-gmf（esp_gmf_afe, esp_gmf_pipeline） | esp-adf（audio_pipeline, i2s_stream, algorithm_stream） |
| **编解码** | ES7210(ADC) + ES8311(DAC)，GMF pipeline 管理 | ESP-ADF board_hal 抽象 |
| **I2S 采样** | 16kHz/32bit/2ch（GMF 管理） | 16kHz/32bit，由 ADF i2s_stream 独占 |
| **传输协议** | WebSocket over TCP（Coze MQTT） | RTC UDP（VOLC_MODE_RTC） |
| **依赖库** | esp-gmf, esp-sr, esp-codec | esp-adf, libVolcEngineRTCLite.a（预编译） |

### 核心冲突

> 两套框架都需要**独占 I2S 驱动和 codec**，无法共存在同一个 `audio_manager_init()` 中。

解决策略：利用现有 `audio_manager_suspend()` API（`audio_processor.h:64`）在 Host Mode 切换时挂起/恢复 GMF pipeline，由 ADF pipeline 独占 I2S。

---

## 四、整体方案设计

### 分阶段策略

```
Phase 1: products/meeting_demo/     ← 独立验证火山 RTC pipeline
                ↓ 验证通过
Phase 2: apps/brookesia_app_meeting/ ← 以 App 形式嵌入 Speaker
```

**不在现有 Speaker 系统内直接硬嵌**，保证两套音频框架的隔离，符合项目现有分层结构。

### App 状态机

```
IDLE ──[Start Meeting]──► MEETING
MEETING ──[Host Mode]──► HOST
MEETING ──[Stop Meeting]──► IDLE
HOST ──[Exit Host Mode]──► MEETING
```

---

## 五、ClarityX API（会议监听后端）

会议监听模式的后端服务由 **ClarityX**（`ailabs.mayfair-inc.net`）提供，负责创建/终止会议 session。

### 基础配置

| 配置项 | 值 |
|--------|-----|
| Kconfig key | `CONFIG_MEETING_CLARITYX_SERVER_URL` |
| Base URL | `https://ailabs.mayfair-inc.net/voice-api` |
| 服务端日志 SSH | `ssh root@192.168.191.214 -p 8122` → `tail -f /opt/voice-api/server.log` |

### API 接口

| 操作 | Method | 完整路径（相对 host） |
|------|--------|----------------------|
| 创建 Session | POST | `/voice-api/api/session` |
| 结束 Session | POST | `/voice-api/api/session/{session_id}/end` |

### URL 构造规则

```
base = "https://ailabs.mayfair-inc.net/voice-api"

createSession: base + "/api/session"
               → https://ailabs.mayfair-inc.net/voice-api/api/session

endSession:    base + "/api/session/{id}/end"
               → https://ailabs.mayfair-inc.net/voice-api/api/session/abc123/end
```

> ⚠️ **注意 double-path 陷阱:** base 末尾已含 `/voice-api`，拼接时不要再加，否则路径变为 `/voice-api/voice-api/api/session`。

### Kconfig 配置

```kconfig
config MEETING_CLARITYX_SERVER_URL
    string "ClarityX Server Base URL"
    default "https://ailabs.mayfair-inc.net/voice-api"
    help
        会议监听模式后端服务地址。
        createSession  = this_value + "/api/session"
        endSession     = this_value + "/api/session/{id}/end"
```

### C 接口设计（会议监听模式）

```c
// clarityx_session.h

/**
 * @brief 创建 ClarityX 会议 session
 * @param[out] session_id  输出缓冲区，存储返回的 session ID（调用方分配 ≥64 字节）
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t clarityx_session_create(char *session_id, size_t session_id_len);

/**
 * @brief 结束 ClarityX 会议 session
 * @param session_id  由 clarityx_session_create 返回的 ID
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t clarityx_session_end(const char *session_id);
```

---

## 六、阶段一：Meeting RTC Demo（独立验证）

### 目录结构

```
products/meeting_demo/
├── CMakeLists.txt
├── sdkconfig.defaults                  # 复用 speaker sdkconfig.defaults.esp32s3
├── sdkconfig.defaults.esp32s3
├── partitions.csv
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild               # VOLC 配置项（instance_id/bot_id 等）
    ├── main.cpp                        # app_main: WiFi→SNTP→UI→volc_start
    ├── volc_rtc_session.h              # Session 状态机与 API 声明
    ├── volc_rtc_session.c              # 封装 volc_create/start/stop/destroy
    ├── pipeline_adf.h                  # ADF 版音频 pipeline 声明
    ├── pipeline_adf.c                  # ADF pipeline（ESP-VoCat 硬件适配）
    └── ui_meeting.cpp                  # 简易 LVGL UI（IDLE/MEETING/HOST 三状态）
```

> **注意:** 火山 SDK 组件（`volc_conv_ai`）通过 `idf_component.yml` 或 git submodule 引入，放在 `products/meeting_demo/components/volc_conv_ai/`。

### 核心接口设计

#### `pipeline_adf.h` — ADF 音频 Pipeline

Demo 阶段直接使用 ADF pipeline，不影响现有 GMF 代码：

```c
// pipeline_adf.h
// Demo 专用，不影响现有 GMF pipeline

typedef void *recorder_pipeline_handle_t;
typedef void *player_pipeline_handle_t;

/**
 * @brief 打开录音 pipeline: i2s_stream → algorithm_stream(AEC) → raw_stream
 *        适配 ESP-VoCat: ES7210 ADC, CODEC_ADC_I2S_PORT
 */
recorder_pipeline_handle_t recorder_pipeline_open(void);

/**
 * @brief 打开播放 pipeline: raw_stream → i2s_stream
 *        适配 ESP-VoCat: ES8311 DAC
 */
player_pipeline_handle_t player_pipeline_open(void);

void recorder_pipeline_close(recorder_pipeline_handle_t handle);
void player_pipeline_close(player_pipeline_handle_t handle);
```

> **硬件适配要点:** 直接使用 BSP 宏（`CODEC_ADC_I2S_PORT` 等）绕过 `audio_board_init()`，
> 因为 ESP-VoCat 无对应的 ESP-ADF board 支持文件。

#### `volc_rtc_session.h` — 火山 RTC Session 封装

```c
// volc_rtc_session.h

typedef enum {
    VOLC_SESSION_IDLE,
    VOLC_SESSION_CONNECTING,
    VOLC_SESSION_CONNECTED,
    VOLC_SESSION_ERROR,
} volc_session_state_t;

typedef void (*volc_session_state_cb_t)(volc_session_state_t state, void *ctx);

/**
 * @brief 启动火山 RTC 对话 session
 * @param bot_id       智能体 Bot ID（从 Kconfig 读取）
 * @param cb           状态变化回调（用于更新 UI 底部状态文本）
 * @param ctx          用户数据，透传给 cb
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t volc_rtc_session_start(const char *bot_id,
                                  volc_session_state_cb_t cb,
                                  void *ctx);

/**
 * @brief 停止当前 RTC session 并释放资源
 */
esp_err_t volc_rtc_session_stop(void);

/**
 * @brief 获取当前 session 状态
 */
volc_session_state_t volc_rtc_session_get_state(void);
```

#### `Kconfig.projbuild` — 配置项

```kconfig
menu "Meeting RTC Demo Configuration"

    config VOLC_INSTANCE_ID
        string "Volcano Engine Instance ID"
        default ""
        help
            從火山引擎控制台获取的 Instance ID。

    config VOLC_BOT_ID
        string "Volcano Engine Bot ID"
        default ""
        help
            智能体 Bot ID。

    config VOLC_APP_ID
        string "Volcano Engine App ID"
        default ""

    config VOLC_TOKEN
        string "Volcano Engine Token"
        default ""

    config WIFI_SSID
        string "WiFi SSID"
        default "myssid"

    config WIFI_PASSWORD
        string "WiFi Password"
        default "mypassword"

endmenu
```

#### `main.cpp` — 入口流程

```cpp
// main.cpp 启动流程（伪代码）
extern "C" void app_main(void) {
    // 1. 初始化 NVS
    nvs_flash_init();

    // 2. 初始化 WiFi，等待连接
    wifi_init_sta(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

    // 3. SNTP 时间同步
    sntp_sync_time();

    // 4. 初始化 LVGL 显示（复用 meeting_demo 极简显示初始化）
    ui_meeting_init();

    // 5. 等待用户操作（通过 LVGL 按钮回调触发 volc_rtc_session_start）
}
```

---

## 七、阶段二：Meeting App（集成到 Speaker）

### 目录结构

```
apps/brookesia_app_meeting/
├── CMakeLists.txt
├── idf_component.yml
├── include/
│   └── esp_brookesia_app_meeting.hpp    # MeetingApp 类声明
└── src/
    ├── esp_brookesia_app_meeting.cpp    # App 生命周期实现
    ├── meeting_ui.cpp                   # LVGL UI（3 状态）
    ├── meeting_ui.h
    ├── volc_rtc_bridge.cpp              # GMF ⟷ ADF 切换桥接层
    └── volc_rtc_bridge.h
```

### 音频切换方案

```
Speaker 正常模式:
  audio_manager（GMF pipeline: AFE → Coze Agent）

进入 Host Mode:
  1. audio_manager_suspend(true)    ← 挂起 GMF pipeline，释放 I2S
  2. volc_rtc_pipeline_start()     ← 启动 ADF pipeline（独占 I2S）
  3. volc_rtc_session_start()      ← 建立 RTC session

退出 Host Mode:
  1. volc_rtc_session_stop()       ← 断开 RTC session
  2. volc_rtc_pipeline_stop()      ← 停止 ADF pipeline
  3. audio_manager_suspend(false)  ← 恢复 GMF pipeline
```

> `audio_manager_suspend()` 已在 `core/brookesia_core/ai_framework/agent/audio_processor.h:64` 实现，专为此类场景设计。

### `volc_rtc_bridge.h` — GMF/ADF 切换桥接

```cpp
// volc_rtc_bridge.h
#pragma once
#include "esp_err.h"

/**
 * @brief 进入 Host Mode：挂起 GMF pipeline，启动 ADF + 火山 RTC session
 * @param state_cb 状态变化回调，用于更新底部状态 Label
 * @param ctx      透传给 state_cb 的用户数据
 */
esp_err_t volc_rtc_bridge_enter_host_mode(volc_session_state_cb_t state_cb, void *ctx);

/**
 * @brief 退出 Host Mode：停止 RTC session，停止 ADF pipeline，恢复 GMF pipeline
 */
esp_err_t volc_rtc_bridge_exit_host_mode(void);
```

### `MeetingApp` 类设计

```cpp
namespace esp_brookesia::apps {

class MeetingApp : public systems::speaker::App {
public:
    MeetingApp();

protected:
    bool run() override;     // 创建 UI，初始化为 IDLE 状态
    bool back() override;    // 如在会议中先 stopMeeting，再 notifyCoreClosed
    bool close() override;   // 确保 volc_rtc_session_stop
    bool pause() override;   // 进入后台时暂停 Host Mode（若在 HOST 状态）
    bool resume() override;  // 恢复

private:
    enum class State { IDLE, MEETING, HOST };

    void onStartMeeting();   // IDLE → MEETING：启动会议监听
    void onEnterHostMode();  // MEETING → HOST：suspend GMF，start volc RTC
    void onExitHostMode();   // HOST → MEETING：stop volc RTC，resume GMF
    void onStopMeeting();    // MEETING → IDLE：停止会议

    static void onSessionStateChanged(volc_session_state_t state, void *ctx);
    void updateUI(State state);

    State       _state        = State::IDLE;
    lv_obj_t   *_title_label  = nullptr;
    lv_obj_t   *_status_label = nullptr;
    lv_obj_t   *_btn_start    = nullptr;   // Start Meeting
    lv_obj_t   *_btn_host     = nullptr;   // Host Mode
    lv_obj_t   *_btn_stop     = nullptr;   // Stop Meeting
    lv_obj_t   *_btn_exit_host = nullptr;  // Exit Host Mode
};

} // namespace esp_brookesia::apps
```

### 注册到 Speaker 系统

在 `products/speaker/main/modules/system.cpp` 中：

```cpp
// 包含头文件
#include "esp_brookesia_app_meeting.hpp"

// 在 ordered_app_names 中添加（根据期望的 Launcher 图标位置排列）
std::vector<std::string> ordered_app_names = {
    // ... 其他 App ...
    "Meeting",  // ← 新增
};
```

---

## 八、UI 布局规范

### 360×360 圆屏布局

```
┌──────────── 360px ────────────┐
│                                │  ← 圆屏边缘留空（~60px）
│   ┌──── Title Label ──────┐   │  y=80, 居中, font=20px bold
│   │  Meeting Assistant    │   │
│   └───────────────────────┘   │
│                                │
│        ┌──── 按钮区域 ──────┐  │  y=160~220, 居中
│        │ IDLE:              │  │
│        │   [Start Meeting]  │  │  单按钮，宽200px，高50px
│        │ MEETING:           │  │
│        │  [Host][Stop]      │  │  两按钮横排，各120px，间距10px
│        │ HOST:              │  │
│        │  [Exit Host Mode]  │  │  单按钮，宽200px，高50px
│        └────────────────────┘  │
│                                │
│   ┌──── Status Label ─────┐   │  y=280, 居中, font=14px
│   │  ● Connecting...      │   │  仅 MEETING/HOST 状态显示
│   └───────────────────────┘   │
└────────────────────────────────┘
```

### 各状态 UI 内容

| 状态 | 头部标题 | 按钮 | 底部状态 |
|------|----------|------|----------|
| IDLE | `Meeting Assistant` | `Start Meeting` | 隐藏 |
| MEETING | `Meeting in Progress` | `Host Mode` + `Stop Meeting` | 服务器连接状态 |
| HOST | `Host Mode - Clary` | `Exit Host Mode` | 服务器连接状态 |

### 状态文本映射

| `volc_session_state_t` | 显示文本 | 颜色 |
|------------------------|----------|------|
| `VOLC_SESSION_IDLE` | `● Idle` | 灰色 |
| `VOLC_SESSION_CONNECTING` | `● Connecting...` | 黄色 |
| `VOLC_SESSION_CONNECTED` | `● Connected` | 绿色 |
| `VOLC_SESSION_ERROR` | `● Connection Error` | 红色 |

---

## 九、执行路线图

### Phase 1：Meeting RTC Demo（独立验证）

| Step | 任务 | 输出 |
|------|------|------|
| **1.1** | 创建 `products/meeting_demo/` 目录，配置 CMakeLists.txt 和 Kconfig | 可编译的空壳工程 |
| **1.2** | 引入火山 SDK（`volc_conv_ai`）作为组件，验证链接通过 | 链接成功，无符号冲突 |
| **1.3** | 移植 `pipeline_adf.c`，适配 ESP-VoCat 硬件（ES7210+ES8311，绕过 `audio_board_init`） | codec 初始化正常，音频 pipeline 可录音/播放 |
| **1.4** | 实现 `volc_rtc_session.c`，封装 volc API，接入状态回调 | Session 可建立/断开 |
| **1.5** | 实现极简 LVGL UI（3 状态），端到端语音通路验证 | Host Mode 语音问答正常 |

### Phase 2：Meeting App（集成到 Speaker）

| Step | 任务 | 输出 |
|------|------|------|
| **2.1** | 实现 `volc_rtc_bridge`（GMF suspend/resume + ADF start/stop） | 切换无 I2S 冲突 |
| **2.2** | 实现 `MeetingApp`（生命周期 + UI 3 状态） | App 可在 Launcher 启动 |
| **2.3** | 在 `system.cpp` `ordered_app_names` 中注册 | Launcher 显示 Meeting 图标 |
| **2.4** | 联调：Host Mode = volc RTC，会议监听模式（TBD） | 完整App功能验收 |

---

## 十、关键风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| **ADF i2s_stream 与 GMF 冲突** | 高 | `audio_manager_suspend(true)` 释放 I2S 后再启动 ADF pipeline；退出时反向操作 |
| **`libVolcEngineRTCLite.a` 与 esp-gmf 链接冲突** | 高 | Phase 1 先独立构建验证；Phase 2 用独立组件 REQUIRES 隔离 |
| **ESP-VoCat 无 ESP-ADF board 支持** | 中 | `pipeline_adf.c` 直接使用 `CODEC_ADC_I2S_PORT` 等 BSP 宏，绕过 `audio_board_init()` |
| **PSRAM RAM < 300KB（高性能方案要求）** | 中 | 编译时 `CONFIG_VOLC_RTC_MODE=y`，audio buffer 走 SPIRAM；需实测 heap 余量 |
| **LVGL 线程安全** | 低 | Session 状态回调（非 LVGL 任务）中更新 UI 须用 `LvLockGuard` 保护 |

---

## 十一、各步骤测试验收用例

---

### Phase 1 · Step 1.1：创建 meeting_demo 工程框架

**验收条件：**

```bash
cd products/meeting_demo
idf.py set-target esp32s3
idf.py build
```

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-1.1-1 | 设置目标芯片 esp32s3 | 无报错，生成 `sdkconfig` |
| TC-1.1-2 | 编译空壳工程 | 编译成功，生成 `.bin` 文件，无 undefined reference |
| TC-1.1-3 | Kconfig 配置项可见 | `idf.py menuconfig` 中可见 "Meeting RTC Demo Configuration" 菜单 |
| TC-1.1-4 | 分区表正确 | `idf.py partition-table` 输出包含正确的 app/storage 分区 |

---

### Phase 1 · Step 1.2：引入火山 SDK 并验证链接

**验收条件：**

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-1.2-1 | 添加 `volc_conv_ai` 组件后可编译 | `idf.py build` 成功，`libVolcEngineRTCLite.a` 被链接 |
| TC-1.2-2 | 无符号冲突 | 编译日志中无 `multiple definition` 或 `undefined reference` 错误 |
| TC-1.2-3 | 火山 API 可调用 | 在 `main.cpp` 中调用 `volc_get_version()` 或类似 API，日志输出版本号 |
| TC-1.2-4 | PSRAM 可用 | 设备串口输出 SPIRAM 初始化成功日志，`esp_get_free_heap_size()` > 200KB |

---

### Phase 1 · Step 1.3：ADF Audio Pipeline 适配

**验收条件：**

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-1.3-1 | Codec 初始化成功 | 串口日志输出 ES7210 (ADC) 和 ES8311 (DAC) 初始化成功（无 I2C 错误） |
| TC-1.3-2 | 录音 pipeline 可启动 | `recorder_pipeline_open()` 返回非 NULL 句柄，无报错 |
| TC-1.3-3 | 播放 pipeline 可启动 | `player_pipeline_open()` 返回非 NULL 句柄，无报错 |
| TC-1.3-4 | 录音→播放回环测试 | 对设备说话，扬声器实时回放（用于验证 AEC pipeline 通路正常）；延迟 < 300ms |
| TC-1.3-5 | AEC 消回声有效 | 播放音频时录音数据中扬声器声音被消除，无明显反馈啸叫 |
| TC-1.3-6 | Pipeline 可正常关闭 | `recorder_pipeline_close()` / `player_pipeline_close()` 后无内存泄漏（`heap_caps_get_free_size(MALLOC_CAP_DEFAULT)` 恢复至 open 前水平，误差 < 1KB） |

---

### Phase 1 · Step 1.4：火山 RTC Session 封装

**前置条件：** Kconfig 中已填写有效的 `VOLC_BOT_ID`, `VOLC_TOKEN`, `VOLC_INSTANCE_ID`，设备已连接 WiFi。

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-1.4-1 | Session 建立成功 | `volc_rtc_session_start()` 返回 `ESP_OK`；状态回调顺序: `IDLE → CONNECTING → CONNECTED` |
| TC-1.4-2 | Session 状态查询正确 | 连接后 `volc_rtc_session_get_state()` 返回 `VOLC_SESSION_CONNECTED` |
| TC-1.4-3 | Session 断开成功 | `volc_rtc_session_stop()` 返回 `ESP_OK`；状态回调返回 `IDLE`；无崩溃或 panic |
| TC-1.4-4 | 无效凭证错误处理 | 使用错误 Token 时，状态回调返回 `VOLC_SESSION_ERROR`，串口输出错误日志 |
| TC-1.4-5 | WiFi 断线重连 | WiFi 意外断开后，状态回调返回 `ERROR`；WiFi 恢复后可重新调用 `volc_rtc_session_start()` 成功连接 |
| TC-1.4-6 | 重复调用 stop 安全 | 无 Session 时调用 `volc_rtc_session_stop()` 返回 `ESP_OK`（或 `ESP_ERR_INVALID_STATE`），无崩溃 |

---

### Phase 1 · Step 1.5：端到端 Host Mode 语音问答验证

**前置条件：** Steps 1.1-1.4 全部通过。

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-1.5-1 | UI 初始状态正确 | 开机后显示 IDLE 状态：标题 "Meeting Assistant Agent"，仅显示 "Start Meeting" 按钮 |
| TC-1.5-2 | 进入 MEETING 状态 | 点击 "Start Meeting" → 标题变为 "Meeting in Progress"，显示 "Host Mode" 和 "Stop Meeting" 按钮，底部显示连接状态 |
| TC-1.5-3 | 进入 HOST MODE | 点击 "Host Mode" → 标题变为 "Host Mode - Clary"，底部状态经过 "Connecting..." 最终显示 "Connected" |
| TC-1.5-4 | 语音问答功能 | 在 HOST 状态对设备提问（如"今天天气怎么样"），设备在 2 秒内开始语音回复 |
| TC-1.5-5 | 多轮对话 | 连续提问 3 轮，AI 均能正常回答，无中断或卡顿 |
| TC-1.5-6 | 退出 HOST MODE | 点击 "Exit Host Mode" → 回到 MEETING 状态，RTC session 断开，状态文本消失或重置 |
| TC-1.5-7 | 停止会议 | 在 MEETING 状态点击 "Stop Meeting" → 回到 IDLE 状态，所有状态文本隐藏 |
| TC-1.5-8 | 系统稳定性 | 完整跑完 IDLE→MEETING→HOST→MEETING→IDLE 循环 3 次，无崩溃、panic 或内存泄漏（每次循环后 free heap 变化 < 2KB） |

---

### Phase 2 · Step 2.1：GMF/ADF 音频切换桥接

**前置条件：** Phase 1 全部验证通过；在 Speaker 产品（`products/speaker`）上编译。

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-2.1-1 | GMF 挂起成功 | 调用 `volc_rtc_bridge_enter_host_mode()` 后，`audio_manager_suspend(true)` 执行无报错，串口日志确认 GMF pipeline 暂停 |
| TC-2.1-2 | ADF pipeline 无 I2S 冲突 | GMF 挂起后启动 ADF pipeline，无 "i2s driver already installed" 错误 |
| TC-2.1-3 | GMF 恢复成功 | 调用 `volc_rtc_bridge_exit_host_mode()` 后，GMF pipeline 恢复，Coze Agent 可正常唤醒响应 |
| TC-2.1-4 | 切换无音频爆破声 | 进入/退出 Host Mode 时，扬声器无明显爆破音（主观评估：切换时无突然的"噗"声） |
| TC-2.1-5 | 连续切换稳定性 | 连续进行 5 次 enter/exit host mode 切换，无崩溃，heap 无持续增长 |

---

### Phase 2 · Step 2.2：MeetingApp 生命周期与 UI

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-2.2-1 | App 可在 Launcher 启动 | Launcher 中点击 Meeting 图标，`run()` 被调用，UI 正确显示 IDLE 状态 |
| TC-2.2-2 | 返回键行为（IDLE） | IDLE 状态下按返回键，App 调用 `notifyCoreClosed()`，回到 Launcher |
| TC-2.2-3 | 返回键行为（MEETING） | MEETING 状态下按返回键，先执行 `onStopMeeting()`（回到 IDLE）再关闭 App |
| TC-2.2-4 | 返回键行为（HOST） | HOST 状态下按返回键，先执行 `onExitHostMode()`（回到 MEETING，RTC 断开），再 `onStopMeeting()`，再关闭 App |
| TC-2.2-5 | App 被切换到后台（pause） | 在 HOST 状态打开其他 App（触发 pause），RTC session 应暂停；回到 App 后（resume）可继续 |
| TC-2.2-6 | 反复启动/关闭无泄漏 | 连续启动/关闭 MeetingApp 5 次（每次停留在 IDLE），heap 无累计增长（变化 < 1KB） |
| TC-2.2-7 | LVGL 线程安全 | RTC 状态回调更新底部 Label 时，无 LVGL assert 失败或 mutex 死锁 |

---

### Phase 2 · Step 2.3：Launcher 注册验证

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-2.3-1 | Launcher 显示 Meeting 图标 | `ordered_app_names` 添加后，`initAppFromRegistry()` 日志中包含 Meeting App |
| TC-2.3-2 | App 图标位置正确 | Meeting 图标在 `ordered_app_names` 中的对应位置显示 |
| TC-2.3-3 | 其他 App 不受影响 | 注册 Meeting App 后，原有 App（Settings、Calculator 等）正常启动，无异常 |

---

### Phase 2 · Step 2.4：完整联调验收

**最终验收——完整功能回归：**

| 用例 ID | 描述 | 期望结果 |
|---------|------|----------|
| TC-2.4-1 | 完整状态机流转 | IDLE→MEETING→HOST→MEETING→IDLE 完整流程，每个状态 UI 内容与规范一致 |
| TC-2.4-2 | Host Mode 语音问答（集成版） | 在 Speaker 产品中 Host Mode 语音问答正常，品质与 Phase 1 Demo 一致 |
| TC-2.4-3 | Coze Agent 与 Host Mode 互不干扰 | Host Mode 退出后，原 Coze Agent 唤醒词"小喵小喵"可正常触发 |
| TC-2.4-4 | 长时间稳定性 | MEETING + HOST 状态持续运行 30 分钟，无崩溃、无 WiFi 断连（或断连自动恢复） |
| TC-2.4-5 | 低电量行为 | 电量低于 10% 时，进入 Host Mode 可正常工作（功耗评估：RTC 模式下电流 < 500mA） |
| TC-2.4-6 | 全流程内存稳定 | 跑完 10 个完整 IDLE→MEETING→HOST→IDLE 循环后，`esp_get_free_heap_size()` 波动 < 5KB |

---

## 附录

### A. 关键 API 位置速查

| API | 文件 | 说明 |
|-----|------|------|
| `audio_manager_suspend()` | `core/brookesia_core/ai_framework/agent/audio_processor.h:64` | GMF pipeline 挂起/恢复 |
| `Speaker::installAppFromRegistry()` | `core/brookesia_core/systems/speaker/esp_brookesia_speaker.hpp` | App 安装入口 |
| `FunctionDefinitionList::requestInstance()` | AI framework | Function Calling 注册（如需语音控制） |
| `LvLockGuard` | `core/brookesia_core/gui/` | LVGL 互斥锁保护 |

### B. 火山引擎配置速查

在 `idf.py menuconfig` → `Meeting RTC Demo Configuration` 中配置：

```
VOLC_INSTANCE_ID  = <从火山控制台获取>
VOLC_BOT_ID       = <智能体 Bot ID>
VOLC_APP_ID       = <App ID>
VOLC_TOKEN        = <Token>
```

### C. 开发环境与烧录

详见 [第二章：开发环境与构建规范](#二开发环境与构建规范)。
