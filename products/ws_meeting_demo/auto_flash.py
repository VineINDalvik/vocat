#!/usr/bin/env python3
"""Auto-flash script for XIAO ESP32-S3 on macOS.
Monitors serial ports for the chip to appear in download mode,
then immediately flashes the firmware.

Instructions:
1. Unplug USB-C cable from XIAO
2. Hold BOOT button (bottom side of board, near USB-C)
3. Plug USB-C cable back in
4. Release BOOT button after 1 second
5. This script will detect the chip and flash automatically
"""

import subprocess
import time
import os
import glob

FIRMWARE_DIR = "/Users/vinexio/Desktop/dev-projects/vocat/products/ws_meeting_demo/build"
ESPTOOL_CMD = [
    "python3", "-m", "esptool",
    "--chip", "esp32s3",
    "--port", None,  # will be filled in
    "--before", "no_reset",
    "--after", "hard_reset",
    "--baud", "460800",
    "write_flash",
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "16MB",
    "0x0", f"{FIRMWARE_DIR}/bootloader/bootloader.bin",
    "0x8000", f"{FIRMWARE_DIR}/partition_table/partition-table.bin",
    "0xd000", f"{FIRMWARE_DIR}/ota_data_initial.bin",
    "0xb0000", f"{FIRMWARE_DIR}/ws_meeting_demo.bin",
]

def find_esp32_port():
    """Find ESP32-S3 USB-Serial/JTAG serial port."""
    cu_ports = glob.glob("/dev/cu.usbmodem*")
    # Filter for Espressif devices (ESP32-S3 USB-Serial/JTAG)
    for port in cu_ports:
        return port  # Return the first matching port
    return None

def wait_for_port(max_wait=60):
    """Wait for ESP32-S3 serial port to appear."""
    print("Waiting for ESP32-S3 serial port to appear...")
    print("(Unplug USB, hold BOOT, replug USB, release BOOT)")

    start_time = time.time()
    initial_port = find_esp32_port()

    if initial_port:
        print(f"Port {initial_port} exists — trying direct connection...")
        return initial_port

    while time.time() - start_time < max_wait:
        port = find_esp32_port()
        if port:
            print(f"Port {port} appeared!")
            time.sleep(0.5)  # Brief wait for driver to stabilize
            return port
        time.sleep(0.5)

    return None

def try_flash(port, attempts=3):
    """Try to flash firmware via esptool."""
    cmd = [c if c is not None else port for c in ESPTOOL_CMD]

    for attempt in range(attempts):
        print(f"\nFlash attempt {attempt + 1}/{attempts}...")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)

        print(result.stdout)
        if result.stderr:
            print(result.stderr)

        if result.returncode == 0:
            print("\n✅ FLASH SUCCESSFUL!")
            return True

        if "No serial data received" in result.stderr or "Failed to connect" in result.stderr:
            print("Chip not in download mode — waiting for BOOT button...")
            time.sleep(2)
            continue

        print(f"Flash failed with exit code {result.returncode}")
        break

    return False

def main():
    print("=== XIAO ESP32-S3 Auto-Flash Script ===")
    print()

    # Verify firmware files exist
    for addr, path in [("0x0", "bootloader/bootloader.bin"),
                       ("0x8000", "partition_table/partition-table.bin"),
                       ("0xd000", "ota_data_initial.bin"),
                       ("0xb0000", "ws_meeting_demo.bin")]:
        full_path = os.path.join(FIRMWARE_DIR, path)
        if not os.path.exists(full_path):
            print(f"ERROR: {full_path} not found!")
            return
        size = os.path.getsize(full_path)
        print(f"  {addr}: {path} ({size} bytes)")

    print()

    # First try: check if chip is already in download mode
    port = find_esp32_port()
    if port:
        print(f"Found port: {port}")
        if try_flash(port):
            return
        print("Direct flash failed — chip not in download mode.")

    # Second try: wait for user to enter download mode
    print()
    print("=== MANUAL BOOT MODE ENTRY REQUIRED ===")
    print("1. UNPLUG USB-C cable from XIAO")
    print("2. HOLD BOOT button (bottom side, near USB-C)")
    print("3. PLUG USB-C cable back in")
    print("4. RELEASE BOOT after 1 second")
    print()

    # Wait for port to appear (user will replug USB)
    # The port might disappear and reappear
    print("Monitoring for serial port changes...")

    # Wait a bit, then scan for ports
    port = None
    for i in range(120):  # 60 seconds
        port = find_esp32_port()
        if port:
            # Try to connect — chip might be in download mode
            result = subprocess.run(
                ["python3", "-m", "esptool", "--chip", "esp32s3", "--port", port,
                 "--before", "no_reset", "--baud", "115200", "flash_id"],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0:
                print(f"✅ CHIP IN DOWNLOAD MODE! Port: {port}")
                print(result.stdout)
                # Now flash at higher speed
                if try_flash(port):
                    return
            else:
                if i % 10 == 0:
                    print(f"  Waiting... (scan {i}, port {port} exists but chip not in download mode)")
        time.sleep(0.5)

    print("Timeout — chip never entered download mode.")

if __name__ == "__main__":
    main()