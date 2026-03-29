# ESP-VoCat 开发踩坑记录

> 项目：ESP-VoCat (喵伴) · ESP32-S3 · ESP-IDF v5.5
> 记录时间：2026-03-28
> 涵盖：meeting_demo (Volc RTC) + speaker 产品开发过程中遇到的所有真实问题

---

## 目录

1. [构建系统](#一构建系统)
2. [显示 / LVGL / UI](#二显示--lvgl--ui)
3. [音频硬件初始化](#三音频硬件初始化)
4. [Volcano Engine SDK](#四volcano-engine-sdk)
5. [音频播放质量](#五音频播放质量)
6. [FreeRTOS 并发](#六freertos-并发)
7. [网络 / TLS](#七网络--tls)
8. [私有凭证管理](#八私有凭证管理)

---

## 一、构建系统

### 1.1 ESP-IDF 5.5 伪目标重建 Bug

**症状：** 每次 `idf.py build` 即使无代码改动也触发 50+ 个目标重新构建，耗时数秒。

**原因：** ESP-IDF 5.5 中 `CUSTOM_COMMAND` 的 stamp 文件被声明为 output 但从未创建。

**修复：** 在 `build.sh` 中用 `touch` 补齐所有 stamp 文件：
```bash
_fix_stamps() {
    local stamps=(
        "$BUILD_DIR/bootloader-prefix/src/bootloader-stamp/bootloader-build"
        "$BUILD_DIR/bootloader-prefix/src/bootloader-stamp/bootloader-install"
        "$BUILD_DIR/esp-idf/esptool_py/CMakeFiles/app_check_size"
        "$BUILD_DIR/bootloader/esp-idf/esptool_py/CMakeFiles/bootloader_check_size"
        # + spiffs/assets stamps if applicable
    )
    for stamp in "${stamps[@]}"; do
        [ -d "$(dirname "$stamp")" ] && touch "$stamp" 2>/dev/null || true
    done
}
```

**原则：** 永远用 `build.sh`，不要裸跑 `idf.py build`。

---

### 1.2 Flash 方式：必须用 OpenOCD，不能用 esptool

**症状：** 固件卡死/WDT crash 后，`idf.py flash` 或 `esptool.py` 报连接超时。

**原因：** 固件崩溃时 USB-CDC 栈失效，esptool 依赖 USB-CDC 无法连接。

**修复：** 始终使用 OpenOCD JTAG 烧录：
```bash
openocd -f board/esp32s3-builtin.cfg \
  -c "program_esp {meeting_demo.bin} 0xb0000 verify reset exit"
```

**注意：** OpenOCD reset 后 USB-CDC 重新枚举，端口名可能变（如 `usbmodem101`→`usbmodem2101`）。

---

## 二、显示 / LVGL / UI

### 2.1 开机白屏闪烁

**症状：** 开机时屏幕闪两次白色，然后才显示正常 UI。

**原因：** 背光 `bsp_display_backlight_on()` 在 LVGL UI 创建之前就被调用，导致 LVGL 还未渲染完成时背光已亮，显示白色默认背景。

**修复：** 严格按顺序：先建 UI，再开背光。
```c
// WRONG: 背光先开
bsp_display_start();
bsp_display_backlight_on();   // 此时 UI 还没建好
ui_meeting_create();

// CORRECT:
bsp_display_start();
bsp_display_lock(0);
ui_meeting_create();           // 先建 UI
bsp_display_unlock();
bsp_display_backlight_on();   // 最后开背光
```

---

### 2.2 背光不亮（黑屏）

**症状：** 固件运行正常（串口有日志），但屏幕全黑。

**原因：**
1. `bsp_display_brightness_init()` 被重复调用（`bsp_display_start()` 内部已调用），导致 LEDC 冲突（`GPIO 44 is not usable`）。
2. GPIO 44 是背光 PWM 引脚，冲突后背光停留在 0%。

**修复：** 不要重复调用 `bsp_display_brightness_init()`，改用 `bsp_display_backlight_on()`（注意：该函数在 esp_vocat.h 声明缺失，需手动前向声明）：
```c
// 在 main.c 顶部
esp_err_t bsp_display_backlight_on(void);  // 头文件漏了，手动声明

// 使用
bsp_display_backlight_on();
```

---

### 2.3 LVGL 按钮回调死锁

**症状：** 点击 UI 按钮后设备卡死。

**原因：** LVGL 事件回调运行在 LVGL task 上下文中，此时 LVGL lock 已被持有。回调内再调用 `bsp_display_lock()` / `ui_meeting_set_state()`（内部也调 lock）→ **同一 task 重入同一 mutex → 死锁**。

**修复规则：**
- 在 LVGL 回调内直接操作 widget（`lv_label_set_text`、`lv_obj_add_flag` 等），**不需要也不能**调 lock。
- 回调内不能做任何阻塞操作（I2C、网络、`volc_create`）。必须派发到独立 task：

```c
static void btn_host_cb(lv_event_t *e)
{
    apply_state(UI_STATE_HOST);                        // 直接操作，无需 lock
    lv_label_set_text(s_status_label, "Connecting");  // 直接操作
    xTaskCreate(task_session_start, "sess", 6144, NULL, 5, NULL);  // 异步
}
```

---

### 2.4 Montserrat 字体不支持 Unicode 特殊字符

**症状：** 日志警告 `glyph dsc. not found for U+25CF`，状态文字 `● Connected` 显示为空。

**原因：** LVGL Montserrat 字体默认只含基本 ASCII + 部分拉丁字符，`●`（U+25CF）不在字体内。

**修复：** 用 ASCII 替代符号，或编译包含该字符的自定义字体：
```c
// WRONG
lv_label_set_text(s_status_label, "● Connected");

// CORRECT
lv_label_set_text(s_status_label, "* Connected");
```

---

## 三、音频硬件初始化

### 3.1 I2C 总线冲突（最常见）

**症状：** `ESP_ERROR_CHECK failed: ESP_ERR_INVALID_STATE` 或 `I2C driver already installed`，codec 初始化失败。

**原因：** `bsp_display_start()` 内部调用 `bsp_i2c_init()` 初始化了 I2C port 0（用于触摸屏 CST816S）。如果后续代码再次调 `i2c_new_master_bus(port=0, ...)` → 冲突。

**修复：** 直接复用 BSP 持有的 I2C handle，不重新创建：
```c
// WRONG
i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);

// CORRECT
s_i2c_bus = bsp_i2c_get_handle();  // 借用 BSP 已有的 handle
if (!s_i2c_bus) {
    // 只有 BSP 未初始化时才自己创建
    i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
}
```

**原则：** 释放时不要 `i2c_del_master_bus()`，因为 bus 由 BSP 拥有。

---

### 3.2 ES8311 I2C 地址错误

**症状：** `Fail to write to dev 18`，`ES8311: Open fail`。

**原因：** `esp_codec_dev` 的 I2C ctrl 层期望传入 **8-bit 地址**（含读写位）。
- `ES8311_CODEC_DEFAULT_ADDR = 0x30`（8-bit 格式）
- 7-bit 地址 `0x18` 是错误的传入方式

**修复：**
```c
#define ES8311_I2C_ADDR  (0x30)  // 使用 driver header 定义的 8-bit 默认地址
```

---

### 3.3 ES7210 I2C 地址错误

**症状：** `Fail to write to dev 40`，`ES7210: Open fail`。

**原因：** 同 3.2。ES7210 的 8-bit 地址是 `0x80`（ADDR[1:0]=00），7-bit `0x40` 是不正确的传入方式。

**修复：**
```c
#define ES7210_I2C_ADDR  (0x80)  // ES7210_CODEC_DEFAULT_ADDR = 0x80
```

---

### 3.4 ES7210 麦克风无信号（RMS < 10）

**症状：** 麦克风采集到的 PCM 数据 RMS 极低（3-10），正常语音应为 300-3000+，VAD 永远不触发，Agent 不回复。

**原因：** `esp_codec_dev_open()` 后麦克风增益默认为 0dB，实际拾音几乎无信号。

**修复：** 打开 codec 后显式设置录音增益：
```c
esp_codec_dev_open(s_rec_dev, &rec_info);
esp_codec_dev_set_in_gain(s_rec_dev, 30.0f);  // 30dB，与 speaker 项目一致
```

---

### 3.5 麦克风拾音通道错误

**症状：** 设置增益后 RMS 仍然很低，偶尔有短暂信号但不稳定。

**原因：** ES7210 在 ESP-VoCat 上的 I2S 输出中，麦克风信号可能分布在左右两个通道，仅取左声道（`src[i*2]`）会错过部分信号。

**修复：** 对左右声道取平均作为 mono 输出：
```c
static void conv_32s_to_16m(const int32_t *src, int16_t *dst, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t l = src[i * 2]     >> 16;
        int32_t r = src[i * 2 + 1] >> 16;
        dst[i] = (int16_t)((l + r) / 2);  // L+R 平均混音
    }
}
```

---

### 3.6 音频采样率格式不匹配（16kHz vs 8kHz）

**症状：** 麦克风 RMS 正常（有信号），但 Agent 完全没有反应（无 LISTENING/THINKING/ANSWERING 事件）。

**原因：** Volc SDK 的 G711A RTC 信道期望接收 **8kHz 16bit mono PCM**，但代码发送的是 **16kHz PCM**（ES7210 采样率）。服务端 VAD 把 16kHz 数据当 8kHz 解析，音调错位，VAD 永远不触发。

**修复：** 发送前做 2:1 降采样：
```c
// 16kHz → 8kHz（简单抽取，生产环境建议用低通滤波后再抽取）
static void downsample_2to1(const int16_t *src, int16_t *dst, int src_frames)
{
    for (int i = 0; i < src_frames / 2; i++) {
        dst[i] = src[i * 2];
    }
}
```

**相关参数：**
- volc G711A 信道：输入 8kHz 16bit mono，输出 8kHz 16bit mono（SDK 内部解码为 PCM）
- 每帧 20ms = 160 samples × 2 bytes = 320 bytes（8kHz）
- I2S 录音帧：640 bytes（16kHz 20ms）→ 降采样后 320 bytes（8kHz 20ms）

---

## 四、Volcano Engine SDK

### 4.1 `volc_create` 返回 -1：`video_codec` 字段缺失

**症状：**
```
[ERR] volc_rtc_create: read video_codec failed
[ERR] volc_rtc_create: rtc init failed
[ERR] rtc instance is NULL
volc_start failed: -1
```

**原因：** SDK 在解析 config JSON 时，即使 video 禁用（`publish: false`），也**必须**存在 `video.codec` 字段。官方 example 有 `"codec":1`，遗漏后 RTC 初始化报错。

**修复：** video 节点加上 codec 字段，并加上 params 数组：
```c
"\"video\":{\"publish\":false,\"subscribe\":false,\"codec\":1},"
"\"params\":["
    "\"{\\\"debug\\\":{\\\"log_to_console\\\":1}}\","
    "\"{\\\"audio\\\":{\\\"codec\\\":{\\\"internal\\\":{\\\"enable\\\":1}}}}\","
    "\"{\\\"rtc\\\":{\\\"access\\\":{\\\"concurrent_requests\\\":1}}}\","
    "\"{\\\"rtc\\\":{\\\"ice\\\":{\\\"concurrent_agents\\\":1}}}\""
"]"
```

---

### 4.2 `volc_create` 返回 -1：TLS 证书未挂载

**症状：**
```
mbedtls_net_connect error, return -0x52
webclient POST request failed
Failed to register device
```

**原因：** `volc_create` 内部会向火山服务端发起 HTTPS 设备注册请求。ESP-IDF mbedTLS 默认不信任任何 CA，TLS 握手失败。

**修复：** 在 `volc_create` 前挂载证书包：
```c
#include "esp_tls.h"
#include "esp_crt_bundle.h"

esp_tls_init_global_ca_store();
esp_crt_bundle_attach(NULL);
// 然后再调用 volc_create(...)
```

**追加 CMakeLists REQUIRES：** `esp-tls mbedtls`

---

### 4.3 WiFi 断连时机导致 TLS 连接失败

**症状：** 点击 Host Mode 的恰好是 WiFi 重连中，`volc_create` 的 HTTPS 请求失败。

**修复：** 在 `volc_create` 前等待 WiFi IP 就绪：
```c
for (int i = 0; i < 50; i++) {  // 最多等 5s
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) break;
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

---

### 4.4 config_buf 太小导致 snprintf 截断

**症状：** 编译报 `-Werror=format-truncation`。

**原因：** 加入 `params` 数组后，config JSON 超出了原来的 512 字节缓冲。

**修复：** 扩大 config_buf：
```c
char config_buf[1024];  // 原 512 → 改为 1024
```

---

## 五、音频播放质量

### 5.1 扬声器严重杂音（ZOH 方波谐波）

**症状：** Agent 语音播放有明显高频噪声/刺耳杂音。

**原因：** 8kHz→16kHz 上采样用的是 Zero-Order Hold（每个样本直接复制2次），产生方波谐波 → 高频噪声。

**修复：** 改用线性插值（相邻样本取均值作为插值点）：
```c
static void expand_8k16m_to_16k32s(const int16_t *src, int32_t *dst, int in_frames)
{
    for (int i = 0; i < in_frames; i++) {
        int16_t s0   = (i == 0) ? s_play_prev_sample : src[i - 1];
        int16_t s1   = src[i];
        int16_t smid = (int16_t)(((int32_t)s0 + s1) / 2);  // 插值点
        int32_t v0   = (int32_t)smid << 16;
        int32_t v1   = (int32_t)s1   << 16;
        dst[i * 4]     = v0;  // frame 2i,   L
        dst[i * 4 + 1] = v0;  // frame 2i,   R
        dst[i * 4 + 2] = v1;  // frame 2i+1, L
        dst[i * 4 + 3] = v1;  // frame 2i+1, R
    }
    if (in_frames > 0) s_play_prev_sample = src[in_frames - 1];
}
```

---

### 5.2 扬声器音频重复/卡顿（阻塞 volc 回调）

**症状：** Agent 语音出现重复片段、卡顿、断续。

**原因：**
1. `esp_codec_dev_write()` 是阻塞调用（等待 I2S DMA），在 volc 回调线程（高优先级）里直接调用会阻塞 volc 的内部处理。
2. 每次回调都 `heap_caps_malloc/free`，造成堆碎片和延迟抖动。

**修复：** 引入 Ring Buffer + 独立播放任务：
```
volc audio callback → xRingbufferSend (非阻塞, 5ms timeout)
                            ↓
play_task (Core 0, prio 7) → xRingbufferReceive → expand → esp_codec_dev_write
```

关键配置：
```c
// 使用 NOSPLIT 保持帧完整性
s_play_rb = xRingbufferCreate(PLAY_RB_SIZE, RINGBUF_TYPE_NOSPLIT);

// play_task 放 Core 0，避免与 feed_task (Core 1) 竞争
xTaskCreatePinnedToCore(player_task, "play_task", 4096, NULL, 7, &s_play_task, 0);
```

---

### 5.3 音频重复（旧帧数据污染 out_buf）

**症状：** 听到前一句话的末尾被重复播放。

**原因：** `xRingbufferReceiveUpTo` 可能返回不足一帧的数据（< 320 字节）。`expand_8k16m_to_16k32s` 只填了 `in_frames*4` 个 int32，但 `esp_codec_dev_write` 始终写满 `PLAY_OUT_BYTES`（2560 字节），多出来的部分是 out_buf 中残留的**上一帧旧数据**。

**修复：**
1. 用 `RINGBUF_TYPE_NOSPLIT` 保证每次 receive 拿到完整的 320 字节帧。
2. 实际写出字节数根据 in_frames 计算，不足部分补零：
```c
int actual_bytes = in_frames * CODEC_CHANNELS * sizeof(int32_t) * 2;
if (actual_bytes < PLAY_OUT_BYTES) {
    memset((uint8_t*)out_buf + actual_bytes, 0, PLAY_OUT_BYTES - actual_bytes);
}
esp_codec_dev_write(s_play_dev, out_buf, PLAY_OUT_BYTES);
```

---

### 5.4 回声自问自答（无 AEC）

**症状：** Agent 说完后，Agent 立刻又继续说，形成对话死循环，不等用户开口。

**原因：** 30dB 增益后麦克风把扬声器的 Agent 声音录进去，发回服务端，服务端 VAD 以为用户一直在说话，Agent 不断触发应答。

**根本原因：** 没有 AEC（声学回声消除）。

**临时解决方案（无 AEC 的替代）：** 收到 Agent 音频时立即静音麦克风，Agent 停止发音频 600ms 后自动开麦：
```c
static volatile int64_t s_last_agent_audio_ms = 0;
#define AGENT_SILENCE_UNMUTE_MS 600

// 在 on_volc_audio_data 回调里
s_last_agent_audio_ms = esp_timer_get_time() / 1000;
s_sess.mic_muted = true;

// 在 audio_feed_task 里
if (s_sess.mic_muted && s_last_agent_audio_ms > 0) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_agent_audio_ms >= AGENT_SILENCE_UNMUTE_MS) {
        s_sess.mic_muted = false;
    }
}
```

**注意：** `conv_status` 事件（ANSWERING/ANSWER_FINISH）在某些 bot 配置下不稳定触发，不要完全依赖它控制静音。

---

## 六、FreeRTOS 并发

### 6.1 播放任务与录音任务同核竞争

**症状：** 加了播放 task 后麦克风不工作，发送帧 RMS 极低。

**原因：** `play_task`（优先级 7）和 `audio_feed_task`（优先级 5）都 pin 在 Core 1，高优先级任务在阻塞（`esp_codec_dev_write` DMA 等待）期间会挤占低优先级任务。

**修复：** 两个 task 分开核：
```c
// audio_feed_task → Core 1 (在 volc_rtc_session.c)
#define FEED_TASK_CORE  1

// play_task → Core 0 (在 pipeline_gmf.c)
#define PLAY_TASK_CORE  0
```

---

### 6.2 `eTaskGetState(handle) == eDeleted` 是悬空访问

**症状：** stop 后偶发崩溃或卡死。

**原因：** FreeRTOS 中 task 自删除（`vTaskDelete(NULL)`）后 handle 变为无效指针，`eTaskGetState(handle)` 是未定义行为。

**修复：** 改用 TaskNotify 机制：
```c
// feed task 退出前通知 stop caller
if (s_sess.caller_task) {
    xTaskNotify(s_sess.caller_task, 1, eSetValueWithOverwrite);
}
vTaskDelete(NULL);

// stop 函数等待通知
s_sess.caller_task = xTaskGetCurrentTaskHandle();
s_sess.running = false;
ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
```

---

### 6.3 双启动竞态（Double-Start Race）

**症状：** 连续快速点击按钮，session 重复初始化导致崩溃。

**原因：** `volc_rtc_session_start()` 的 guard 只检查 `s_sess.running`，但 `s_sess.running` 是在 task 创建后才设置为 true，中间有窗口期允许第二次进入。

**修复：** 加 `starting` flag，在 task 创建前即锁定：
```c
if (s_sess.running || s_sess.starting) return ESP_ERR_INVALID_STATE;
s_sess.starting = true;
// ... 初始化 ...
s_sess.running = true;
s_sess.starting = false;
```

---

### 6.4 `mic_muted` 跨 session 状态污染

**症状：** 第二次进入 Host Mode 后麦克风完全静音，无论说什么 Agent 都不响应。

**原因：** `mic_muted` 是静态全局变量，上次 session 在 ANSWERING 状态停止时 `mic_muted = true` 留存，下次 start 没有重置。

**修复：** 在 session start 和 stop 时都重置：
```c
// 在 volc_rtc_session_start() 开头
s_sess.mic_muted = false;

// 在 volc_rtc_session_stop() 结尾
s_sess.mic_muted = false;
```

---

## 七、网络 / TLS

### 7.1 TLS 握手失败后的重试

**症状：** WiFi 刚连上或刚断线重连后，立刻发起 HTTPS 请求失败。

**修复：** 在执行 volc_create 前等待 IP 地址就绪（见 4.3 节），并在 `sdkconfig.defaults` 配置 TLS 跳过验证（开发期）：
```
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

---

## 八、私有凭证管理

### 8.1 凭证不能写进 sdkconfig.defaults

**问题：** WiFi 密码、Volc Token 等私有凭证不能提交到代码仓库。

**方案：** 用 `sdkconfig.local` 覆盖层（gitignore）：

1. `CMakeLists.txt` 中声明：
```cmake
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
endif()
```

2. 提供 `sdkconfig.local.example`（提交到仓库）：
```
CONFIG_MEETING_WIFI_SSID="your_ssid"
CONFIG_MEETING_WIFI_PASSWORD="your_password"
CONFIG_MEETING_VOLC_BOT_ID="your_bot_id"
# ...
```

3. 在 `.gitignore` 中排除：
```
products/meeting_demo/sdkconfig.local
```

---

## 快速诊断表

| 现象 | 第一检查点 |
|------|------------|
| 屏幕黑屏，串口正常 | 背光顺序？`bsp_display_backlight_on()` 是否在 UI 建好后调用 |
| 点按钮卡死 | 是否在 LVGL 回调里调了 `bsp_display_lock()` 或阻塞操作 |
| codec 初始化 NACK | I2C 地址用 8-bit 还是 7-bit？ES8311=0x30, ES7210=0x80 |
| 多次 I2C NACK 后续成功 | 正常，首次地址扫描探测行为 |
| 麦克风 RMS < 20 | 是否调了 `esp_codec_dev_set_in_gain(rec_dev, 30.0f)` |
| volc_create 返回 -1 | ① config JSON 有无 `video.codec`；② TLS bundle 是否挂载；③ WiFi 是否就绪 |
| Agent 不回复（无 conv_status） | 麦克风发送的是 8kHz 还是 16kHz？检查 RMS 是否 > 100 |
| 音频重复/卡顿 | Ring buffer 是否用 NOSPLIT？play_task 是否在 Core 0 |
| Agent 回答后自问自答 | AEC 缺失，检查 `mic_muted` 逻辑是否在收到 audio_data 后立即静音 |
| 第二次进 Host Mode 麦克风静音 | `mic_muted` 是否在 session_start/stop 时重置为 false |
