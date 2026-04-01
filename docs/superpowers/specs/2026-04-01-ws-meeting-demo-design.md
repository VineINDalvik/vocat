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

## 9. 实现顺序

按"先跑通流程"原则，从 outside-in 建设：

1. **复制目录 + 清理依赖**（去掉 RTC，改 CMakeLists）
2. **pipeline_ws.c**（改 player 为写 raw PCM，先 stub 验证硬件 I/O）
3. **api_client.c**（HTTP create/end session）
4. **transcribe_ws.c + transcribe_feed_task**（会议模式跑通）
5. **vad.c**（能量 VAD 单独测试）
6. **host_ws.c + host_feed_task**（主持人 WS 连接 + 发音频）
7. **mp3_player.c**（minimp3 + 重采样 + 播放，先用硬编码 MP3 测试）
8. **ws_session.c**（状态机串联所有模块）
9. **ui_meeting.c**（加麦克风圆点、文字显示）
10. **集成测试 + 调整 VAD 参数**

---

## 10. 不在范围内

- 转写结果实时显示（会议模式服务器不下发 transcription）
- 多轮会话历史显示（屏幕太小）
- OTA 升级
- 证书 pinning（开发阶段 skip verify）
