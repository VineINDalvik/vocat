# ws_meeting_demo — 设计规范

**日期:** 2026-04-01  
**产品:** `products/ws_meeting_demo`  
**基于:** `products/rtc_meeting_demo`（保留 UI 骨架 + pipeline 硬件层，替换 RTC → WebSocket）  
**后端:** ClarityX Voice API — `https://ailabs.mayfair-inc.net/voice-api`

---

## 1. 目标

将 `rtc_meeting_demo` 中 Mock 的会议模式和 RTC 主持人模式，全部替换为 WebSocket 实现：

| 模式 | 接口 | 方向 |
|------|------|------|
| 会议模式（监听） | `wss://.../ws/transcribe/{session_id}` | 单向（设备→服务器） |
| 主持人模式（问答） | `wss://.../ws/host/{session_id}` | 双向 |

优先级：**跑通完整流程 → 主持人 VAD → 流畅 MP3 播放**。

---

## 2. 产品目录结构

```
products/ws_meeting_demo/
  main/
    main.c                 # app_main（去掉 RTC 版本日志，其余同 rtc_meeting_demo）
    wifi_init.c/h          # 原样复用
    ui_meeting.c/h         # 修改：加麦克风状态圆点
    pipeline_ws.c/h        # 改自 pipeline_gmf.c：去掉 Opus，player 改为写 raw PCM
    api_client.c/h         # HTTP：POST /api/session, POST /api/session/{id}/end
    ws_session.c/h         # 状态机：协调各模块生命周期
    transcribe_ws.c/h      # 会议模式 WS（单向，发 PCM 音频帧）
    host_ws.c/h            # 主持人模式 WS（双向，含 VAD 逻辑）
    mp3_player.c/h         # MP3 队列解码播放器
    vad.c/h                # 能量 VAD
    minimp3.h              # 单头文件 MP3 解码器（MIT，直接放入 main/）
    CMakeLists.txt         # 去掉 volc_rtc_engine_lite，加 esp_websocket_client
    Kconfig.projbuild      # API_BASE_URL, VAD 参数
  sdkconfig.defaults       # 从 rtc_meeting_demo 复制，去掉 RTC 相关选项
  partitions.csv           # 原样复用
  CMakeLists.txt           # 去掉 components/volc_rtc_engine_lite
  dependencies.lock        # 重新生成
```

**移除：**
- `components/volc_rtc_engine_lite/`
- `server/`（后端已经是远程服务）

**新增依赖：**
- `minimp3.h`（单头文件，MIT 协议）
- `esp_websocket_client`（ESP-IDF 内置，无需额外安装）

**保留依赖：**
- `esp_codec_dev`、`es8311_codec`、`es7210_adc`（硬件 codec）
- `mbedtls`（base64 编码，内置）
- `cJSON`（JSON 解析，内置）
- `bsp/esp_vocat`（板级支持）

---

## 3. 状态机

```
IDLE
  ─[Start btn]──────→ api_client_create_session()
                        [OK]  → 连接 transcribe WS → MEETING
                        [FAIL]→ UI 显示 "Connection Error" → IDLE

MEETING
（transcribe WS 已连接，麦克风持续录音，麦克风圆点绿色脉冲）
  ─[Stop btn]────────→ 发 {"type":"end"} → 断开 transcribe WS
                        → api_client_end_session()
                        → 停止麦克风 → IDLE
  ─[Host btn]────────→ 断开 transcribe WS → 停止麦克风
                        → 连接 host WS → HOST

HOST
（host WS 已连接，VAD 持续监听）
  VAD WAITING/SPEECH   → 持续发送音频帧 {"type":"audio","data":"<b64>"}
  VAD 触发 end_of_speech → 发 {"type":"end_of_speech"}，暂停麦克风发送
  收到 transcription   → 更新 UI status label（"你问：xxx"）
  收到 answer_text     → 追加到 UI status label
  收到 answer_audio    → base64 解码 → 推入 mp3_player 队列
                          （同时 g_mic_muted=true）
  收到 done            → 等待 mp3_player 队列清空
                          → 300ms 后 g_mic_muted=false → 重置 VAD → 继续监听
  收到 error           → UI 显示错误，停留在 HOST（不自动退出）
  ─[Exit btn]────────→ 发 {"type":"stop"} → 断开 host WS
                        → 停止 mp3_player → 停止麦克风
                        → 重新连接 transcribe WS → MEETING
```

