#!/bin/bash
# Fast incremental build script for esp-brookesia/meeting_demo
#
# Fixes ESP-IDF 5.5 bug: CUSTOM_COMMAND stamp files are declared as outputs
# but never created, causing 50+ spurious target rebuilds on every build.
#
# Usage:
#   ./tools/build.sh                      # incremental build only
#   ./tools/build.sh flash [PORT]         # build + flash app
#   ./tools/build.sh monitor [PORT]       # open serial monitor
#   ./tools/build.sh flash monitor [PORT] # build + flash + monitor

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
MONITOR_LOG="/tmp/esp_monitor_meeting.log"
APP_BIN="$BUILD_DIR/meeting_demo.bin"
APP_OFFSET="0xb0000"

OPENOCD_BIN="$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250422/openocd-esp32/bin/openocd"
OPENOCD_SCRIPTS="$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250422/openocd-esp32/share/openocd/scripts"

# Source ESP-IDF if not already active
if [ -z "$IDF_PATH" ]; then
    source "$HOME/esp/esp-idf/export.sh" 2>/dev/null || {
        echo "ERROR: ESP-IDF not found at ~/esp/esp-idf"
        exit 1
    }
fi

_detect_port() {
    ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1
}

# After OpenOCD reset, USB-CDC re-enumerates — wait up to 10s for it.
_wait_for_port() {
    echo "Waiting for USB-CDC device..."
    for i in $(seq 1 20); do
        local p
        p=$(_detect_port)
        if [ -n "$p" ]; then
            echo "Device ready: $p"
            PORT="$p"
            return 0
        fi
        sleep 0.5
    done
    echo "ERROR: No USB-CDC device appeared after 10s"
    return 1
}

# Touch stamp files that ESP-IDF 5.5 CUSTOM_COMMANDs declare but never create.
# meeting_demo has no spiffs/asset partitions, so only bootloader stamps needed.
_fix_stamps() {
    local stamps=(
        "$BUILD_DIR/bootloader-prefix/src/bootloader-stamp/bootloader-build"
        "$BUILD_DIR/bootloader-prefix/src/bootloader-stamp/bootloader-install"
        "$BUILD_DIR/esp-idf/esptool_py/CMakeFiles/app_check_size"
        "$BUILD_DIR/bootloader/esp-idf/esptool_py/CMakeFiles/bootloader_check_size"
    )
    for stamp in "${stamps[@]}"; do
        [ -d "$(dirname "$stamp")" ] && touch "$stamp" 2>/dev/null || true
    done
}

# Flash via OpenOCD JTAG (works even when firmware is frozen/crashed).
_do_flash() {
    local offset="$1"
    local binary="$2"

    if [ ! -f "$binary" ]; then
        echo "ERROR: Binary not found: $binary"
        exit 1
    fi

    if [ -x "$OPENOCD_BIN" ]; then
        echo "Flashing via OpenOCD JTAG: $(basename "$binary") → $offset"
        "$OPENOCD_BIN" -s "$OPENOCD_SCRIPTS" \
            -f board/esp32s3-builtin.cfg \
            -c "program_esp {$binary} $offset verify reset exit" 2>&1 \
            | grep -E "(Programming|Verify|Error|error|FAILED|failed|Warn)" || true
        return
    fi

    echo "WARNING: OpenOCD not found, falling back to esptool (requires live firmware)"
    PORT="${PORT:-$(_detect_port)}"
    [ -z "$PORT" ] && { echo "ERROR: No serial port found"; exit 1; }
    python3 -m esptool \
        --chip esp32s3 -p "$PORT" -b 460800 \
        --before=default_reset --after=hard_reset \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
        "$offset" "$binary"
}

_start_monitor() {
    local port="$1"
    echo ""
    echo "Monitor on $port  (log: $MONITOR_LOG)  — Ctrl+C to stop"
    exec python3 -m esp_idf_monitor --port "$port" 2>&1 | tee "$MONITOR_LOG"
}

cd "$PROJECT_DIR"

CMD="${1:-build}"
shift || true

PORT=""
MONITOR=false
for arg in "$@"; do
    case "$arg" in
        /dev/*) PORT="$arg" ;;
        monitor) MONITOR=true ;;
    esac
done

case "$CMD" in
    build)
        ninja -C "$BUILD_DIR"
        _fix_stamps
        ;;

    flash)
        ninja -C "$BUILD_DIR"
        _fix_stamps
        _do_flash "$APP_OFFSET" "$APP_BIN"
        if $MONITOR; then
            _wait_for_port
            _start_monitor "$PORT"
        fi
        ;;

    monitor)
        PORT="${PORT:-$(_detect_port)}"
        if [ -z "$PORT" ]; then _wait_for_port; fi
        _start_monitor "$PORT"
        ;;

    *)
        echo "Usage: $0 [build|flash|monitor] [PORT] [monitor]"
        echo "  build                 Incremental build (default)"
        echo "  flash [PORT]          Build + flash via OpenOCD JTAG"
        echo "  flash monitor [PORT]  Build + flash + open monitor"
        echo "  monitor [PORT]        Open serial monitor (log: $MONITOR_LOG)"
        exit 1
        ;;
esac
