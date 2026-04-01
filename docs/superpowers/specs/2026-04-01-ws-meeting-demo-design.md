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

**接口详情：**

**POST /api/session — 创建会议**

请求体（Content-Type: application/json）：
```json
{ "topic": "产品评审会" }
```
- `topic`：可选，默认 "未命名会议"；从 `CONFIG_WS_MEETING_TOPIC` 读取

成功响应（HTTP 200）：
```json
{ "session_id": "sess-abc123def456", "created_at": "2026-03-24T10:30:00.000000+00:00" }
```
- 代码只需提取 `session_id` 字段

**POST /api/session/{session_id}/end — 结束会议**

无请求体；成功响应（HTTP 200）：
```json
{ "status": "ended", "transcript_count": 42 }
```
- 成功判断：HTTP status == 200，无需解析响应体

**URL 拼接注意（⚠️ 双路径陷阱）：**
- Base URL 末尾已含 `/voice-api`：`https://ailabs.mayfair-inc.net/voice-api`
- 路径直接拼接：`/api/session`、`/api/session/{id}/end`
- 最终 URL 示例：`https://ailabs.mayfair-inc.net/voice-api/api/session`
- **禁止**在 Base URL 之后再次拼接 `/voice-api`

**其他配置：**
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
- **服务端不下发任何消息**（纯单向，`WEBSOCKET_EVENT_DATA` 在此接口永远不会触发）
- 注意：transcribe WS 与 host WS 可同时连接；本设计选择进入 HOST 模式时断开 transcribe WS（减少带宽），退出时重新连接

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

服务端下发消息格式（完整 JSON 结构）：

| 消息 | JSON 格式 | 字段说明 |
|------|-----------|---------|
| 问题转写 | `{"type":"transcription","text":"..."}` | `text`：ASR 识别出的问题文本 |
| 流式文本 | `{"type":"answer_text","text":"...","done":false}` | `text`：当前片段；`done=true` 时 text 为空字符串，表示文本流结束 |
| TTS 音频 | `{"type":"answer_audio","data":"<base64_mp3>"}` | `data`：Base64 编码的 MP3 数据，24kHz，每条消息是一个完整句子 |
| 完成 | `{"type":"done"}` | 无其他字段，一轮 Q&A 完成 |
| 错误 | `{"type":"error","message":"..."}` | `message`：错误描述 |

收到各类消息后的处理动作：

| type | 动作 |
|------|------|
| `transcription` | `ws_session_update_ui_status("Q: " + msg.text)` |
| `answer_text` | 追加 `msg.text` 到 UI status（`msg.done==true` 时换行） |
| `answer_audio` | base64 解码 `msg.data` → `mp3_player_enqueue(data, len)` |
| `done` | 设置 `g_got_done_signal = true`（原子操作） |
| `error` | `ws_session_update_ui_status("Error: " + msg.message)` |

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

按"先跑通流程"原则，从 outside-in 建设。每个任务附带测试方法、量化验收条件，以及**日志验收标志**——代码中必须打印的关键日志字符串，用于在串口监视器中直接确认阶段性完成。

> **日志约定：** 所有关键节点使用 `ESP_LOGI(TAG, "[OK] ...")` 标记成功，`ESP_LOGE(TAG, "[FAIL] ...")` 标记失败，`ESP_LOGI(TAG, "[LATENCY] ...")` 标记延迟测量。格式固定便于 `grep` 过滤。

---

### Task 1 — 复制目录 + 清理依赖

**工作内容：**
- 复制 `products/rtc_meeting_demo` → `products/ws_meeting_demo`
- 移除 `components/volc_rtc_engine_lite/` 目录
- 移除 `server/` 目录
- 更新 `CMakeLists.txt`：去掉 `volc_rtc_engine_lite` 链接，加入 `esp_websocket_client`
- 在 `sdkconfig.defaults` 加入 `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`
- `main/main.c`：删除 RTC 版本日志，改为启动标识日志

**测试用例：**
1. 在 `products/ws_meeting_demo/` 执行 `idf.py set-target esp32s3 && idf.py build`
2. 确认无 undefined symbol 报错（特别是 `byte_rtc_*` 符号不存在）

**日志验收标志：**

| 阶段 | 必须出现的日志（串口） |
|------|----------------------|
| 启动 | `I (...) main: ws_meeting_demo starting` |
| WiFi 就绪 | `I (...) wifi: connected, ip=...` |
| 禁止出现 | 任何 `byte_rtc_` 相关字符串 / `Guru Meditation Error` |