---

## 4. 模块详细设计

### 4.1 api_client.c

HTTP 客户端，使用 `esp_http_client`（ESP-IDF 内置）。

```c
// 创建会议，返回 session_id（最长 64 字节）
esp_err_t api_client_create_session(const char *topic,
                                    char *out_session_id, size_t len);

// 结束会议
esp_err_t api_client_end_session(const char *session_id);
```

- Base URL 从 Kconfig `CONFIG_WS_MEETING_API_BASE_URL` 读取（默认 `https://ailabs.mayfair-inc.net/voice-api`）
- HTTPS：需要配置 `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`（开发阶段）或内嵌根证书
- 超时 10s，失败返回 `ESP_FAIL`

### 4.2 pipeline_ws.c

改自 `pipeline_gmf.c`，**保留所有硬件初始化代码**（I2S、ES8311、ES7210），只改 player：

```c
// 硬件初始化/反初始化（不变）
esp_err_t pipeline_ws_hw_init(void);
esp_err_t pipeline_ws_hw_deinit(void);

// 录音（不变）：输出 16kHz 16bit mono PCM
esp_err_t pipeline_ws_recorder_open(void);
int       pipeline_ws_recorder_read(void *buf, size_t size);
esp_err_t pipeline_ws_recorder_close(void);

// 播放（改变）：接受 16kHz 16bit mono PCM，写入 I2S
esp_err_t pipeline_ws_player_open(void);
esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames);
esp_err_t pipeline_ws_player_close(void);

esp_err_t pipeline_ws_set_volume(int volume);
```

**Player 实现变化：**
- 去掉 `esp_opus_dec_*`
- 保留 ring buffer + player_task 结构（防止 I2S 被阻塞）
- `player_task` 从 ring buffer dequeue PCM frames → `conv_16m_to_32s()` → `esp_codec_dev_write()`
- `pipeline_ws_player_write_pcm()` 将 PCM 帧推入 ring buffer

Ring buffer 大小：`16000 × 2 × 0.5s = 16000 bytes`（500ms 缓冲）

### 4.3 vad.c

简单能量 VAD，无状态外漏，所有参数可 Kconfig 配置。

```c
typedef enum {
    VAD_STATE_WAITING,              // 等待用户开口
    VAD_STATE_SPEECH,               // 检测到语音
    VAD_STATE_SILENCE_AFTER_SPEECH, // 语音后静音
} vad_state_t;

typedef struct {
    vad_state_t state;
    int speech_frames;   // 已累积语音帧数
    int silence_frames;  // 连续静音帧数
} vad_ctx_t;

typedef enum {
    VAD_RESULT_CONTINUE,       // 继续监听
    VAD_RESULT_END_OF_SPEECH,  // 触发结束
} vad_result_t;

void        vad_reset(vad_ctx_t *ctx);
vad_result_t vad_process_frame(vad_ctx_t *ctx,
                                const int16_t *pcm, int frames);
```

**状态转换（每 20ms 帧一次）：**

```
WAITING → SPEECH：连续 2 帧 RMS > THRESHOLD（约 40ms）
SPEECH → SILENCE_AFTER_SPEECH：RMS < THRESHOLD
SILENCE_AFTER_SPEECH → SPEECH：重新检测到声音（重置 silence 计数）
SILENCE_AFTER_SPEECH 触发 END_OF_SPEECH 条件：
    silence_frames × 20ms ≥ VAD_SILENCE_MS（默认 600ms）
    AND speech_frames × 20ms ≥ VAD_SPEECH_MIN_MS（默认 300ms）
```

**Kconfig 参数：**
- `CONFIG_VAD_ENERGY_THRESHOLD`：默认 800（RMS 绝对值，int16）
- `CONFIG_VAD_SPEECH_MIN_MS`：默认 300ms
- `CONFIG_VAD_SILENCE_MS`：默认 600ms

### 4.4 transcribe_ws.c

