#!/usr/bin/env python3
"""Continuous Direct Gain range log for walk and attenuator tests.

Polls the lab snapshot ('p') and appends every DG3_OBS field to a CSV with a
host timestamp. Type a note (e.g. "picture lost", "30 dB") and press Enter to
add a marker row, so the log shows exactly where the picture failed.

    python tools/range_logger.py COM10 walk1.csv [--period 0.5]

The port is opened with DTR/RTS deasserted (USB-Serial-JTAG otherwise resets
or stalls the ESP32-C5 console).
"""

import argparse
import csv
import queue
import re
import sys
import threading
import time

import serial

FIELD = re.compile(r"(\w+)=(-?\d+)")


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.dtr = False
    port.rts = False
    port.timeout = 0.1
    for _ in range(60):
        try:
            port.open()
            return port
        except serial.SerialException:
            time.sleep(0.5)
    raise SystemExit(f"cannot open {name}")


def notes(out):
    for line in sys.stdin:
        out.put(line.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("csv")
    ap.add_argument("--period", type=float, default=0.5)
    args = ap.parse_args()

    port = open_port(args.port)
    marks = queue.Queue()
    threading.Thread(target=notes, args=(marks,), daemon=True).start()
    columns = None
    rows = 0
    with open(args.csv, "w", newline="", encoding="utf-8") as fh:
        writer = None
        buffer = b""
        next_poll = 0.0
        start = time.time()
        while True:
            now = time.time()
            if now >= next_poll:
                port.write(b"p")
                next_poll = now + args.period
            buffer += port.read(8192)
            *lines, buffer = buffer.split(b"\n")
            for raw in lines:
                line = raw.decode(errors="replace")
                if "DG3_OBS" not in line:
                    continue
                values = dict(FIELD.findall(line))
                if columns is None:
                    columns = ["t_s", "note"] + list(values)
                    writer = csv.DictWriter(fh, fieldnames=columns,
                                            extrasaction="ignore")
                    writer.writeheader()
                values["t_s"] = f"{now - start:.2f}"
                values["note"] = ""
                writer.writerow(values)
                rows += 1
                print(f"\r{rows:5d}  gain={values.get('gain')} "
                      f"lane={values.get('lane')} p50={values.get('p50')} "
                      f"coh={values.get('coherence')} bw40={values.get('bw40')}   ",
                      end="", flush=True)
            while not marks.empty() and writer is not None:
                writer.writerow({"t_s": f"{now - start:.2f}", "note": marks.get()})
                print("  [marked]")
            fh.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
