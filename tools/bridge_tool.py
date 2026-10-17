"""PC companion for the C5VRX Wi-Fi bridge (XIAO ESP32-S3) over its USB console.

  python tools/bridge_tool.py status            one status line
  python tools/bridge_tool.py raw               raw bus samples -> captures/raw_*.bin + .png plots
  python tools/bridge_tool.py frame             current JPEG -> captures/frame_*.jpg
  python tools/bridge_tool.py selftest          loopback test (NOTHING connected to D0..D5!)
  python tools/bridge_tool.py keys tc           send console keys (t source, c levels, j/l/i/k shift)
  python tools/bridge_tool.py ports             list serial ports

--port COM7 picks the port; otherwise the tool asks every Espressif USB port
for its help text and uses the one that answers as the bridge (a C5VRX
receiver on another port just prints its own help).
Needs: pip install pyserial numpy matplotlib (numpy/matplotlib only for plots).
"""
import argparse
import base64
import pathlib
import re
import sys
import time
import zlib

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
B64_LINE = re.compile(r"^[A-Za-z0-9+/=]+$")
OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "captures"


def open_port(name: str) -> serial.Serial:
    # DTR/RTS low: on the native USB console they would reset the chip.
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def read_until(s: serial.Serial, done, timeout: float) -> list[str]:
    lines, buf, end = [], b"", time.time() + timeout
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            lines.append(line)
            if done(line):
                return lines
    return lines


def find_bridge(port_arg: str | None) -> serial.Serial:
    if port_arg:
        return open_port(port_arg)
    for p in list_ports.comports():
        if p.vid != ESPRESSIF_VID:
            continue
        try:
            s = open_port(p.device)
        except serial.SerialException:
            continue
        s.reset_input_buffer()
        s.write(b"h")
        lines = read_until(s, lambda l: "Wi-Fi bridge keys" in l, 1.5)
        if any("Wi-Fi bridge keys" in l for l in lines):
            read_until(s, lambda l: False, 0.3)   # drain the rest of the help
            print(f"bridge on {p.device}")
            return s
        s.close()
    sys.exit("No bridge found. Plug in the XIAO ESP32-S3, or pass --port COMx (see: bridge_tool.py ports).")


def fetch_block(s: serial.Serial, key: bytes, tag: str, timeout: float = 20.0) -> tuple[dict, bytes]:
    s.reset_input_buffer()
    s.write(key)
    lines = read_until(s, lambda l: l.startswith(f"{tag} END") or l.startswith(f"{tag} ERROR"), timeout)
    head = next((l for l in lines if l.startswith(f"{tag} BEGIN")), None)
    tail = next((l for l in lines if l.startswith(f"{tag} END") or l.startswith(f"{tag} ERROR")), None)
    if not head or not tail or tail.startswith(f"{tag} ERROR"):
        sys.exit(tail or f"no {tag} data received")
    meta = dict(kv.split("=", 1) for kv in head.split()[2:])
    start = lines.index(head) + 1
    body = "".join(l for l in lines[start:lines.index(tail)] if B64_LINE.match(l))
    data = base64.b64decode(body)
    crc = tail.split("crc=", 1)[1]
    if f"{zlib.crc32(data):08x}" != crc:
        print(f"warning: CRC mismatch (got {zlib.crc32(data):08x}, device says {crc})")
    return meta, data


