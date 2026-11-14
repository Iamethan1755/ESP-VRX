# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for the bridge's web side, to work on the page without hardware:
the page and /status on 127.0.0.1:8091, an MJPEG test stream (moving bar,
frame counter) on 127.0.0.1:81, like the S3 (page on 80, stream on 81).

  python tools/mock_bridge.py [--fps 30] [--pal]

The page's buttons work as on the bridge (/cmd toggles show in /status).
To see how the page copes when things go wrong:

  --no-receiver       /c5 answers 503, as when the receiver is not listening
  --freeze-after N    the stream stops sending after N seconds (connection kept)
  --status-hang N     /status stops answering after N seconds

Needs Pillow (comes with matplotlib).
"""
import argparse
import http.server
import io
import json
import pathlib
import threading
import time

from PIL import Image, ImageDraw

WEB = pathlib.Path(__file__).resolve().parent.parent / "bridge" / "main" / "web"
BOUNDARY = "vrxframe"


def frame(n: int, w: int, h: int, colour: bool) -> bytes:
    img = Image.new("RGB" if colour else "L", (w, h), (40, 40, 40) if colour else 40)
    d = ImageDraw.Draw(img)
    bars = [(235, 235, 235), (235, 235, 16), (16, 235, 235), (16, 235, 16), (235, 16, 235), (235, 16, 16), (16, 16, 235),
            (16, 16, 16)]
    for k in range(8):   # bars
        d.rectangle([k * w // 8, 0, (k + 1) * w // 8, h // 3], fill=bars[k] if colour else 255 - k * 30)
    x = (n * 4) % w      # moving bar
    d.rectangle([x, h // 3, x + 12, h - 30], fill=(230, 230, 230) if colour else 230)
    d.ellipse([w // 2 - 60, h // 2 - 45, w // 2 + 60, h // 2 + 45], outline=(200, 200, 200) if colour else 200, width=3)
    d.text((8, h - 22), f"MOCK frame {n}", fill=(255, 255, 255) if colour else 255)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=60)
    return out.getvalue()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--pal", action="store_true")
    ap.add_argument("--no-receiver", action="store_true", help="/c5 answers 503")
    ap.add_argument("--freeze-after", type=float, default=0, metavar="N", help="the stream stops sending after N s")
    ap.add_argument("--status-hang", type=float, default=0, metavar="N", help="/status stops answering after N s")
    a = ap.parse_args()
    w, h = 320, 288 if a.pal else 240
    # The keys of the bridge's /status (bridge_status_json in bridge/main/main.c);
    # tools/check_status_keys.py compares the two and the page.
    status = {"version": "mock", "source": "c5", "capture_ok": True, "h_locked": True, "v_locked": True,
              "std": "PAL" if a.pal else "NTSC", "lines_per_field": 312 if a.pal else 262, "line_us": 63.55,
              "sync": 15.0, "blank": 21.0, "black": 18.0, "white": 40.0, "levels": "auto", "filter": "median",
              "denoise": True, "colour": True, "colour_shown": not a.pal, "burst": 2.7, "colour_noisy": False,
              "burst_noise": 9.0, "colour_sat": 1.0, "slips": 0, "clock_ppm": 1.9, "fields": 0, "overruns": 0,
              "catchups": 0, "lock_losses": 0, "h_offset_us": 0.0, "v_offset": 0, "msps": 20.0, "load": 0.6,
              "fps": a.fps, "pipe_ms": 28.0, "pipe_max_ms": 41.0, "jpeg_bytes": 0, "frames": 0, "clients": 0,
              "stations": 1, "c5_keys": 0, "c5_channel": -1, "heap_int": 140000, "heap_psram": 7900000, "uptime": 0,
              "reset": "mock"}
    t0 = time.time()

    def command(c: str) -> bool:
        if c == "f":
            status["filter"] = "smooth" if status["filter"] == "median" else "median"
        elif c == "d":
            status["denoise"] = not status["denoise"]
        elif c == "c":
            status["levels"] = "std" if status["levels"] == "auto" else "auto"
        elif c == "m":
            status["colour"] = not status["colour"]
            status["colour_shown"] = status["colour"] and not a.pal
        elif c == "t":
            order = ["c5", "test-ntsc", "test-pal"]
            status["source"] = order[(order.index(status["source"]) + 1) % 3]
        elif c in "jl":
            status["h_offset_us"] = round(status["h_offset_us"] + (0.2 if c == "j" else -0.2), 1)
        elif c in "ik":
            status["v_offset"] += 1 if c == "i" else -1
        elif c == "z":
            status.update(filter="median", denoise=True, levels="auto", colour=True, colour_shown=not a.pal,
                          h_offset_us=0.0, v_offset=0)
        else:
            return False
        return True

    class Page(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def reply(self, body: bytes, kind: str, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path, _, query = self.path.partition("?")
            args = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
            if path == "/":
                self.reply((WEB / "index.html").read_bytes(), "text/html")
            elif path in ("/icon.png", "/favicon.ico", "/apple-touch-icon.png", "/apple-touch-icon-precomposed.png"):
                self.reply((WEB / "icon.png").read_bytes(), "image/png")
            elif path == "/status":
                if a.status_hang and time.time() - t0 > a.status_hang:
                    time.sleep(3600)   # never answers
                status["uptime"] = int(time.time() - t0)
                self.reply(json.dumps(status).encode(), "application/json")
            elif path == "/snapshot.jpg":
                self.reply(frame(0, w, h, status["colour_shown"]), "image/jpeg")
            elif path == "/c5":
                ch, key = args.get("ch"), args.get("k", "")
                if ch is not None and not (ch.isdigit() and int(ch) <= 47):
                    return self.send_error(400, "channel 0..47")
                if ch is None and (len(key) != 1 or key not in "ocbsnxve"):
                    return self.send_error(400, "unknown receiver key")
                if a.no_receiver:
                    return self.reply(b"receiver not listening on the key line", "text/plain", 503)
                status["c5_keys"] += 1
                if ch is not None:
                    status["c5_channel"] = int(ch)
                elif key in "bcsx":
                    status["c5_channel"] = -1   # like the bridge: the receiver picks the channel
                self.reply(b"ok", "text/plain")
            elif path == "/cmd":
                if not command(args.get("c", "")):
                    return self.send_error(400, "unknown command")
                self.reply(b"ok", "text/plain")
            else:
                self.send_error(404)

    class Stream(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def do_GET(self) -> None:
            if not self.path.startswith("/stream"):
                return self.send_error(404)
            self.send_response(200)
            if "raw=1" in self.path:   # like the bridge: plain binary for the page's reader
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("X-Content-Type-Options", "nosniff")
            else:
                self.send_header("Content-Type", f"multipart/x-mixed-replace;boundary={BOUNDARY}")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            n = 0
            status["clients"] += 1
            try:
                while True:
                    if a.freeze_after and time.time() - t0 > a.freeze_after:
                        time.sleep(1)   # connection open, nothing sent
                        continue
                    jpeg = frame(n, w, h, status["colour_shown"])
                    status["jpeg_bytes"] = len(jpeg)
                    status["frames"] += 1
                    self.wfile.write(f"--{BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg)}\r\n\r\n".encode()
                                     + jpeg + b"\r\n")
                    n += 1
                    time.sleep(1 / a.fps)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            finally:
                status["clients"] -= 1

    for port, handler in ((81, Stream), (8091, Page)):
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("mock bridge: page http://127.0.0.1:8091/  stream http://127.0.0.1:81/stream", flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