**验收标准：**
- `idf.py build` 零错误完成（允许存在 warning）
- 生成的 `.bin` 文件存在且大小合理（< 2MB）
- 串口出现 `ws_meeting_demo starting`，无 panic，显示屏亮起

---

### Task 2 — pipeline_ws.c：音频硬件 I/O

**工作内容：**
- 以 `pipeline_gmf.c` 为基础创建 `pipeline_ws.c`
- 保留所有硬件初始化代码（I2S、ES8311、ES7210）
- 将 player 从"接受 Opus 包 → 解码"改为"接受 16kHz 16bit mono PCM → 直接写 I2S"
- 删除 `esp_opus_dec_*` 相关代码
- 新增 `pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)`

**测试用例：**
1. **录音测试**：循环读取 PCM 帧，每 50 帧打印一次 RMS；静音 RMS < 200，说话 RMS > 1000
2. **播放测试**：写入 1kHz 正弦波 PCM（1 秒），扬声器发声
3. **同时开启测试**：录音 + 播放同时运行 5 秒，无 I2S 超时

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| 硬件初始化完成 | `I (...) pipeline_ws: [OK] hw_init: I2S ready 16000Hz 32bit 2ch` |
| DAC 就绪 | `I (...) pipeline_ws: [OK] ES8311 (DAC) ready` |
| ADC 就绪 | `I (...) pipeline_ws: [OK] ES7210 (ADC) ready, gain=42dB` |
| 录音打开 | `I (...) pipeline_ws: [OK] recorder opened` |
| 播放打开 | `I (...) pipeline_ws: [OK] player opened (PCM ring buffer)` |
| 录音 RMS 采样（每 50 帧） | `I (...) pipeline_ws: rms=#<N> val=<RMS>` |
| 播放关闭 | `I (...) pipeline_ws: [OK] player closed` |
| 录音关闭 | `I (...) pipeline_ws: [OK] recorder closed` |
| 禁止出现 | `E (...) pipeline_ws:` 任何错误行 |

**验收标准：**
- 串口连续出现 `rms=...` 行，静音时 val < 200，说话时 val > 1000
- 扬声器正常出声，无失真
- 5 秒同时运行后无 `E (` 日志

---

### Task 3 — api_client.c：HTTP 会话管理

**工作内容：**
- 实现 `api_client_create_session(topic, out_id, len)`：POST `/api/session`，解析 `session_id`
- 实现 `api_client_end_session(session_id)`：POST `/api/session/{id}/end`
- 连接超时 10s，失败返回 `ESP_FAIL`

**测试用例：**
1. **创建会议**：WiFi 连接后调用 create_session，打印 session_id
2. **结束会议**：用上一步 session_id 调用 end_session
3. **无网络容错**：WiFi 断开后调用，≤ 12s 内返回 ESP_FAIL
4. **重复结束**：对已结束 session 再次调用 end，不 crash

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| 会议创建成功 | `I (...) api_client: [OK] session created: sess-<ID>` |
| 会议结束成功 | `I (...) api_client: [OK] session ended: sess-<ID>` |
| HTTP 失败（容错测试） | `E (...) api_client: [FAIL] create_session http_err=<N>` |
| Heap 检查（每次调用后） | `I (...) api_client: heap_free=<N>` |
| 禁止出现 | 任何 `Guru Meditation` / `assert failed` |

**验收标准：**
- `session_id` 格式为 `sess-` 开头，串口可见 `[OK] session created`
- 无网络时 ≤ 12s 出现 `[FAIL]` 日志后函数返回
- 调用前后 heap_free 差值 < 512 bytes

---

### Task 4 — transcribe_ws.c + transcribe_feed_task：会议模式

**工作内容：**
- 实现 `transcribe_ws_connect(session_id)`
- 实现 `transcribe_ws_send_audio(pcm, size)`：base64 编码 + 发 JSON
- 实现 `transcribe_ws_send_end()`
- 实现 `transcribe_ws_disconnect()`
- 实现 `transcribe_feed_task`：持续读取 100ms PCM 帧并发送

**测试用例：**
1. **连接测试**：调用 connect()，观察 CONNECTED 日志
2. **发送测试**：运行 feed_task 30 秒，说话，HTTP 查询 transcript count > 0
3. **断开测试**：disconnect() 后 task 退出，heap 无增长
4. **断线重连**：网络中断 3s 恢复，30s 内自动重连

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| WS 连接成功 | `I (...) transcribe_ws: [OK] connected wss://.../ws/transcribe/sess-<ID>` |
| feed task 启动 | `I (...) transcribe_ws: [OK] feed task started` |
| 每 100 帧汇报一次 | `I (...) transcribe_ws: sent #<N> frames total` |
| 发送 end 信号 | `I (...) transcribe_ws: [OK] sent end signal` |
| 断开完成 | `I (...) transcribe_ws: [OK] disconnected` |
| feed task 退出 | `I (...) transcribe_ws: [OK] feed task stopped, heap_free=<N>` |
| 自动重连 | `I (...) transcribe_ws: reconnecting attempt 1...` → 随后再出现 `[OK] connected` |
| 禁止出现 | `E (...) transcribe_ws:` 任何错误行（正常流程中） |