```c
// 连接 wss://<base>/ws/transcribe/<session_id>
esp_err_t transcribe_ws_connect(const char *session_id);

// 发送一帧 PCM（内部 base64 编码，组装 JSON，发送）
// pcm: 16kHz 16bit mono，size 字节
esp_err_t transcribe_ws_send_audio(const void *pcm, size_t size);

// 发送结束信号 {"type":"end"}
esp_err_t transcribe_ws_send_end(void);

// 断开连接
esp_err_t transcribe_ws_disconnect(void);
```

**transcribe_feed_task（core 1，priority 5）：**
```
while (running):
    read 3200 bytes PCM (100ms) from pipeline_ws_recorder_read()
    base64_encode(pcm, 3200) → b64 string
    json = {"type":"audio","data":"<b64>"}
    esp_websocket_client_send_text(ws, json)
    // no sleep needed: recorder_read() blocks on I2S DMA
```

- 帧大小：3200 bytes = 16000Hz × 2bytes × 0.1s（100ms，符合 API 文档推荐）
- Base64 后约 4268 字节，JSON 总长约 4290 字节
- WS 发送非阻塞（`esp_websocket_client_send_text` 内部有队列）

### 4.5 host_ws.c

```c
// 连接 wss://<base>/ws/host/<session_id>
esp_err_t host_ws_connect(const char *session_id);

// 断开（先发 {"type":"stop"}）
esp_err_t host_ws_disconnect(void);

// 注册消息回调
typedef void (*host_ws_msg_cb_t)(const char *type, cJSON *root, void *ctx);
void host_ws_set_callback(host_ws_msg_cb_t cb, void *ctx);
```

**host_feed_task（core 1，priority 5）：**
```
vad_ctx_t vad = {0};
bool sending = true;

while (running):
    read 640 bytes PCM (20ms) from pipeline_ws_recorder_read()
    
    if g_mic_muted:
        vad_reset(&vad)
        continue
    
    result = vad_process_frame(&vad, pcm, 320)
    
    if sending && vad.state == SPEECH:
        base64_encode(pcm, 640) → b64
        esp_websocket_client_send_text(host_ws, {"type":"audio","data":"<b64>"})
    
    if result == VAD_RESULT_END_OF_SPEECH:
        esp_websocket_client_send_text(host_ws, {"type":"end_of_speech"})
        sending = false  // 停止发送，等待 done 信号

    // done 信号由 WS 收消息回调设置：
    if got_done_signal:
        sending = true
        vad_reset(&vad)
        got_done_signal = false
```

**WS 消息接收（`WEBSOCKET_EVENT_DATA` 回调，esp_websocket_client 内部 task）：**

| type | 动作 |
|------|------|
| `transcription` | `ws_session_update_ui_status("Q: " + text)` |
| `answer_text` | 追加到 UI status（done=true 时换行） |
| `answer_audio` | base64 解码 → `mp3_player_enqueue(data, len)` |
| `done` | 设置 `g_got_done_signal = true`（原子操作） |
| `error` | `ws_session_update_ui_status("Error: " + message)` |

### 4.6 mp3_player.c

```c
esp_err_t mp3_player_open(void);                            // 启动 mp3_play_task
esp_err_t mp3_player_enqueue(const uint8_t *mp3, size_t len); // 推入队列（非阻塞）
esp_err_t mp3_player_flush_and_wait(void);                  // 等待队列清空
esp_err_t mp3_player_close(void);                           // 停止播放，清空队列

bool mp3_player_is_busy(void);  // 是否正在播放
```

**mp3_play_task（core 0，priority 7）：**
```
mp3dec_t mp3d;
mp3dec_init(&mp3d);

while (running):
    chunk = queue_dequeue(timeout=100ms)
    if chunk == NULL: continue
    
    // minimp3 解码整个 chunk（一个完整 MP3 句子）
    int16_t pcm_out[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    offset = 0;
    
    while offset < chunk.len:
        samples = mp3dec_decode_frame(&mp3d,
                    chunk.data + offset, chunk.len - offset,
                    pcm_out, &info)
        if samples <= 0: break
        offset += info.frame_bytes
        
        // 重采样 24kHz → 16kHz（线性插值，2:3 比率）
        int16_t resampled[samples * 2 / 3 + 1];
        int out_frames = resample_24k_to_16k(pcm_out, samples, resampled);
        
        pipeline_ws_player_write_pcm(resampled, out_frames)
    
    free(chunk.data)

// 播放完毕后
g_playback_done = true
```

