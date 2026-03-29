# rtc_meeting_demo 实现文档

> 基于 Volcengine VolcEngineRTCLite SDK 的实时 AI 语音对话方案
> 目标芯片：ESP32-S3 (ESP-VoCat 板)
> 音频格式：Opus 16kHz

---

## 一、整体架构

```
┌─────────────────────ESP32-S3──────────────────────┐      ┌──────────────┐      ┌──────────────────┐
│                                                     │      │              │      │                  │
│  ES7210(麦克风) ──I2S RX──► pipeline_gmf           │      │   Python     │      │  Volcengine      │
│                              │ 16kHz 16bit mono PCM │      │  RtcAigc     │      │  Cloud           │
│                              ▼                      │      │  Service.py  │      │                  │
│                         Opus 编码                   │◄────►│              │◄────►│  ASR (大模型)    │
│                              │ Opus 80bytes@32kbps  │ HTTP │  /startvoice │ RTC  │  LLM (豆包)     │
│                              ▼                      │      │  /stopvoice  │      │  TTS (bidirec)   │
│                    byte_rtc_send_audio_data()       │      │              │      │                  │
│                                                     │      └──────────────┘      └──────────────────┘
│  ES8311(扬声器) ◄─I2S TX──  pipeline_gmf           │
│                              ▲ 16kHz 32bit stereo   │
│                         Opus 解码                   │
│                              ▲ Opus packet           │
│                    on_audio_data() callback          │
│                                                     │
│  LVGL UI (360×360) ── ui_meeting.c                 │
│  WiFi (STA) ────────── wifi_init.c                 │
└─────────────────────────────────────────────────────┘
```

---

## 二、模块说明

### 目录结构

```
products/rtc_meeting_demo/
├── main/
│   ├── main.c              # 入口：NVS→Display→UI→WiFi
│   ├── bot_client.c/h      # HTTP 客户端：调用 Python 服务端
│   ├── rtc_session.c/h     # RTC 会话管理：核心状态机
│   ├── pipeline_gmf.c/h    # 音频管线：I2S/codec/Opus编解码
│   ├── ui_meeting.c/h      # LVGL UI：三态屏幕
│   ├── wifi_init.c/h       # WiFi STA 连接
│   └── CMakeLists.txt
├── components/
│   └── volc_rtc_engine_lite/   # VolcEngineRTCLite.a 封装组件
├── server/
│   ├── RtcAigcService.py       # Python HTTP 业务服务器
│   ├── RtcAigcConfig.py        # 凭证配置（gitignored）
│   ├── AccessToken.py          # RTC Token 生成
│   └── RtcApiRequester.py      # Volcengine API 签名
├── sdkconfig.defaults          # ESP32-S3 默认配置
└── sdkconfig.local             # 本地凭证（gitignored）
```

---

## 三、关键流程

### 3.1 启动流程

```
app_main()
  │
  ├── nvs_flash_init()
  ├── bsp_display_start()     ── LVGL 初始化
  ├── ui_meeting_create()     ── 创建 UI（黑屏后亮屏，防白闪）
  ├── bsp_display_backlight_on()
  └── wifi_init_sta()         ── 连接 WiFi，阻塞直到获取 IP
```

### 3.2 Host Mode 触发流程（按按钮后）

```
btn_host_cb()
  └── xTaskCreate(task_session_start)
        └── rtc_session_start(session_state_cb, NULL)
              │
              ├── 等待 WiFi IP (最多 5s)
              ├── pipeline_gmf_hw_init()      ── I2C/I2S/ES8311/ES7210 初始化
              ├── pipeline_gmf_recorder_open() ── 开始录音
              ├── pipeline_gmf_player_open()   ── 启动 player_task (PSRAM 20KB栈)
              │
              ├── bot_client_start_chat()      ── HTTP POST /startvoicechat
              │     └── 返回 rtc_room_info_t {room_id, uid, token, task_id, app_id, bot_uid}
              │
              ├── byte_rtc_create(app_id, handlers)
              ├── byte_rtc_set_audio_codec(AUDIO_CODEC_TYPE_OPUS)
              ├── byte_rtc_init()
              ├── byte_rtc_join_room(room_id, uid, token)
              │
              └── xTaskCreatePinnedToCoreWithCaps(audio_feed_task, PSRAM 24KB栈, Core1)
```

### 3.3 音频上行链路（麦克风 → 云端 ASR）

