# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for the bridge's web side, to work on the page without hardware:
the page and /status on 127.0.0.1:8091, an MJPEG test stream (moving bar,
frame counter) on 127.0.0.1:81, like the S3 (page on 80, stream on 81).

  python tools/mock_bridge.py [--fps 30] [--pal]
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


def frame(n: int, w: int, h: int) -> bytes:
    img = Image.new("L", (w, h), 40)
    d = ImageDraw.Draw(img)
    for k in range(8):   # bars
        d.rectangle([k * w // 8, 0, (k + 1) * w // 8, h // 3], fill=255 - k * 30)
    x = (n * 4) % w      # moving bar
    d.rectangle([x, h // 3, x + 12, h - 30], fill=230)
    d.ellipse([w // 2 - 60, h // 2 - 45, w // 2 + 60, h // 2 + 45], outline=200, width=3)
    d.text((8, h - 22), f"MOCK frame {n}", fill=255)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=60)
    return out.getvalue()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--pal", action="store_true")
    a = ap.parse_args()
    w, h = 320, 288 if a.pal else 240
    status = {"source": "c5", "capture_ok": True, "h_locked": True, "v_locked": True, "std": "PAL" if a.pal else "NTSC",
              "lines_per_field": 312 if a.pal else 262, "line_us": 63.55, "sync": 15.0, "blank": 21.0, "black": 18.0,
              "white": 40.0, "levels": "auto", "filter": "median", "denoise": True, "fields": 0, "overruns": 0,
              "lock_losses": 0, "h_offset_us": 0.0, "v_offset": 0, "msps": 20.0, "load": 0.6, "fps": a.fps,
              "jpeg_bytes": 0, "frames": 0, "clients": 0, "stations": 1, "c5_keys": 0, "heap_int": 140000,
              "heap_psram": 7900000, "uptime": 0, "reset": "mock", "c5_channel": -1}
    t0 = time.time()

    class Page(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def reply(self, body: bytes, kind: str) -> None:
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path = self.path.split("?")[0]
            if path == "/":
                self.reply((WEB / "index.html").read_bytes(), "text/html")
            elif path == "/status":
                status["uptime"] = int(time.time() - t0)
                self.reply(json.dumps(status).encode(), "application/json")
            elif path == "/snapshot.jpg":
                self.reply(frame(0, w, h), "image/jpeg")
            elif path == "/c5" and "ch=" in self.path:
                val = self.path.split("ch=")[1].split("&")[0]
                if not val.isdigit() or int(val) > 47:
                    return self.send_error(400, "channel 0..47")
                status["c5_channel"] = int(val)
                self.reply(b"ok", "text/plain")
            elif path == "/c5" and any(f"k={k}" in self.path for k in "bcsx"):
                status["c5_channel"] = -1   # like the bridge: the receiver picks the channel
                self.reply(b"ok", "text/plain")
            elif path in ("/cmd", "/c5"):
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
            try:
                while True:
                    jpeg = frame(n, w, h)
                    status["jpeg_bytes"] = len(jpeg)
                    self.wfile.write(f"--{BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg)}\r\n\r\n".encode()
                                     + jpeg + b"\r\n")
                    n += 1
                    time.sleep(1 / a.fps)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    for port, handler in ((81, Stream), (8091, Page)):
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("mock bridge: page http://127.0.0.1:8091/  stream http://127.0.0.1:81/stream", flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