**重采样（24kHz → 16kHz，比率 2/3）：**
```c
// 线性插值，每 3 个输入样本产生 2 个输出样本
static int resample_24k_to_16k(const int16_t *in, int in_n, int16_t *out) {
    int out_n = (in_n * 2) / 3;
    for (int i = 0; i < out_n; i++) {
        // 输入位置：i * 1.5
        int p = i * 3 / 2;
        int frac = (i * 3) % 2;  // 0 or 1
        if (frac == 0 || p + 1 >= in_n) {
            out[i] = in[p];
        } else {
            out[i] = (int16_t)(((int)in[p] + in[p + 1]) / 2);
        }
    }
    return out_n;
}
```

**MP3 chunk 队列：**
- FreeRTOS queue，最多 8 个 chunk（防止 host WS 发太快）
- 每个 chunk 是 malloc 的 buffer + length
- 收满丢弃最旧的（`xQueueOverwrite` 不适用，改为：若满则 log 警告并丢弃新 chunk）

**AEC muting：**
- `mp3_play_task` 开始处理队列第 1 个 chunk 时：设 `g_mic_muted = true`（通知 host_feed_task）
- 队列清空（队列为空 + 当前 chunk 已写完）后：等 300ms → `g_mic_muted = false`
- `g_mic_muted` 用 `volatile bool` + 原子读写即可（单写单读）

### 4.7 ws_session.c

主协调器，管理状态机和所有模块生命周期。

```c
typedef enum {
    WS_SESSION_IDLE,
    WS_SESSION_CONNECTING,   // HTTP 创建会议中
    WS_SESSION_MEETING,      // transcribe WS active
    WS_SESSION_HOST,         // host WS active
    WS_SESSION_ERROR,
} ws_session_state_t;

// UI 调用
esp_err_t ws_session_start_meeting(void);   // btn_start_cb 触发
esp_err_t ws_session_stop_meeting(void);    // btn_stop_cb 触发
esp_err_t ws_session_enter_host(void);      // btn_host_cb 触发
esp_err_t ws_session_exit_host(void);       // btn_exit_cb 触发

// 内部 UI 更新（可从任意 task 调用，内部走 lv_async_call）
void ws_session_update_ui_status(const char *text);
```

**所有公开函数都必须快速返回**（仅设置 flag / 发消息到内部 queue），实际阻塞操作在后台 task 执行：

```c
// 内部 command task（core 0 或 1，priority 4）
static void session_cmd_task(void *arg) {
    while (1) {
        cmd = xQueueReceive(s_cmd_queue, portMAX_DELAY);
        switch (cmd.type) {
        case CMD_START:
            // 执行 HTTP + WS connect（可阻塞）
            do_start_meeting();
            break;
        case CMD_STOP:
            do_stop_meeting();
            break;
        // ...
        }
    }
}
```

### 4.8 ui_meeting.c 修改

**新增麦克风圆点（MEETING 状态）：**
- 位于按钮正下方、status label 正上方（y ≈ 225）
- 直径 12px，绿色（`lv_color_make(0x00, 0xCC, 0x44)`）
- 使用 LVGL `lv_timer_create()` 每 500ms 切换显示/隐藏（闪烁）
- HOST 状态：圆点隐藏

**UI status label 用途：**

| 状态 | 内容 |
|------|------|
| IDLE | （空） |
| MEETING | 麦克风圆点 + 无文字 |
| HOST | "Listening..." / "Q: ..." / "A: ..." / "Error: ..." |

---

## 5. 任务与资源总览

```
Core 0:
  LVGL task (priority 5)
  mp3_play_task (priority 7)
  session_cmd_task (priority 4)
  esp_websocket_client 内部 task（2 个 WS 各 1 个）

Core 1:
  transcribe_feed_task (priority 5, 仅 MEETING 态)
  host_feed_task (priority 5, 仅 HOST 态)
```

**内存估算（PSRAM 分配）：**
- mp3_play_task stack: 32KB（minimp3 需要较大栈）
- transcribe_feed_task stack: 8KB
- host_feed_task stack: 8KB
- MP3 chunk queue: 8 × 最大 MP3 句子大小（约 8 × 10KB = 80KB，PSRAM）
- PCM ring buffer（player）: 16KB
- base64 buffer per frame（3200 × 4/3 ≈ 4300 bytes，栈上分配）