def plot_raw(data: bytes, fs: int, png: pathlib.Path) -> None:
    try:
        import numpy as np
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("numpy/matplotlib missing: raw data saved without plots")
        return
    x = np.frombuffer(data, dtype=np.uint8) & 63
    # Line period from the syncs: falling crossings of a slicer 20 % from the
    # sync tip towards the picture (as the decoder does), kept when they stay
    # low for 3.5 us, spaced about one line apart. NTSC 63.56 us, PAL 64 us.
    lo, hi = np.percentile(x, 2), np.percentile(x, 60)
    below = x < lo + max((hi - lo) * 0.2, 0.5)
    min_low = int(3.5e-6 * fs)
    edges = [e for e in np.flatnonzero(~below[:-1] & below[1:]) + 1
             if e + min_low < len(x) and below[e:e + min_low].all()]
    spacing = np.diff(edges)
    spacing = spacing[(spacing > 62e-6 * fs) & (spacing < 65.5e-6 * fs)]
    line = float(np.median(spacing)) if len(spacing) else fs * 63.5556e-6
    first = edges[0] if edges else 0
    fig, ax = plt.subplots(3, 1, figsize=(12, 11), gridspec_kw={"height_ratios": [1, 0.6, 1.6]})
    n = int(3 * line)
    t = np.arange(n) / fs * 1e6
    ax[0].plot(t, x[:n], lw=0.7)
    ax[0].set(title="First three lines of the C5 video bus", xlabel="µs", ylabel="DAC code (0..63)", ylim=(-1, 64))
    ax[1].bar(np.arange(64), np.bincount(x, minlength=64), width=1.0)
    ax[1].set(title="Level histogram (sync tip = lowest peak, then blanking)", xlabel="DAC code", yscale="log")
    # Stack lines at the measured period from the first sync: a straight dark
    # band means stable line timing; the picture shows between the bands.
    rows = int((len(x) - first) // line) - 1
    starts = (first + np.arange(rows) * line).astype(int)
    width = int(line)
    img = np.stack([x[s:s + width] for s in starts])
    ax[2].imshow(img, aspect="auto", cmap="gray", vmin=0, vmax=63, interpolation="nearest")
    found = f"{line / fs * 1e6:.2f} µs measured from {len(spacing)} sync pairs" if len(spacing) else "no syncs found, 63.56 µs assumed"
    ax[2].set(title=f"Lines stacked at {found} (sync = dark band at the left)", xlabel="sample", ylabel="line")
    fig.tight_layout()
    fig.savefig(png, dpi=110)
    print(f"plot: {png}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "raw", "frame", "selftest", "keys", "ports"])
    ap.add_argument("keys", nargs="?", default="", help="for 'keys': characters to send")
    ap.add_argument("--port")
    a = ap.parse_args()

    if a.command == "ports":
        for p in list_ports.comports():
            vid = f"{p.vid:04x}:{p.pid:04x}" if p.vid else "----:----"
            print(f"{p.device:8} {vid}  {p.description}")
        return 0

    s = find_bridge(a.port)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    if a.command == "status":
        s.reset_input_buffer()
        s.write(b"s")
        print(next((l for l in read_until(s, lambda l: l.startswith("STATUS"), 3) if l.startswith("STATUS")),
                   "no status received"))
    elif a.command == "raw":
        meta, data = fetch_block(s, b"R", "RAW")
        OUT_DIR.mkdir(exist_ok=True)
        out = OUT_DIR / f"raw_{stamp}.bin"
        out.write_bytes(data)
        print(f"{len(data)} samples at {int(meta['fs']) / 1e6:g} MS/s -> {out}")
        plot_raw(data, int(meta["fs"]), out.with_suffix(".png"))
    elif a.command == "frame":
        _, data = fetch_block(s, b"F", "JPEG")
        OUT_DIR.mkdir(exist_ok=True)
        out = OUT_DIR / f"frame_{stamp}.jpg"
        out.write_bytes(data)
        print(f"{len(data)} bytes -> {out}")
    elif a.command == "selftest":
        print("Drives the S3 data pins. Continue only if NOTHING is connected to D0..D5.")
        if input("type yes: ").strip().lower() != "yes":
            return 1
        s.reset_input_buffer()
        s.write(b"L")
        read_until(s, lambda l: "press y" in l, 3)
        s.write(b"y")
        lines = read_until(s, lambda l: l.startswith("SELFTEST") and ("PASS" in l or "FAIL" in l), 10)
        print(next((l for l in reversed(lines) if l.startswith("SELFTEST")), "no result"))
    elif a.command == "keys":
        for ch in a.keys:
            s.write(ch.encode())
            print("\n".join(l for l in read_until(s, lambda l: l.startswith("COMMAND"), 2) if l.startswith("COMMAND")))
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
