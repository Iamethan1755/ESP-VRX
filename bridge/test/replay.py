# SPDX-License-Identifier: GPL-3.0-only
"""Replay a raw C5 recording (tools/bridge_tool.py rawlong) through the decoder.

Usage: python bridge/test/replay.py captures/rawlong_*_20MHz.bin [FS_HZ] [median|smooth]
Prints lock/field events; drawn fields go to captures/replay/<name>_NN.png.
"""
import pathlib
import re
import subprocess
import sys

from run_host_tests import HERE, MAIN, pgm_to_png


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rec = pathlib.Path(sys.argv[1]).resolve()
    m = re.search(r"_(\d+)MHz", rec.name)
    fs = sys.argv[2] if len(sys.argv) > 2 else str(int(m.group(1)) * 1000000 if m else 20000000)
    filt = sys.argv[3] if len(sys.argv) > 3 else "median"
    exe = HERE / ("replay.exe" if sys.platform == "win32" else "replay")
    cmd = [sys.executable, "-m", "ziglang", "cc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
           f"-I{MAIN}", str(HERE / "replay.c"), str(MAIN / "video.c"), "-o", str(exe)]
    if subprocess.call(cmd) != 0:
        return 1
    out = rec.parent / "replay"
    out.mkdir(exist_ok=True)
    for old in out.glob(f"{rec.stem}_{filt}_*.png"):
        old.unlink()
    prefix = out / f"{rec.stem}_{filt}"
    rc = subprocess.call([str(exe), str(rec), fs, filt, str(prefix)])
    for pgm in sorted(out.glob(f"{rec.stem}_{filt}_*.pgm")):
        pgm_to_png(pgm)
        pgm.unlink()
    return rc


if __name__ == "__main__":
    sys.exit(main())