---

## 6. 错误处理

| 场景 | 处理 |
|------|------|
| HTTP 创建会议失败 | UI 显示 "Connection Error"，回到 IDLE |
| transcribe WS 断连 | 若 session 仍 active，自动重连 1 次；否则回 IDLE |
| host WS 断连 | UI 显示 "Connection Error"，保持 HOST 状态（用户可手动退出） |
| MP3 队列满 | 丢弃新 chunk，log 警告 |
| VAD 误触发（环境噪声） | 调高 `VAD_ENERGY_THRESHOLD`；`VAD_SPEECH_MIN_MS` 过滤短噪声 |

---

## 7. HTTPS/WSS 证书

开发阶段：`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`（sdkconfig.defaults）  
生产阶段：内嵌 ailabs.mayfair-inc.net 的根证书（Let's Encrypt R3 根证书）。

---

## 8. Kconfig 配置项（Kconfig.projbuild）

```kconfig
config WS_MEETING_API_BASE_URL
    string "ClarityX Voice API base URL"
    default "https://ailabs.mayfair-inc.net/voice-api"

config WS_MEETING_TOPIC
    string "Default meeting topic"
    default "产品评审会"

config VAD_ENERGY_THRESHOLD
    int "VAD energy threshold (RMS int16)"
    default 800
    range 200 5000

config VAD_SPEECH_MIN_MS
    int "Minimum speech duration (ms) before silence counts"
    default 300
    range 100 2000

config VAD_SILENCE_MS
    int "Silence duration (ms) to trigger end_of_speech"
    default 600
    range 200 3000
```

---

## 9. 实现任务、测试用例与验收标准

按"先跑通流程"原则，从 outside-in 建设。每个任务附带测试方法和明确的通过条件。

---

### Task 1 — 复制目录 + 清理依赖

**工作内容：**
- 复制 `products/rtc_meeting_demo` → `products/ws_meeting_demo`
- 移除 `components/volc_rtc_engine_lite/` 目录
- 移除 `server/` 目录
- 更新 `CMakeLists.txt`：去掉 `volc_rtc_engine_lite` 链接，加入 `esp_websocket_client`
- 在 `sdkconfig.defaults` 加入 `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`
- `main/main.c`：删除 `#include "VolcEngineRTCLite.h"` 及 RTC 版本日志

**测试用例：**
1. 在 `products/ws_meeting_demo/` 执行 `idf.py set-target esp32s3 && idf.py build`
2. 确认无 undefined symbol 报错（特别是 `byte_rtc_*` 符号不存在）

**验收标准：**
- `idf.py build` 零错误完成（允许存在 warning）
- 生成的 `.bin` 文件存在且大小合理（< 2MB）
- 串口烧录后设备正常启动，显示屏亮起，日志无 panic

---

### Task 2 — pipeline_ws.c：音频硬件 I/O

**工作内容：**
- 以 `pipeline_gmf.c` 为基础创建 `pipeline_ws.c`
- 保留所有硬件初始化代码（I2S、ES8311、ES7210）
- 将 player 从"接受 Opus 包 → 解码"改为"接受 16kHz 16bit mono PCM → 直接写 I2S"
- 删除 `esp_opus_dec_*` 相关代码
- 新增 `pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)`

**测试用例：**
1. **录音测试**：在 `app_main` 中调用 `pipeline_ws_hw_init()` + `pipeline_ws_recorder_open()`，循环读取 PCM 帧并打印每帧 RMS 值到串口
   - 静音时 RMS < 200
   - 对麦克风说话时 RMS > 1000
2. **播放测试**：调用 `pipeline_ws_player_open()`，循环写入 1kHz 正弦波 PCM（1 秒），从扬声器听到声音
3. **同时开启测试**：录音和播放同时运行 5 秒，无 assert / panic / I2S 超时

**验收标准：**
- 静音环境录音 RMS < 200，说话时 RMS > 1000（串口可观测）
- 扬声器能正常发出 1kHz 测试音，无明显失真或卡顿
- 录音 + 播放同时运行 5 秒设备不崩溃，日志无 `E (` 级别错误