**验收标准：**
- WS 连接建立 ≤ 3s，串口出现 `[OK] connected`
- 发送 60s 后 `GET /transcript` count > 0
- disconnect 后 heap_free 与连接前差 < 1KB（日志中 `heap_free=` 可对比）

---

### Task 5 — vad.c：能量 VAD

**工作内容：**
- 实现 `vad_reset(ctx)` 和 `vad_process_frame(ctx, pcm, frames)`
- 三状态机：WAITING → SPEECH → SILENCE_AFTER_SPEECH
- 返回 `VAD_RESULT_CONTINUE` 或 `VAD_RESULT_END_OF_SPEECH`

**测试用例：**
1. **静音不触发**：10 秒零帧，无 END_OF_SPEECH
2. **噪声过滤**：200ms 高能量 + 2s 静音，无触发
3. **正常触发**：1s 高能量 + 700ms 静音，1700ms ± 100ms 触发
4. **重置后再触发**：reset() 后重复用例 3，再次触发
5. **设备实测**：说一句话，观察日志

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| 检测到语音开始 | `I (...) vad: speech start (rms=<N>)` |
| 检测到静音开始 | `I (...) vad: silence start after <N>ms speech` |
| 触发 end_of_speech | `I (...) vad: [OK] end_of_speech triggered (speech=<N>ms silence=<N>ms)` |
| 重置 | `I (...) vad: reset` |
| 噪声被过滤（未达到 speech_min_ms） | `I (...) vad: noise filtered (<N>ms < speech_min_ms)` |
| 禁止出现（用例 1/2） | 任何 `end_of_speech triggered` |

**验收标准：**
- 用例 3：串口出现 `[OK] end_of_speech triggered (speech=~1000ms silence=~600ms)`
- 设备实测：说话停止后 600ms ± 150ms 出现 `[OK] end_of_speech triggered`
- 用例 1/2：串口中完全无 `end_of_speech triggered`

---

### Task 6 — host_ws.c + host_feed_task：主持人问答 WS

**工作内容：**
- 实现 `host_ws_connect(session_id)`
- 实现 `host_ws_disconnect()`：先发 `{"type":"stop"}`
- 实现 `host_ws_set_callback(cb, ctx)`
- 实现 `host_feed_task`：读 PCM → VAD → 发 audio/end_of_speech
- WS 消息回调处理所有消息类型

**测试用例：**
1. **连接测试**：建立 host WS，10s 无断连
2. **完整问答流程**：提问 5 秒，VAD 触发，等待完整回答
3. **stop 打断测试**：回答阶段发 stop，收到 done，恢复监听
4. **重复问答**：第二轮不重连，成功完成

**日志验收标志（单轮完整问答的完整日志序列）：**

```
I (...) host_ws:   [OK] connected wss://.../ws/host/sess-<ID>
I (...) host_ws:   [OK] feed task started
I (...) vad:       speech start (rms=<N>)
I (...) host_ws:   sending audio frame #<N>           ← 每 50 帧打印一次
I (...) vad:       silence start after <N>ms speech
I (...) vad:       [OK] end_of_speech triggered (speech=<N>ms silence=<N>ms)
I (...) host_ws:   [OK] sent end_of_speech
I (...) host_ws:   recv transcription: <问题文本>
I (...) host_ws:   recv answer_text (done=false) len=<N>  ← 多条
I (...) host_ws:   recv answer_audio chunk #1 len=<N>
I (...) host_ws:   recv answer_text (done=true)
I (...) host_ws:   recv done, resuming VAD
```

| 额外场景 | 日志 |
|---------|------|
| stop 打断 | `I (...) host_ws: [OK] sent stop` → `I (...) host_ws: recv done, resuming VAD` |
| 断开 | `I (...) host_ws: [OK] disconnected, heap_free=<N>` |
| 禁止出现（正常流程） | `E (...) host_ws:` 任何错误行 |

**验收标准：**
- 串口日志按上述顺序出现，无跳步
- VAD 触发到 `recv transcription` ≤ 3s（两条日志的时间戳差）
- `sent end_of_speech` 到 `recv answer_audio chunk #1` ≤ 5s

---

