# SPDX-License-Identifier: GPL-3.0-only
"""Draws the phone page's icon (browser tab, home screen): a screen with a
radio signal coming in, in the page's colours.

  python tools/make_icon.py        -> bridge/main/web/icon.png (180x180)
Needs Pillow (comes with matplotlib). The PNG is committed; rerun this only
to change the drawing.
"""
import pathlib

from PIL import Image, ImageDraw

OUT = pathlib.Path(__file__).resolve().parent.parent / "bridge" / "main" / "web" / "icon.png"
SIZE, SS = 180, 4                      # drawn 4x larger, then scaled down (smooth edges)
BG, PANEL, TEXT, OK = "#0b0d10", "#161a20", "#e8eaed", "#3fb950"


def main() -> None:
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)

    def px(v: float) -> int:
        return round(v * SS)

    # The screen: a 4:3 panel with a light frame.
    left, top, right, bottom = 28, 76, 152, 160
    d.rounded_rectangle([px(left), px(top), px(right), px(bottom)], radius=px(12), fill=PANEL, outline=TEXT, width=px(7))
    # The live dot, as in the page's badge.
    d.ellipse([px(83), px(111), px(97), px(125)], fill=OK)
    # The signal: three arcs coming in from the top right, clear of the screen.
    cx, cy = 146, 30
    for r in (13, 25, 37):
        d.arc([px(cx - r), px(cy - r), px(cx + r), px(cy + r)], start=105, end=195, fill=OK, width=px(6))
    d.ellipse([px(cx - 5), px(cy - 5), px(cx + 5), px(cy + 5)], fill=OK)

    img = img.resize((SIZE, SIZE), Image.LANCZOS).quantize(colors=32, method=Image.Quantize.MEDIANCUT)
    img.save(OUT, "PNG", optimize=True)
    print(f"{OUT} {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
