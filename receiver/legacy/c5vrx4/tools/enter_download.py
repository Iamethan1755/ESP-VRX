#!/usr/bin/env python3
"""Put a running C5VRX-4 board into USB download mode, then exit.

esptool's own USB reset does not work on a running C5VRX-4 image: the RTS/DTR
reset leaves the modem domain (forced clocks, the always-on dump writer)
running into the ROM loader, whose USB download loop then stops answering.
This sends the console key '`', which makes the firmware stop the writer and
Wi-Fi, request a download boot and esp_restart() (main/rf.c,
rf_reboot_to_download). Flash afterwards with esptool --before no-reset.

  python tools/enter_download.py COM33 && esptool --before no-reset ...

If the console is starved (very old images, heavy load) the key is not read;
then use the BOOT button (hold BOOT, tap RESET).
"""
import sys
import time

import serial

ACK = b"download_boot=requested"


def enter_download(port, attempts=5, settle_s=1.5, log=print):
    for attempt in range(1, attempts + 1):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.05, 0.5
        s.dtr = False
        s.rts = False
        try:
            s.open()
        except serial.SerialException as e:
            log(f"enter_download: open {port} failed ({e}); retrying")
            time.sleep(0.5)
            continue
        seen = b""
        try:
            s.reset_input_buffer()
            s.write(b"`")
            deadline = time.time() + 1.0
            while time.time() < deadline and ACK not in seen:
                seen = (seen + s.read(512))[-4096:]
        except serial.SerialTimeoutException:
            log(f"enter_download: attempt {attempt}: console not reading (write timeout)")
        except serial.SerialException:
            # The port vanishing mid-read means the restart already happened.
            seen += ACK
        finally:
            try:
                s.close()
            except serial.SerialException:
                pass
        if ACK in seen:
            log("enter_download: firmware acknowledged, waiting for download mode")
            time.sleep(settle_s)
            return True
        time.sleep(0.3)
    log("enter_download: no acknowledgement; if this board is already in the ROM "
        "loader that is fine, otherwise hold BOOT and tap RESET")
    return False


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(0 if enter_download(sys.argv[1]) else 1)