### Task 7 — mp3_player.c：MP3 流式播放

**工作内容：**
- 集成 `minimp3.h`
- 实现 `mp3_player_open/close/enqueue/flush_and_wait/is_busy`
- `mp3_play_task`：dequeue → minimp3 解码 → 24kHz→16kHz 重采样 → `pipeline_ws_player_write_pcm()`
- 收到第 1 个 chunk 立刻开始播放
- 播放期间 `g_mic_muted = true`，播完 + 300ms 后恢复

**测试用例：**
1. **单 chunk 播放**：hardcode 真实 24kHz MP3，enqueue 后扬声器播出
2. **多 chunk 连续**：enqueue 3 个（间隔 50ms），无明显停顿
3. **播放结束检测**：flush_and_wait() 在播完后 ≤ 200ms 返回
4. **mute 标志**：enqueue 后 g_mic_muted=true，flush_and_wait 返回后 400ms 内恢复 false
5. **队列满**：enqueue 10 个，程序不 crash，丢弃日志可见

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| task 启动 | `I (...) mp3_player: [OK] task started` |
| 开始播放第 1 个 chunk | `I (...) mp3_player: [OK] playing chunk #1 len=<N>` |
| 解码完成（每 chunk） | `I (...) mp3_player: chunk #<N> decoded: <N> mp3 frames -> <N> pcm frames (resampled)` |
| mic 静音开始 | `I (...) mp3_player: mic MUTED (playback started)` |
| 队列清空 | `I (...) mp3_player: [OK] all chunks played, queue empty` |
| mic 静音解除 | `I (...) mp3_player: mic UNMUTED (300ms after last chunk)` |
| 队列满丢弃 | `W (...) mp3_player: queue full, dropping chunk len=<N>` |
| task 关闭 | `I (...) mp3_player: [OK] task stopped, heap_free=<N>` |
| 禁止出现 | `E (...) mp3_player:` 任何错误行（正常流程中） |

**验收标准：**
- 串口依次出现 `mic MUTED` → `all chunks played` → `mic UNMUTED`
- `mic MUTED` 与 `mic UNMUTED` 之间时间戳差 = 音频时长 + 300ms ± 100ms
- 队列满测试后无崩溃，出现 `W (` 的 `queue full` 日志

---

### Task 8 — ws_session.c：状态机集成

**工作内容：**
- 实现完整状态机（IDLE / CONNECTING / MEETING / HOST / ERROR）
- `session_cmd_task`：异步执行阻塞操作
- `ws_session_update_ui_status()`：通过 `lv_async_call` 线程安全更新 UI
- 串联 api_client、transcribe_ws、host_ws、mp3_player 的生命周期

**测试用例：**
1. **完整正向流程**：Start → Meeting → Host → Exit → Meeting → Stop
2. **Start 失败回退**：断网后点 Start，≤ 12s 回 IDLE
3. **MEETING 中断网**：断连后自动重连 1 次
4. **快速连击**：CONNECTING 状态连击 Stop，不 crash
5. **HOST 中点 Exit**：播放中退出，mp3_player 停止，回到 MEETING

**日志验收标志（完整正向流程日志序列）：**

```
I (...) ws_session: state → CONNECTING
I (...) api_client: [OK] session created: sess-<ID>
I (...) transcribe_ws: [OK] connected wss://.../ws/transcribe/sess-<ID>
I (...) ws_session: state → MEETING
  ... (transcribe_feed_task 运行中) ...
I (...) ws_session: state → HOST (entering host mode)
I (...) transcribe_ws: [OK] disconnected
I (...) host_ws: [OK] connected wss://.../ws/host/sess-<ID>
  ... (host 问答循环) ...
I (...) ws_session: state → MEETING (exit host mode)
I (...) host_ws: [OK] disconnected
I (...) transcribe_ws: [OK] connected wss://.../ws/transcribe/sess-<ID>
  ... (transcribe_feed_task 恢复) ...
I (...) transcribe_ws: [OK] sent end signal
I (...) api_client: [OK] session ended: sess-<ID>
I (...) ws_session: state → IDLE
```

| 额外场景 | 日志 |
|---------|------|
| Start 失败 | `E (...) ws_session: [FAIL] start_meeting: session create failed` → `I (...) ws_session: state → IDLE` |
| 自动重连 | `I (...) transcribe_ws: reconnecting attempt 1...` → `I (...) transcribe_ws: [OK] connected` |
| Heap 周期打印（每状态进入时） | `I (...) ws_session: heap_free=<N>` |

**验收标准：**
- 正向流程：串口出现完整日志序列，每个 `state →` 均可见
- Start 失败：`[FAIL]` 日志出现后 ≤ 2s 出现 `state → IDLE`
- 每次状态切换时的 heap_free 值在整个测试中无持续下降趋势

