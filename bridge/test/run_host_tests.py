# SPDX-License-Identifier: GPL-3.0-only
"""Build and run the bridge's host tests with Zig's C compiler (pip install ziglang).

Usage: python bridge/test/run_host_tests.py
Writes test_out_*.pgm frames and converts them to PNG next to this script.
"""
import pathlib
import struct
import subprocess
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
MAIN = HERE.parent / "main"


def pgm_to_png(pgm: pathlib.Path) -> pathlib.Path:
    data = pgm.read_bytes()
    parts = data.split(b"\n", 3)
    w, h = (int(v) for v in parts[1].split())
    pixels = parts[3]
    rows = b"".join(b"\x00" + pixels[y * w:(y + 1) * w] for y in range(h))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    out = pgm.with_suffix(".png")
    out.write_bytes(png)
    return out


def main() -> int:
    exe = HERE / ("test_video.exe" if sys.platform == "win32" else "test_video")
    cmd = [sys.executable, "-m", "ziglang", "cc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
           f"-I{MAIN}", str(HERE / "test_video.c"), str(MAIN / "video.c"), str(MAIN / "testpattern.c"), str(MAIN / "denoise.c"),
           str(MAIN / "mbox.c"),
           "-o", str(exe)]
    if subprocess.call(cmd) != 0:
        return 1
    rc = subprocess.call([str(exe)], cwd=HERE)
    for pgm in sorted(HERE.glob("test_out_*.pgm")):
        pgm_to_png(pgm)
        pgm.unlink()
    return rc


if __name__ == "__main__":
    sys.exit(main())
