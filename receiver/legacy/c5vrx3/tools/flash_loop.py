#!/usr/bin/env python3
"""Wait for ESP32-C5 ROM bootloader and flash immediately."""
import sys
import time
import subprocess
from pathlib import Path
import serial.tools.list_ports

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
bootloader = BUILD / "bootloader/bootloader.bin"
ptable = BUILD / "partition_table/partition-table.bin"
app = BUILD / "c5vrx3.bin"

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"

cmd = [
    sys.executable, "-m", "esptool",
    "--chip", "esp32c5",
    "-p", port,
    "-b", "460800",
    "--before", "no-reset",
    "--after", "watchdog-reset",
    "write-flash",
    "--flash-mode", "dio",
    "--flash-size", "8MB",
    "--flash-freq", "80m",
    "0x2000", str(bootloader),
    "0x8000", str(ptable),
    "0x10000", str(app),
]

print(f"Waiting for ESP32-C5 on {port} in bootloader mode (Hold BOOT, tap RESET, release BOOT)...")
while True:
    res = subprocess.run(cmd)
    if res.returncode == 0:
        print("\n>>> FLASH COMPLETED SUCCESSFULLY! <<<")
        break
    time.sleep(0.5)
