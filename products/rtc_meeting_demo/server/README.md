# rtc_meeting_demo — Python Business Server

## Quick Start

```bash
cd products/rtc_meeting_demo/server
pip install -r requirements.txt
cp RtcAigcConfig.py.example RtcAigcConfig.py   # fill in your credentials
python RtcAigcService.py
```

Must be run from within the `server/` directory (bare module imports).

## Configuration

Edit `RtcAigcConfig.py` (gitignored — never committed):
- `AK` / `SK` — Volcengine IAM access key/secret
- `RTC_APP_ID` / `RTC_APP_KEY` — from Volcengine RTC console
- `ASR_APP_ID` / `ASR_ACCESS_TOKEN` — speech recognition service
- `TTS_APP_ID` / `TTS_ACCESS_TOKEN` — speech synthesis service
- `DEFAULT_END_POINT_ID` — ARK large model endpoint

## Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/startvoicechat` | POST | Start AI agent, returns room credentials |
| `/stopvoicechat` | POST | Stop AI agent |
| `/updatevoicechat` | POST | Interrupt agent or send function calling result |

See `server/src/README.md` in the official volcengine/rtc-aigc-embedded-demo repo for full API docs.