---

### Task 9 — ui_meeting.c：UI 更新

**工作内容：**
- 在 MEETING 状态按钮正下方加 12px 绿色麦克风圆点（每 500ms 闪烁）
- HOST 状态时圆点隐藏
- status label 在 HOST 模式显示 Q/A 文本
- 所有 UI 操作通过 `lv_async_call` 或 `bsp_display_lock` 保护

**测试用例：**
1. **IDLE 状态**：只有 Start 按钮
2. **MEETING 状态**：绿色圆点可见闪烁
3. **HOST 状态**：圆点消失，显示 "Listening..."
4. **HOST 收到 transcription**：label 更新为 "Q: ..."
5. **HOST 收到 answer_text 流**：文字逐步追加
6. **LVGL watchdog**：所有切换不触发（超时 5s）

**日志验收标志：**

| 阶段 | 必须出现的日志 |
|------|---------------|
| 进入 IDLE | `I (...) ui_meeting: state → IDLE` |
| 进入 MEETING | `I (...) ui_meeting: state → MEETING, mic dot visible` |
| 进入 HOST | `I (...) ui_meeting: state → HOST, mic dot hidden` |
| UI 更新（每次 status 变化） | `I (...) ui_meeting: status updated: "<文本前20字>"` |
| 禁止出现 | `E (...) ui_meeting:` 任何错误行 / `LVGL assert` |

**验收标准：**
- 串口出现 `mic dot visible` 时，屏幕上绿色圆点可见闪烁
- 串口出现 `mic dot hidden` 时，屏幕上圆点消失
- `status updated:` 日志出现到屏幕更新的延迟 ≤ 100ms（肉眼感受即时）

---

### Task 10 — 集成测试 + VAD 参数调优

**工作内容：**
- 端到端测试完整用户流程
- 调整 VAD 参数至最佳值
- 测量关键延迟并记录到日志

**测试用例：**
1. **完整流程**：Meeting 30s → Host 提问 → 听回答 → Exit → Meeting 30s → Stop，无 crash
2. **VAD 灵敏度**：安静环境误触发率 < 5%（20 次测试）
3. **延迟测量**：end_of_speech → first_answer_audio ≤ 5s；answer_audio_recv → 扬声器发声 ≤ 300ms
4. **长时间稳定性**：Meeting 模式 30 分钟，heap 差 < 4KB
5. **模式切换压测**：Meeting→Host→Meeting 切换 20 次，无错乱

**日志验收标志（集成阶段新增的延迟测量日志）：**

代码中在关键时间点打印 `[LATENCY]` 日志（使用 `esp_timer_get_time() / 1000` 获取 ms 时间戳）：

```
I (...) ws_session: [LATENCY] end_of_speech_sent ts=<T1>ms
I (...) ws_session: [LATENCY] first_answer_audio_recv ts=<T2>ms delta=<T2-T1>ms
I (...) mp3_player: [LATENCY] answer_audio_recv ts=<T3>ms
I (...) mp3_player: [LATENCY] playback_start ts=<T4>ms delta=<T4-T3>ms
I (...) ws_session: heap_free=<N>  ← 每 5 分钟打印一次
```

| 阶段 | 必须出现的日志 |
|------|---------------|
| 每轮问答延迟 | `[LATENCY] first_answer_audio_recv ... delta=<N>ms`（N ≤ 5000） |
| 播放启动延迟 | `[LATENCY] playback_start ... delta=<N>ms`（N ≤ 300） |
| 定时 heap 检查 | `I (...) ws_session: heap_free=<N>`（每 5 分钟，N 不持续下降） |
| 30 分钟测试结束 | `I (...) ws_session: [OK] stability test done, initial_heap=<N0> final_heap=<N1> diff=<N1-N0>` |
| 禁止出现（全程） | `Guru Meditation Error` / `abort()` / `stack overflow` |

**验收标准：**
- 完整流程 3 次测试全部通过，串口每次出现完整 `state →` 序列
- `[LATENCY] delta=` 日志显示 end_of_speech→answer_audio ≤ 5000ms
- `[LATENCY] delta=` 日志显示 answer_audio_recv→playback_start ≤ 300ms
- 30 分钟后 `stability test done diff=` 值 < 4096
- 20 次切换测试全程无 `Guru Meditation`

---

## 10. 不在范围内

- 转写结果实时显示（会议模式服务器不下发 transcription）
- 多轮会话历史显示（屏幕太小）
- OTA 升级
- 证书 pinning（开发阶段 skip verify）
