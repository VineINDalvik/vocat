# ESP-Brookesia (喵伴) 项目指南

## 项目概述

ESP-Brookesia 是基于 ESP-IDF 的嵌入式 AI 设备软件框架，支持 Phone 和 Speaker 两种产品形态。当前主要围绕 **Speaker（ESP-VoCat）** 产品开发，目标芯片为 ESP32-S3。

## 架构分层

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│  ┌──────────────────────────┐ ┌───────────────────────┐ │
│  │     System Framework     │ │    AI Framework       │ │
│  │  Apps | Systems | HMI    │ │  HMI | Agent | Proto  │ │
│  │  Core (Style/Manager)    │ │  (Expr/Eye)           │ │
│  └──────────────────────────┘ └───────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                      Middle Layer                        │
│  ┌──────────────────┐  ┌────────────────────────────┐   │
│  │  System Services │  │   Function Components      │   │
│  │  Audio/WiFi/     │  │   Display (LVGL)           │   │
│  │  Storage/Battery │  │   Audio (esp-gmf/esp-adf)  │   │
│  │  Runtime(uPy)    │  │   Generic (boost/utils)    │   │
│  └──────────────────┘  └────────────────────────────┘   │
├─────────────────────────────────────────────────────────┤
│                    Hardware Layer                        │
│          ESP-IDF + HAL (Interface/Adaptor)               │
└─────────────────────────────────────────────────────────┘
```

## 目录结构与模块映射

### 核心框架 `core/brookesia_core/`

通过 Kconfig 宏控制模块编译：`CONFIG_ESP_BROOKESIA_ENABLE_AI_FRAMEWORK`, `ENABLE_GUI`, `ENABLE_SERVICES`, `ENABLE_SYSTEMS`。

| 子目录 | 职责 |
|--------|------|
| `ai_framework/agent/` | AI Agent 核心：状态机 (Init→Start→Sleep→Wake)、Coze 聊天集成、音频处理 (GMF pipeline)、Function Calling |
| `ai_framework/expression/` | 表情/图标动画系统，emoji 到情绪的映射 |
| `gui/` | GUI 层：LVGL 封装、动画播放器、Squareline 集成、样式管理 |
| `services/storage_nvs/` | NVS 持久化存储服务 |
| `systems/base/` | 基类：`App`、`Context`、`Display`、`Manager`、`Event` |
| `systems/phone/` | Phone 设备系统（已发布） |
| `systems/speaker/` | Speaker 设备系统（开发重点） |

### Speaker 系统 `core/brookesia_core/systems/speaker/`

| 文件 | 职责 |
|------|------|
| `esp_brookesia_speaker.hpp` | `Speaker` 类：继承 `base::Context` + `StylesheetManager`，提供 `installApp()`、`begin()` |
| `esp_brookesia_speaker_manager.hpp` | `Manager` 类：手势处理、快捷设置、屏幕管理 (MAIN/APP/DRAW_DUMMY) |
| `esp_brookesia_speaker_display.hpp` | `Display` 类：360x360 圆屏显示 |
| `esp_brookesia_speaker_ai_buddy.hpp` | `AI_Buddy` 类：整合 Agent + Expression，管理说话状态、WiFi 状态；单例模式 |
| `esp_brookesia_speaker_app.hpp` | Speaker 专用 `App` 子类 |
| `widgets/` | UI 组件：app_launcher、gesture、keyboard、quick_settings |
| `stylesheets/360x360/` | 分辨率特定样式表 |
| `assets/animations/` | 动画资源（boot/emotion/icon，.aaf 格式） |

### 独立 Agent 模块 `agent/`

| 目录 | 职责 |
|------|------|
| `brookesia_agent_coze/` | 豆包 (Coze) 平台：MQTT 聊天 |
| `brookesia_agent_openai/` | OpenAI Realtime API：WebRTC datachannel |
| `brookesia_agent_xiaozhi/` | 小智平台：MQTT 聊天 |
| `brookesia_agent_manager/` | Agent 基类、管理器、状态机 |
| `brookesia_agent_helper/` | 统一 facade 头文件 |

### 服务模块 `service/`

| 目录 | 职责 |
|------|------|
| `brookesia_service_audio/` | 音频服务抽象 |
| `brookesia_service_wifi/` | WiFi 管理 |
| `brookesia_service_nvs/` | NVS 存储 |
| `brookesia_service_sntp/` | 时间同步 |
| `brookesia_service_manager/` | 服务注册/管理 |

### 应用模块 `apps/`

每个 App 是独立组件，继承 `esp_brookesia::systems::base::App`：
- `brookesia_app_settings/` — 设备设置
- `brookesia_app_ai_profile/` — AI 配置管理
- `brookesia_app_calculator/` — 计算器
- `brookesia_app_game_2048/` — 2048 游戏
- `brookesia_app_timer/` — 计时器
- `brookesia_app_pos/` — POS 演示
- `brookesia_app_usbd_ncm/` — USB 网络共享

### 硬件抽象层 `hal/`

| 目录 | 职责 |
|------|------|
| `brookesia_hal_interface/` | 抽象接口：`Device`、`audio_player`、`display_panel`、`display_touch`、`status_led`、`storage_fs` |
| `brookesia_hal_adaptor/` | 板级适配：ESP-VOCAT v1.0/v1.2、ESP-Box-3、Korvo-2-v3 |

### 工具库 `utils/brookesia_lib_utils/`

状态机、日志、线程配置、内存分析、任务调度、时间分析等。

### 产品入口 `products/speaker/`

| 路径 | 职责 |
|------|------|
| `main/main.cpp` | `app_main()` 入口：初始化 services → display → LED → filesystem → audio → system |
| `main/modules/system.cpp` | 系统初始化核心：创建 Speaker、加载样式、注册 App、配置 AI Agent、Function Calling、传感器 |
| `main/modules/audio.cpp` | 音频子系统初始化 |
| `main/modules/display.cpp` | 显示初始化 |
| `main/modules/services.cpp` | 服务初始化 |
| `main/modules/file_system.cpp` | SPIFFS 文件系统 |
| `main/modules/battery_monitor.h` | BQ27220 电量监测 |
| `main/modules/imu_gesture.h` | BMI270 手势识别 |
| `main/modules/touch_sensor.h` | 触摸按键 |
| `main/modules/coze_agent_config.h` | Coze Agent 配置加载 |
| `spiffs/` | 音频提示文件 (.mp3) |
| `common_components/espressif__gmf_ai_audio/` | GMF AI 音频：AEC、AFE、唤醒词 |
| `managed_components/` | 30+ ESP-IDF 托管依赖 |

## App 开发模式

### App 生命周期

继承 `esp_brookesia::systems::base::App`（或 speaker 专用 `esp_brookesia::systems::speaker::App`），实现以下虚函数：

```
init() → run() → [pause() ↔ resume()] → close() → deinit()
```

- `run()` — App 启动入口，在此创建所有 UI 资源，使用 `lv_scr_act()` 操作默认屏幕
- `back()` — 返回事件处理，调用 `notifyCoreClosed()` 通知框架关闭
- `close()` — 关闭时清理
- `pause()` / `resume()` — 被其他 App 切换时的暂停/恢复
- `init()` / `deinit()` — 安装/卸载时的初始化/反初始化

### App 注册与安装

1. App 通过 `PluginRegistry` 自动注册（使用 `esp_utils::PluginRegistry<App>`）
2. 在 `system.cpp` 中通过 `speaker->initAppFromRegistry()` 获取所有已注册 App
3. 通过 `ordered_app_names` 指定 App 图标排列顺序
4. 通过 `speaker->installAppFromRegistry()` 安装到 Launcher

### Function Calling 集成

AI Agent 支持 Function Calling，允许语音控制设备功能。注册流程：

```cpp
FunctionDefinition func("func_name", "description");
func.addParameter("param_name", "param_desc", FunctionParameter::ValueType::String);
func.setCallback([](const std::vector<FunctionParameter> &params) {
    // 处理逻辑
}, thread_config);
FunctionDefinitionList::requestInstance().addFunction(func);
```

已有 Function：`open_app`、`set_volume`、`set_brightness`。

### 样式系统

每个设备和 App 都有独立的样式表（Stylesheet），按分辨率组织：
- Speaker 样式：`core/brookesia_core/systems/speaker/stylesheets/360x360/`
- App 样式：各 App 目录下的 `stylesheets/` 子目录

## 关键命名空间

```cpp
esp_brookesia::systems::base      // 基类：App, Context, Manager, Display, Event
esp_brookesia::systems::speaker   // Speaker 系统：Speaker, Manager, Display, AI_Buddy, App
esp_brookesia::ai_framework       // AI 框架：Agent, Expression, CozeChat, FunctionCalling
esp_brookesia::gui                // GUI：StyleSize, StyleImage, StylesheetManager, LvLockGuard
esp_brookesia::services           // 服务：StorageNVS, ServiceManager
esp_brookesia::apps               // 内置 App：Settings, Calculator, Game2048 等
```

## 关键单例

```cpp
AI_Buddy::requestInstance()           // Speaker AI 助手（Agent + Expression）
Agent::requestInstance()              // AI Agent 核心
StorageNVS::requestInstance()         // NVS 存储服务
FunctionDefinitionList::requestInstance() // Function Calling 注册表
```

## 构建与烧录

```bash
cd products/speaker
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## 添加新 App 的检查清单

1. 在 `apps/` 下创建 `brookesia_app_xxx/` 组件目录
2. 继承 `esp_brookesia::systems::speaker::App`（或 `base::App`）
3. 实现 `run()`、`back()` 等虚函数
4. 提供 launcher icon 图标资源
5. 配置 CMakeLists.txt 和 idf_component.yml
6. 在 `products/speaker/main/modules/system.cpp` 的 `ordered_app_names` 中添加 App 名称
7. 如需语音控制，注册 Function Calling
8. 如需自定义样式，添加 Stylesheet

## 代码风格

- C++17（使用 Boost 库，`boost::signals2` 用于事件通知）
- LVGL C API 用于 GUI
- ESP-IDF Kconfig 系统控制功能开关
- 命名：类用 PascalCase，函数用 camelCase，常量用 UPPER_SNAKE_CASE
- 日志使用 `ESP_UTILS_LOG*` 系列宏
- GUI 操作需在 `LvLockGuard` 保护下执行
