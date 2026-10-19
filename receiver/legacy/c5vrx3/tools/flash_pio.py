#!/usr/bin/env python3
"""Flash a PlatformIO build of C5VRX to the XIAO ESP32-C5, the proven way.

usage: python tools/flash_pio.py [ENV] [PORT]      (default env xiao_c5, port auto)

The XIAO's USB-Serial/JTAG auto-reset is unreliable with C5VRX firmware
running, so this uses the ROM download mode entered by hand:

1. If the chip is already in the ROM bootloader, it is written at once.
2. Otherwise (the application is running) the script asks you to unplug the
   XIAO, hold BOOT, plug it in and release BOOT; it waits for the port to go
   away and come back, and writes then.

Always DIO at 80 MHz (the XIAO's flash does not boot in QIO), --before
no-reset, --after watchdog-reset (the application starts by itself; no RESET
press). Never asserts DTR/RTS.
"""
import subprocess
import sys
import time
from pathlib import Path

import serial.tools.list_ports

ROOT = Path(__file__).resolve().parent.parent
env = sys.argv[1] if len(sys.argv) > 1 else "xiao_c5"
port_arg = sys.argv[2] if len(sys.argv) > 2 else None
build = ROOT / ".pio" / "build" / env

images = {
    "0x2000": build / "bootloader.bin",
    "0x8000": build / "partitions.bin",
    "0x10000": build / "firmware.bin",
}
for path in images.values():
    if not path.is_file():
        raise SystemExit(f"missing {path}; run: pio run -e {env}")


def current_port():
    ports = [p for p in serial.tools.list_ports.comports()]
    if port_arg:
        return port_arg if port_arg in [p.device for p in ports] else None
    for p in ports:
        if "303A" in (p.hwid or "").upper():
            return p.device
    return None


def wait_port(present, timeout_s):
    """Wait until the port is there (present) or gone; (condition met, port)."""
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        port = current_port()
        if (port is not None) == present:
            return True, port
        time.sleep(0.1)
    return False, None


def write(port):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c5", "-p", port, "-b", "460800",
           "--before", "no-reset", "--after", "watchdog-reset", "--connect-attempts", "3",
           "write-flash", "--flash-mode", "dio", "--flash-size", "8MB", "--flash-freq", "80m"]
    for offset, path in images.items():
        cmd += [offset, str(path)]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ok = res.returncode == 0 and "Hash of data verified" in res.stdout
    last = [l for l in res.stdout.splitlines() if l.strip()][-1:] or [""]
    print(f"  esptool: {'OK' if ok else 'failed: ' + last[0][:120]}")
    return ok


found, port = wait_port(True, 10.0)
if not found:
    raise SystemExit("no Espressif USB-Serial/JTAG port found")
print(f"flashing {build} via {port}")
if write(port):
    sys.exit(0)
print("the application is running: unplug the XIAO, hold BOOT, plug it in, release BOOT")
gone, _ = wait_port(False, 120.0)
if not gone:
    raise SystemExit("the port never went away; run again after the replug")
back, port = wait_port(True, 120.0)
if not back:
    raise SystemExit("the port did not come back")
time.sleep(0.5)
if write(port):
    sys.exit(0)
raise SystemExit("write failed in download mode too; check the cable and run again")
