#!/usr/bin/env python3
"""Launch local HTTP server and open C5VRX Web Flasher in the browser."""

import http.server
import socketserver
import webbrowser
import threading
import time
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = ROOT / "web"
PORT = 8080


class FlasherHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, format, *args):
        # Concise logging
        sys.stderr.write(f"[HTTP] {self.address_string()} - {format % args}\n")


def start_server():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), FlasherHTTPRequestHandler) as httpd:
        print(f"=======================================================")
        print(f" C5VRX WEB FLASHER LOCAL SERVER")
        print(f" Serving at: http://localhost:{PORT}/web/index.html")
        print(f" Press Ctrl+C to stop the server.")
        print(f"=======================================================")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")


def main():
    if not (WEB_DIR / "index.html").exists():
        print(f"Error: web/index.html not found in {WEB_DIR}")
        sys.exit(1)

    url = f"http://localhost:{PORT}/web/index.html"
    print(f"Opening browser at {url} ...")

    # Start server in thread or open browser after brief delay
    threading.Thread(target=lambda: (time.sleep(0.5), webbrowser.open(url)), daemon=True).start()
    start_server()


if __name__ == "__main__":
    main()