```c
// audio_feed_task: 运行在 Core 1，PSRAM 栈
while (running) {
    // 1. 从 ES7210 读取 PCM（阻塞）
    pipeline_gmf_recorder_read(pcm_buf, 640);   // 16kHz 16bit mono, 20ms

    // 2. Mic Mute 判断（防出声回音反馈）
    if (mic_muted) {
        // 检查 Bot 是否已静音超过 800ms
        now - last_bot_audio_ms >= 800ms → mic_muted = false
        continue;
    }

    // 3. Opus 编码：16kHz PCM → Opus packet
    esp_opus_enc_process(enc_hd, &in, &out);    // ~80 bytes

    // 4. 发送给 RTC SDK
    byte_rtc_send_audio_data(engine, room_id, opus_buf, encoded_bytes, &frame_info);
}
```

**关键 Opus 编码器参数：**
```c
enc_cfg.sample_rate    = 16000;
enc_cfg.channel        = 1;      // mono
enc_cfg.bits_per_sample = 16;
enc_cfg.bitrate        = 32000;  // 32kbps
enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
```

### 3.4 音频下行链路（Bot TTS → 扬声器）

```c
// on_audio_data 回调（RTC SDK 内部线程）
on_audio_data(engine, room, uid, sent_ts, codec, data_ptr, data_len, ...) {
    if (data_len >= 20)  pipeline_gmf_player_write(data_ptr, data_len);  // 写入 ring buffer
    if (data_len >= 80)  mic_muted = true;  // Bot 真正在说话才 mute
                         last_bot_audio_ms = now_ms;
}

// player_task: 运行在 Core 0，PSRAM 20KB 栈
while (running) {
    // 从 ring buffer 取 Opus packet（NOSPLIT，保证包完整性）
    item = xRingbufferReceive(play_rb, &rx_len, 60ms);
    if (!item) { write_silence(); continue; }

    // Opus 解码：Opus packet → 16kHz 16bit mono PCM
    esp_opus_dec_decode(s_opus_dec, &in, &out_frame, &dec_info);

    // 格式转换：16bit mono → 32bit stereo（ES8311 需要）
    conv_16m_to_32s(pcm_buf, out_buf, frames);

    // 写入 I2S（ES8311 播放）
    esp_codec_dev_write(s_play_dev, out_buf, PLAY_OUT_BYTES);  // 2560 bytes
}
```

---

## 四、Mic Mute 机制（无 AEC 解决方案）

由于没有回声消除（AEC），Bot 说话时如果麦克风开着，扬声器的声音会被麦克风拾取，导致 ASR 把 Bot 自己的话当成用户输入形成死循环。

**判断策略：**

| 数据包大小 | 含义 | 操作 |
|-----------|------|------|
| `< 20` bytes | DTX 静音包，忽略 | 既不播放也不更新时间戳 |
| `20~79` bytes | Comfort Noise（背景噪音）| 播放但不 mute，不更新时间戳 |
| `≥ 80` bytes | 真实语音帧 | 播放 + mute mic + 更新 last_bot_audio_ms |

**Unmute 时机：**
```c
// feed_task 每帧（20ms）检查
if (mic_muted && (now_ms - last_bot_audio_ms) >= 800ms) {
    mic_muted = false;  // Bot 已静音 800ms，认为说话结束
}
```

---

## 五、bot_client HTTP 接口

### /startvoicechat 请求

```json
{
  "audio_codec": "OPUS",
  "enable_burst": false,
  "asr_type": 1,
  "tts_is_bidirection": true,
  "enable_conversation_state_callback": true
}
```

**关键参数说明：**
- `audio_codec: "OPUS"` — room_id 前缀为 `OPUS{uuid}`，匹配 Volcengine AIGC Opus 策略组（不含 `OPUSLOW`）
- `asr_type: 1` — 大模型 ASR（`volc.seedasr.sauc.duration` + `StreamMode: 2`）
- `tts_is_bidirection: true` — 双向流式 TTS（`seed-tts-2.0`）
- `enable_conversation_state_callback: true` — 接收 LISTENING/THINKING/ANSWERING/errorOccurred 状态

### /startvoicechat 响应

```json
{
  "data": {
    "room_id": "OPUSxxxxxx",
    "uid": "userxxxxxx",
    "app_id": "68c4d518...",
    "token": "001688c4...",
    "task_id": "xxxxxxxx",
    "bot_uid": "botxxxxxx"
  }
}
```

### Authorization Header

```
Authorization: af78e30{RTC_APP_ID}
```

---

## 六、服务端配置（Python）

### ASR 配置（大模型）

```python
{
    "Mode": "bigmodel",
    "AppId": ASR_APP_ID,            # 7881262484
    "AccessToken": ASR_ACCESS_TOKEN,
    "ApiResourceId": "volc.seedasr.sauc.duration",
    "StreamMode": 2                  # 并发版流式输入流式输出
}
```

### TTS 配置（双向流式）