---

### Task 3 — api_client.c：HTTP 会话管理

**工作内容：**
- 实现 `api_client_create_session(topic, out_id, len)`：POST `/api/session`，解析 `session_id`
- 实现 `api_client_end_session(session_id)`：POST `/api/session/{id}/end`
- 连接超时 10s，失败返回 `ESP_FAIL`

**测试用例：**
1. **创建会议**：在 WiFi 连接后调用 `api_client_create_session("测试会议", buf, 64)`，打印返回的 `session_id`
2. **结束会议**：用上一步的 `session_id` 调用 `api_client_end_session()`，打印返回状态
3. **无网络容错**：WiFi 断开时调用 create_session，函数应在 10s 内返回 `ESP_FAIL`（不 hang 死）
4. **重复结束**：对已结束的 session 再次调用 end，应返回非成功但不 crash

**验收标准：**
- `create_session` 返回 `ESP_OK`，`session_id` 格式为 `sess-` 开头的字符串
- `end_session` 返回 `ESP_OK`，串口日志显示 HTTP 200
- 无网络时函数在 ≤ 12s 内返回错误码（不阻塞主流程）
- 无内存泄漏（调用前后 `esp_get_free_heap_size()` 差值 < 512 bytes）

---

### Task 4 — transcribe_ws.c + transcribe_feed_task：会议模式

**工作内容：**
- 实现 `transcribe_ws_connect(session_id)`：建立 `wss://.../ws/transcribe/{id}` 连接
- 实现 `transcribe_ws_send_audio(pcm, size)`：base64 编码 + 发送 JSON
- 实现 `transcribe_ws_send_end()`：发送 `{"type":"end"}`
- 实现 `transcribe_ws_disconnect()`
- 实现 `transcribe_feed_task`：持续读取 100ms PCM 帧并发送

**测试用例：**
1. **连接测试**：调用 `connect()`，串口观察 `WEBSOCKET_EVENT_CONNECTED` 日志
2. **发送测试**：连接后运行 `transcribe_feed_task` 30 秒，对麦克风正常说话，通过 HTTP `GET /api/session/{id}/transcript` 查询转写记录条数 > 0
3. **断开测试**：调用 `disconnect()`，WS 正常关闭，task 退出，无内存泄漏
4. **WS 断线重连**：服务端强制断开（或网络中断 3s 后恢复），30s 内 WS 自动重连，feed_task 恢复发送

**验收标准：**
- WS 连接建立时间 ≤ 3s（局域网环境）
- 持续发送 60 秒后，`GET /transcript` 返回 `count > 0`，内容包含真实说话文本
- `disconnect()` 后 heap 无增长（前后差 < 1KB）
- `transcribe_feed_task` CPU 占用 ≤ 15%（`vTaskGetRunTimeStats` 可观测）

---

### Task 5 — vad.c：能量 VAD

**工作内容：**
- 实现 `vad_reset(ctx)` 和 `vad_process_frame(ctx, pcm, frames)`
- 三状态机：WAITING → SPEECH → SILENCE_AFTER_SPEECH
- 返回 `VAD_RESULT_CONTINUE` 或 `VAD_RESULT_END_OF_SPEECH`

**测试用例（可用 host 端 C 单元测试或设备端 stub 测试）：**
1. **静音不触发**：送入 10 秒静音帧（全零 PCM），不应触发 `END_OF_SPEECH`
2. **噪声过滤**：送入 200ms 高能量帧（模拟短噪声）+ 2 秒静音，不触发（speech_min_ms 过滤）
3. **正常说话触发**：送入 1 秒高能量帧 + 700ms 静音帧，应触发 `END_OF_SPEECH`
4. **中断重新检测**：触发后调用 `vad_reset()`，再次重复测试用例 3，再次触发
5. **设备实测**：在 HOST 模式下说一句话，观察串口日志 `[vad] end_of_speech triggered`

**验收标准：**
- 用例 1：整个过程无 `END_OF_SPEECH`
- 用例 2：无 `END_OF_SPEECH`
- 用例 3：在第 1700ms ± 100ms 触发 `END_OF_SPEECH`
- 用例 4：重置后再次正常触发
- 设备实测：说话停止后 600ms ± 150ms 触发，日志可见

