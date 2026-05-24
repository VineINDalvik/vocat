# ws_meeting_demo Debug Workflow

## Standard Problem Investigation → Fix → Verification Process

### Phase 1: Problem Investigation (Log-Driven, No Code Changes)

1. **Capture device logs** — Use pyserial to read `/dev/cu.usbmodem1101` at 115200 baud. Capture 180-300s of logs while reproducing the issue. Filter out LVGL font warnings (`draw_letter` lines) to reduce noise.

2. **Identify the failure pattern** — Look for:
   - `Guru Meditation Error` / `panic'ed` — hardware fault (stack overflow, null pointer, watchdog)
   - State transitions that shouldn't happen (`state → IDLE` when user didn't press Stop)
   - `queue full` / `dropping chunk` — resource exhaustion
   - `WS disconnected` / `WS error` / `mbedtls_ssl_write error` — connectivity/TLS failures
   - `heap_free` trending downward — memory leak
   - Unexpected `vad_reset` or repeated `end_of_speech` — algorithm state corruption

3. **Decode crash backtrace** — If Guru Meditation found, use `xtensa-esp32s3-elf-gdb` (at `~/.espressif/tools/xtensa-esp-elf-gdb/*/bin/`) with the `.elf` file to resolve addresses to function names.

4. **Cross-reference with code** — Read the relevant source files to trace the exact code path from the log timestamps. Confirm root cause before touching any code.

### Phase 2: Multi-Agent Problem Report

Spawn three specialist agents with full code context:

| Agent | Role | Focus |
|-------|------|-------|
| **Algorithm Expert** | Technical root cause analysis | Stack budgets, memory layouts, race conditions, TLS path analysis, buffer sizing |
| **Test Expert** | Regression risk assessment | State machine integrity, fix scope verification, test matrix creation, edge case enumeration |
| **Product Expert** | User impact evaluation | UX consequences, priority ranking, follow-up improvements, reliability gaps |

Each agent reads the actual source files and reports independently. Aggregate findings into a single decision before coding.

### Phase 3: Code Fix (Minimal, Surgical)

- Fix only the confirmed root cause. No speculative changes.
- Match existing patterns in the codebase (e.g., if another module already uses 12KB stack for TLS, use the same value).
- Add comments only when the WHY is non-obvious (e.g., "12KB: TLS ops need ~4KB stack").
- Do NOT refactor adjacent code, do NOT add unrelated improvements.

### Phase 4: Build + Flash + Log Verification

1. Build: `idf.py build` (with correct IDF_PATH and IDF_PYTHON_ENV_PATH)
2. Flash: `idf.py -p /dev/cu.usbmodem1101 flash`
3. Monitor: pyserial capture for 180-300s, exercising the affected feature
4. Verify:
   - No Guru Meditation / panic in logs
   - No unexpected state transitions
   - Feature works correctly (transcription, playback, interrupt)
   - Heap stable, no memory leak trend

### Phase 5: Regression Checklist

After every fix, verify these scenarios are NOT broken:

| Scenario | What to check |
|----------|--------------|
| Meeting mode start/stop | IDLE → CONNECTING → MEETING → IDLE cycle |
| Meeting → Host → Meeting | Same session_id, transcribe WS reconnects cleanly |
| Host mode interrupt | Interrupt button works, mp3_player stops, mic unmutes, VAD resumes |
| Long listening session | No crash after 5+ minutes continuous transcription |
| Long TTS response | No queue overflow, no audio dropout (QUEUE_DEPTH=64) |
| All button labels | English ASCII only, no CJK (montserrat font limitation) |

### Common ESP32-S3 Pitfalls Reference

| Pitfall | Symptom | Root cause pattern |
|---------|---------|-------------------|
| Stack overflow in TLS tasks | `panic'ed (LoadProhibited)` in `mbedtls_ssl_write/read` | `task_stack < 12288` for any task that calls esp_websocket_client_send_text |
| CJK font missing | `lv_draw_letter: glyph dsc. not found` + blank button | Using non-ASCII text with `lv_font_montserrat_*` |
| MP3 queue overflow | `queue full, dropping chunk` + audio cut-out | `QUEUE_DEPTH` too small for long TTS responses |
| VAD poison state | Repeated `end_of_speech` every 20ms | Missing `vad_reset()` before returning `VAD_RESULT_END_OF_SPEECH` |
| Interrupt beginning swallowed | First audio chunk of new response discarded | `s_interrupt_flag` checked after queue receive instead of before |