```python
{
    "app": {
        "appid": TTS_APP_ID,         # 7806065490
        "token": TTS_ACCESS_TOKEN
    },
    "audio": {
        "voice_type": DEFAULT_VOICE_TYPE  # zh_female_mizai_saturn_bigtts
    },
    "ResourceId": "seed-tts-2.0"
}
```

---

## 七、关键 Kconfig 配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_MEETING_WIFI_SSID` | WiFi SSID |
| `CONFIG_MEETING_WIFI_PASSWORD` | WiFi 密码 |
| `CONFIG_AIGENT_SERVER_HOST` | Python 服务器 `ip:port`（无 scheme） |
| `CONFIG_RTC_APPID` | Volcengine RTC App ID（同时用于 Authorization header） |

凭证存于 `sdkconfig.local`（已 gitignore），不提交到仓库。

---

## 八、踩坑记录

### 1. room_id 前缀决定 AIGC 策略组
Volcengine 根据 room_id 前缀识别 AIGC 策略组（编解码格式）。必须使用 `OPUS{uuid}`，不能用 `OPUSOPUSLOW{uuid}`（后者匹配的策略组不支持真正的 Opus 双向流）。

### 2. play_task 必须用 PSRAM 大栈
Opus 解码器（`esp_opus_dec_decode`）需要大量栈空间，6KB/12KB 均会溢出，需 **20KB PSRAM 栈**：
```c
xTaskCreatePinnedToCoreWithCaps(player_task, "play_task", 20*1024, ..., MALLOC_CAP_SPIRAM);
```

### 3. audio_feed_task 也需 PSRAM 大栈
Opus 编码器 `esp_opus_enc_process` 同样需要大栈，使用 **24KB PSRAM 栈**。

### 4. 没有 AEC 需要手动 Mic Mute
ESP-VoCat 硬件层面没有 AEC（回声消除），必须在 Bot 说话时 mute 麦克风，否则 ASR 将 Bot 声音识别为用户输入产生死循环。判断用 `data_len >= 80` 区分真实语音和 CN 包。

### 5. bot_client.c 不含 connect_timeout_ms
ESP-IDF 5.5 的 `esp_http_client_config_t` 没有 `connect_timeout_ms` 字段，只有 `timeout_ms`。

### 6. ASR WebSocket bad handshake
`asr:websocket: bad handshake` 错误通常由 ASR AccessToken 过期或 ApiResourceId 不匹配引起。需在[火山引擎语音识别控制台](https://console.volcengine.com/speech/service/16)确认 Token 有效期。

### 7. VolcEngineRTCLite bind local ip failed
SDK 内部 `SocketConnection-Lite.c` 在 KCP 通道初始化时调用 `volc_get_local_ip` 获取本机 IP 并 bind UDP socket。在某些网络环境下此步骤失败，但主 RTC 音频通道（ICE/DTLS）仍然可以工作，**此错误可以忽略**，不影响实际音频传输。

---

## 九、构建 & 烧录

```bash
cd products/rtc_meeting_demo

# 首次配置
cp sdkconfig.local.example sdkconfig.local
# 编辑 sdkconfig.local 填写 WiFi / 服务器 / RTC App ID

# 编译烧录
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash

# 启动 Python 服务器（同局域网 Mac 上）
cd server
cp RtcAigcConfig.py.example RtcAigcConfig.py
# 编辑 RtcAigcConfig.py 填写所有凭证
pip install requests
python RtcAigcService.py
```

---

## 十、数据流时序

```
[设备上电]
   │ WiFi 连接 (~1-2s)
   │
[点击 Host Mode]
   │ HTTP POST /startvoicechat (~500ms)
   │ byte_rtc_join_room (~200ms)
   │
[Bot 发送欢迎语]
   │ on_audio_data(len≥80) → mic_muted=true
   │ Opus decode → I2S → ES8311 → 扬声器
   │ ASR subv 字幕实时输出（欢迎语识别）
   │ CONV answerFinish
   │ (800ms 后) mic_muted=false
   │
[用户说话]
   │ ES7210 → PCM → Opus encode → byte_rtc_send_audio_data
   │ ──────────── 上行 UDP ────────────►
   │                                    ASR 识别用户语音
   │                                    LLM 生成回复
   │                                    TTS 合成音频
   │ ◄─────────── 下行 RTC ────────────
   │ on_audio_data(len≥80) → mic_muted=true
   │ Opus decode → I2S → ES8311 → 扬声器（Bot 回答）
   │
[点击 Exit Host Mode]
   │ rtc_session_stop()
   │   byte_rtc_leave_room → byte_rtc_fini → byte_rtc_destroy
   │   bot_client_stop_chat (POST /stopvoicechat)
   │   pipeline_gmf_recorder/player_close
```