---

### Task 6 — host_ws.c + host_feed_task：主持人问答 WS

**工作内容：**
- 实现 `host_ws_connect(session_id)`：建立 `wss://.../ws/host/{id}` 连接
- 实现 `host_ws_disconnect()`：发送 `{"type":"stop"}` 后断开
- 实现 `host_ws_set_callback(cb, ctx)`：注册消息回调
- 实现 `host_feed_task`：读 PCM → VAD → 发 audio/end_of_speech
- WS 消息回调处理：`transcription`、`answer_text`、`answer_audio`、`done`、`error`

**测试用例：**
1. **连接测试**：建立 host WS，串口观察 CONNECTED 事件，10s 内无意外断连
2. **完整问答流程**：连接后对麦克风提问（说 5 秒），VAD 触发后等待，串口应按顺序看到：
   - `[host_ws] sent end_of_speech`
   - `[host_ws] recv transcription: <问题文本>`
   - `[host_ws] recv answer_text: ...`（多条）
   - `[host_ws] recv answer_audio`（至少 1 条）
   - `[host_ws] recv done`
3. **stop 打断测试**：在回答阶段（收到 answer_audio 之前）发送 `{"type":"stop"}`，服务器应回 `done`，task 正常恢复监听
4. **重复问答**：完成一轮问答后，不重连，再次说话，成功完成第二轮

**验收标准：**
- 从 VAD 触发到收到 `transcription` ≤ 3s
- 从 `end_of_speech` 到收到第一个 `answer_audio` ≤ 5s
- 两轮完整问答均成功，无 WS 断连
- `disconnect()` 后 heap 无增长（< 1KB）

---

### Task 7 — mp3_player.c：MP3 流式播放

**工作内容：**
- 集成 `minimp3.h`
- 实现 `mp3_player_open/close/enqueue/flush_and_wait/is_busy`
- `mp3_play_task`：dequeue → minimp3 解码 → 24kHz→16kHz 重采样 → `pipeline_ws_player_write_pcm()`
- 收到第 1 个 chunk 立刻开始播放（不等后续 chunk）
- 播放期间 `g_mic_muted = true`，全部播完 + 300ms 后恢复

**测试用例：**
1. **单 chunk 播放**：将一个真实的 24kHz MP3 文件（约 2s）hardcode 到 flash，调用 `mp3_player_enqueue()` 后扬声器播出，质量可接受（无明显噪声或变调）
2. **多 chunk 连续播放**：依次 enqueue 3 个 MP3 chunk（间隔 50ms），扬声器连续播放，无明显停顿（chunk 间隔 ≤ 100ms）
3. **播放结束检测**：所有 chunk 入队后调用 `mp3_player_flush_and_wait()`，函数在所有 chunk 播完后返回
4. **mute 标志测试**：enqueue 后立即读取 `g_mic_muted`，应为 true；flush_and_wait 返回后 400ms 内，`g_mic_muted` 变为 false（串口打印确认）
5. **队列满丢弃测试**：连续 enqueue 10 个大 chunk（超过队列上限 8），程序不 crash，log 显示 "queue full, dropping chunk"

**验收标准：**
- 单 chunk 播放正常，扬声器声音清晰可辨
- 连续 3 chunk 播放无明显停顿（肉耳感受流畅）
- `flush_and_wait()` 在播放完成后 ≤ 200ms 内返回
- `g_mic_muted` 在播放结束 300-500ms 后恢复 false（串口可见日志）
- 10 chunk 压测后设备无崩溃，heap 无持续增长

---

### Task 8 — ws_session.c：状态机集成

**工作内容：**
- 实现完整状态机（IDLE / CONNECTING / MEETING / HOST / ERROR）
- `session_cmd_task`：异步执行阻塞操作
- `ws_session_update_ui_status()`：通过 `lv_async_call` 线程安全更新 UI
- 串联 api_client、transcribe_ws、host_ws、mp3_player 的生命周期

