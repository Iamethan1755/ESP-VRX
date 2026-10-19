#!/usr/bin/env python3
"""USB console soak test for C5VRX-4 (read-only).

Sends a read-only command ('p' = SNAPSHOT row) at a fixed rate and measures,
per command: whether the host write was accepted, and how long until the
SNAPSHOT reply arrived. Also tracks "HB console" / "HB predemod" heartbeat
gaps, task watchdog lines and resets. Writes a CSV and prints a summary.

  python tools/console_soak.py COM33 --seconds 60 --period 0.2 --csv soak.csv

A starved console task shows up as write timeouts (the host cannot hand the
64-byte USB OUT FIFO to the chip) and as long reply latencies.
"""
import argparse
import csv
import re
import sys
import threading
import time

import serial

WATCHDOG = re.compile(r"task_wdt|Task watchdog", re.I)
RESET = re.compile(r"rst:0x|ESP-ROM:|Guru Meditation|abort\(\)|panic", re.I)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--period", type=float, default=0.2)
    ap.add_argument("--command", default="p")
    ap.add_argument("--reply", default="SNAPSHOT")
    ap.add_argument("--reply-timeout", type=float, default=2.0)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--log", default=None, help="raw console log file")
    args = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = args.port, 115200, 0.05, 0.5
    s.dtr = False
    s.rts = False
    s.open()

    lock = threading.Lock()
    pending = []          # send times awaiting a reply, oldest first
    rows = []             # (t_send, write_ok, latency_s or None)
    events = {"wdt": 0, "reset": 0}
    hb = {"HB console": [], "HB predemod": []}
    stop = threading.Event()
    raw = open(args.log, "w", encoding="utf-8") if args.log else None

    def reader():
        tail = b""
        while not stop.is_set():
            try:
                chunk = s.read(4096)
            except serial.SerialException as e:
                print(f"read error: {e}", file=sys.stderr)
                return
            if not chunk:
                continue
            tail += chunk
            *lines, tail = tail.split(b"\n")
            now = time.time()
            for line in lines:
                text = line.decode(errors="replace").rstrip("\r")
                if raw:
                    raw.write(f"{now:.3f} {text}\n")
                for key in hb:
                    if text.startswith(key):
                        hb[key].append(now)
                if WATCHDOG.search(text):
                    events["wdt"] += 1
                if RESET.search(text):
                    events["reset"] += 1
                if args.reply in text:
                    with lock:
                        if pending:
                            idx, t0 = pending.pop(0)
                            rows[idx] = (rows[idx][0], True, now - t0)

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(1.0)

    start = time.time()
    nxt = start
    cmd = args.command.encode()
    while time.time() - start < args.seconds:
        now = time.time()
        if now < nxt:
            time.sleep(min(0.01, nxt - now))
            continue
        nxt += args.period
        with lock:
            # Replies older than the timeout are counted lost.
            while pending and time.time() - pending[0][1] > args.reply_timeout:
                pending.pop(0)
            # Register before writing, and prevent the reader from matching
            # a reply until the write result is known.
            t0 = time.time()
            idx = len(rows)
            rows.append((t0, True, None))
            pending.append((idx, t0))
            try:
                s.write(cmd)
            except serial.SerialTimeoutException:
                rows[idx] = (t0, False, None)
                pending.pop()
    time.sleep(args.reply_timeout)
    stop.set()
    th.join(1.0)
    s.close()
    if raw:
        raw.close()

    sent = len(rows)
    write_fail = sum(1 for r in rows if not r[1])
    lat = sorted(r[2] for r in rows if r[2] is not None)
    lost = sent - write_fail - len(lat)

    def pct(p):
        return lat[min(len(lat) - 1, int(p * len(lat)))] * 1000 if lat else float("nan")

    print(f"commands={sent} write_timeouts={write_fail} replies={len(lat)} lost={lost}")
    if lat:
        print(f"latency_ms p50={pct(0.5):.1f} p90={pct(0.9):.1f} p99={pct(0.99):.1f} max={lat[-1]*1000:.1f}")
    for key, times in hb.items():
        gaps = [b - a for a, b in zip(times, times[1:])]
        print(f"{key}: beats={len(times)} max_gap_s={max(gaps):.1f}" if gaps else f"{key}: beats={len(times)}")
    print(f"watchdog_lines={events['wdt']} reset_lines={events['reset']}")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_rel_s", "write_ok", "latency_ms"])
            for t0, ok, l in rows:
                w.writerow([f"{t0 - start:.3f}", int(ok), "" if l is None else f"{l*1000:.1f}"])
    return 0 if write_fail == 0 and lost == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