**测试用例：**
1. **完整正向流程**：按顺序操作 Start → (等待) Meeting → Host → (等待) → Exit → Meeting → Stop，每步检查状态机转换日志
2. **Start 失败回退**：断网后点 Start，10s 内回到 IDLE，UI 显示 "Connection Error"
3. **MEETING 中断网**：Meeting 状态下断网，transcribe WS 断连，自动重连 1 次；若仍失败，UI 显示错误提示
4. **快速连击按钮**：在 CONNECTING 状态快速点 Stop，不 crash，最终回到 IDLE
5. **HOST 中点 Exit**：在 HOST 模式收到 answer_audio 播放中点 Exit，mp3_player 停止，WS 断开，重连 transcribe WS，回到 MEETING

**验收标准：**
- 正向流程每步状态转换在串口可观察（`[ws_session] state → MEETING` 等）
- Start 失败后 ≤ 12s 回到 IDLE，UI 有错误提示
- 快速连击 10 次不 crash，最终状态正确
- Exit HOST 后 ≤ 3s 回到 MEETING 状态，麦克风圆点重新出现

---

### Task 9 — ui_meeting.c：UI 更新

**工作内容：**
- 在 MEETING 状态按钮正下方加 12px 绿色麦克风圆点（每 500ms 闪烁）
- HOST 状态时圆点隐藏
- status label 在 HOST 模式显示 Q/A 文本（Q: xxx / A: xxx 流式追加）
- 所有 UI 操作通过 `lv_async_call` 或 `bsp_display_lock` 保护

**测试用例：**
1. **IDLE 状态**：只显示 "Start Meeting" 按钮，无圆点，无 status
2. **MEETING 状态**：出现绿色圆点（目视可见闪烁），"Host Mode" 和 "Stop Meeting" 按钮显示
3. **HOST 状态**：圆点消失，"Exit Host Mode" 按钮显示，status label 显示 "Listening..."
4. **HOST 收到 transcription**：status label 更新为 `"Q: <问题文本>"`
5. **HOST 收到 answer_text 流**：文字逐步追加显示（每条 answer_text 追加到 label）
6. **LVGL watchdog**：以上所有状态切换不触发 LVGL watchdog（超时通常 5s）

**验收标准：**
- 三种状态 UI 元素与设计描述一致（可截图对比）
- 麦克风圆点在 MEETING 状态可见闪烁，在其他状态不可见
- 在 HOST 状态收到服务器文字后，label 内容在 100ms 内更新（肉眼即时响应）
- 运行 10 分钟无 LVGL panic 或 watchdog 触发

---

### Task 10 — 集成测试 + VAD 参数调优

**工作内容：**
- 端到端测试完整用户流程
- 在真实会议环境（多人说话）测试 transcribe 模式
- 调整 VAD 参数至最佳值
- 测量关键延迟指标

**测试用例：**
1. **完整会议→主持人→会议流程**：
   - Start Meeting → 说话 30 秒 → Host Mode → 提问 → 听回答 → Exit → 继续说话 30 秒 → Stop Meeting
   - 全程无 crash，无 WS 断连（或自动重连成功）
2. **VAD 灵敏度测试**：
   - 安静环境（< 40dB）：说话后静默 600ms 应触发（不误触发）
   - 有背景音环境（60dB）：说话停止后 ≤ 1s 触发（允许适当调高阈值）
3. **延迟测量**：
   - 从 VAD end_of_speech 到收到第一个 `answer_audio`（记录时间戳，目标 ≤ 5s）
   - 从收到 `answer_audio` 到扬声器开始发声（目标 ≤ 300ms）
4. **长时间稳定性**：连续运行 Meeting 模式 30 分钟，无 heap 泄漏（每 5 分钟打印 heap 大小，差值 < 4KB），无崩溃
5. **模式切换压测**：反复 Meeting→Host→Meeting 切换 20 次，无状态错乱或资源泄漏

**验收标准：**
- 完整流程测试 3 次全部通过，无 crash
- 安静环境 VAD 误触发率 < 5%（20 次测试中 ≤ 1 次误触发）
- `answer_audio` 到扬声器发声延迟 ≤ 300ms（串口时间戳可测）
- 30 分钟运行后 heap 剩余量与启动时差值 < 4KB
- 20 次模式切换测试全部成功完成

---

## 10. 不在范围内

- 转写结果实时显示（会议模式服务器不下发 transcription）
- 多轮会话历史显示（屏幕太小）
- OTA 升级
- 证书 pinning（开发阶段 skip verify）
