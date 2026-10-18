"""Generate docs/wiring.svg: how the XIAO ESP32-C5 receiver and the XIAO
ESP32-S3 Wi-Fi bridge are wired. Edit the tables below and rerun:

    python tools/make_wiring_svg.py
"""
import pathlib
from xml.sax.saxutils import escape

OUT = pathlib.Path(__file__).resolve().parent.parent / "docs" / "wiring.svg"

# Wire colours follow the resistor colour code: bit 0 brown ... bit 5 blue.
BIT_COLOURS = ["#8B4513", "#D62728", "#F28E1C", "#C9A400", "#2CA02C", "#1F77B4"]
LINK = "#E8590C"
GND = "#222222"
DNC = "#C92A2A"
MUTED = "#8A94A3"
INK = "#1D2733"

# (C5 pin, C5 GPIO, S3 pin, S3 GPIO) per video bit, LSB first.
VIDEO = [
    ("D4", "GPIO23", "D0", "GPIO1"),
    ("D5", "GPIO24", "D1", "GPIO2"),
    ("D6", "GPIO11", "D2", "GPIO3"),
    ("D7", "GPIO12", "D3", "GPIO4"),
    ("D8", "GPIO8", "D4", "GPIO5"),
    ("D9", "GPIO9", "D5", "GPIO6"),
]

# Physical XIAO pinout, top view, USB-C up: left column D0..D6 top to
# bottom, right column 5V, GND, 3V3, D10, D9, D8, D7.
# Each entry: (pin, gpio, kind, note). kind: bit0..bit5, gnd, link, dnc, free, power.
C5_LEFT = [
    ("D0", "GPIO1", "dnc", "leave free (IQ)"),
    ("D1", "GPIO0", "dnc", "leave free (IQ)"),
    ("D2", "GPIO25", "dnc", "leave free (IQ)"),
    ("D3", "GPIO7", "dnc", "leave free (IQ)"),
    ("D4", "GPIO23", "bit0", "bit 0"),
    ("D5", "GPIO24", "bit1", "bit 1"),
    ("D6", "GPIO11", "bit2", "bit 2"),
]
C5_RIGHT = [
    ("5V", "", "power", "not to S3 5V"),
    ("GND", "", "gnd", "to S3 GND"),
    ("3V3", "", "free", "not used"),
    ("D10", "GPIO10", "dnc", "leave free (IQ)"),
    ("D9", "GPIO9", "bit5", "bit 5"),
    ("D8", "GPIO8", "bit4", "bit 4"),
    ("D7", "GPIO12", "bit3", "bit 3"),
]
S3_LEFT = [
    ("D0", "GPIO1", "bit0", "bit 0"),
    ("D1", "GPIO2", "bit1", "bit 1"),
    ("D2", "GPIO3", "bit2", "bit 2"),
    ("D3", "GPIO4", "bit3", "bit 3"),
    ("D4", "GPIO5", "bit4", "bit 4"),
    ("D5", "GPIO6", "bit5", "bit 5"),
    ("D6", "GPIO43", "dnc", "avoid (boot log)"),
]
S3_RIGHT = [
    ("5V", "", "power", "not to C5 5V"),
    ("GND", "", "gnd", "to C5 GND"),
    ("3V3", "", "free", "not used"),
    ("D10", "GPIO9", "free", "free"),
    ("D9", "GPIO8", "free", "free"),
    ("D8", "GPIO7", "link", "link to C5 GPIO2"),
    ("D7", "GPIO44", "free", "free"),
]

W, H = 1400, 1450
parts = []


def add(s):
    parts.append(s)


def text(x, y, s, size=14, weight="normal", fill=INK, anchor="start", family=None, style=None):
    fam = f' font-family="{family}"' if family else ""
    st = f' font-style="{style}"' if style else ""
    add(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
        f'text-anchor="{anchor}"{fam}{st}>{escape(s)}</text>')


def kind_colour(kind):
    if kind.startswith("bit"):
        return BIT_COLOURS[int(kind[3])]
    return {"gnd": GND, "link": LINK, "dnc": DNC}.get(kind, MUTED)


def pin_marker(x, y, kind):
    c = kind_colour(kind)
    if kind == "dnc":
        add(f'<circle cx="{x}" cy="{y}" r="7.5" fill="#fff" stroke="{c}" stroke-width="2"/>')
        add(f'<path d="M{x-4} {y-4}L{x+4} {y+4}M{x-4} {y+4}L{x+4} {y-4}" stroke="{c}" stroke-width="2"/>')
    elif kind in ("free", "power"):
        add(f'<circle cx="{x}" cy="{y}" r="7.5" fill="#fff" stroke="{MUTED}" stroke-width="2"/>')
    else:
        add(f'<circle cx="{x}" cy="{y}" r="7.5" fill="{c}" stroke="#fff" stroke-width="1.5"/>')


def antenna(x, y, label, anchor):
    add(f'<path d="M{x} {y}V{y-24}M{x-9} {y-36}L{x} {y-24}L{x+9} {y-36}" stroke="{INK}" '
        f'stroke-width="2" fill="none"/>')
    dx = 14 if anchor == "start" else -14
    text(x + dx, y - 22, label, 13, fill=INK, anchor=anchor)


def resistor_h(x1, x2, y, colour, label):
    """Horizontal IEC resistor (box) between x1 and x2, centred."""
    mid = (x1 + x2) / 2
    add(f'<rect x="{mid-30}" y="{y-9}" width="60" height="18" rx="2" fill="#fff" stroke="{colour}" stroke-width="2"/>')
    text(mid, y + 5, label, 12, "600", colour, "middle")


# ---- background and title ------------------------------------------------
add(f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>')
text(40, 48, "C5VRX Wi-Fi bridge: wiring", 28, "700")
text(40, 74, "XIAO ESP32-C5 (5.8 GHz receiver, C5VRX) to XIAO ESP32-S3 (Wi-Fi bridge). "
             "Seven wires carry the video; one optional wire lets the phone change channels.", 15, fill=MUTED)

# ---- row 1: schematic ----------------------------------------------------
BOX_Y, BOX_H = 130, 460
add(f'<rect x="60" y="{BOX_Y}" width="300" height="{BOX_H}" rx="14" fill="#F3F6FB" stroke="#3B4A5F" stroke-width="2"/>')
add(f'<rect x="1040" y="{BOX_Y}" width="300" height="{BOX_H}" rx="14" fill="#F3F6FB" stroke="#3B4A5F" stroke-width="2"/>')
text(82, BOX_Y + 36, "XIAO ESP32-C5", 21, "700")
text(82, BOX_Y + 58, "receiver: C5VRX-5 (branch wifi-bridge)", 13, fill=MUTED)
text(1062, BOX_Y + 36, "XIAO ESP32-S3", 21, "700")
text(1062, BOX_Y + 58, "Wi-Fi bridge: camera port + JPEG + AP", 13, fill=MUTED)
antenna(110, BOX_Y, "5.8 GHz antenna (U.FL)", "start")
antenna(1290, BOX_Y, "2.4 GHz antenna (U.FL), fit it!", "end")

rows = [BOX_Y + 100 + i * 42 for i in range(6)]
gnd_y = BOX_Y + 100 + 6 * 42
link_y = gnd_y + 64
mono = "Consolas, 'Cascadia Mono', monospace"

for i, (c5p, c5g, s3p, s3g) in enumerate(VIDEO):
    y, col = rows[i], BIT_COLOURS[i]
    add(f'<line x1="360" y1="{y}" x2="1040" y2="{y}" stroke="{col}" stroke-width="4" stroke-linecap="round"/>')
    add(f'<path d="M952 {y-7}L966 {y}L952 {y+7}Z" fill="{col}"/>')
    text(700, y - 8, f"bit {i}" + (" (LSB)" if i == 0 else " (MSB)" if i == 5 else ""), 13, "600", col, "middle")
    text(346, y + 5, f"{c5p} · {c5g}", 15, "600", INK, "end", mono)
    text(1054, y + 5, f"{s3p} · {s3g}", 15, "600", INK, "start", mono)
    pin_marker(360, y, f"bit{i}")
    pin_marker(1040, y, f"bit{i}")

add(f'<line x1="360" y1="{gnd_y}" x2="1040" y2="{gnd_y}" stroke="{GND}" stroke-width="4"/>')
text(700, gnd_y - 8, "GND: common ground, required", 13, "600", GND, "middle")
text(346, gnd_y + 5, "GND", 15, "600", INK, "end", mono)
text(1054, gnd_y + 5, "GND", 15, "600", INK, "start", mono)
pin_marker(360, gnd_y, "gnd")
pin_marker(1040, gnd_y, "gnd")

add(f'<line x1="360" y1="{link_y}" x2="1040" y2="{link_y}" stroke="{LINK}" stroke-width="3" stroke-dasharray="10 7"/>')
resistor_h(660, 740, link_y, LINK, "1 kΩ")
add(f'<path d="M462 {link_y-7}L448 {link_y}L462 {link_y+7}Z" fill="{LINK}"/>')
text(700, link_y - 16, "optional: phone channel control (S3 sends keys, UART 115200)", 13, "600", LINK, "middle")
text(346, link_y - 3, "GPIO2", 15, "600", INK, "end", mono)
text(346, link_y + 14, "MTMS pad, back", 12, fill=MUTED, anchor="end")
text(1054, link_y - 3, "D8 · GPIO7", 15, "600", INK, "start", mono)
text(1054, link_y + 14, "UART TX", 12, fill=MUTED, anchor="start")
pin_marker(360, link_y, "link")
pin_marker(1040, link_y, "link")

text(700, BOX_Y + BOX_H + 26, "Video flows C5 → S3 (the C5's 6-bit DAC bus, 20 MS/s). "
     "The S3 inputs only listen: goggles on the DAC keep working.", 13, fill=MUTED, anchor="middle")

# ---- row 2: physical pinouts ---------------------------------------------
ROW2 = 700
text(40, ROW2 - 22, "Where the pins are: top view, USB-C at the top", 19, "700")
PITCH = 34


def board(x0, name, sub, left, right, notes):
    bx, by, bw, bh = x0, ROW2 + 50, 170, 280
    text(bx + bw / 2, ROW2 + 8, name, 17, "700", INK, "middle")
    text(bx + bw / 2, ROW2 + 26, sub, 12, fill=MUTED, anchor="middle")
    add(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="12" fill="#2E3A48" stroke="#1D2733" stroke-width="2"/>')
    add(f'<rect x="{bx + bw/2 - 32}" y="{by - 14}" width="64" height="26" rx="8" fill="#B8C2CC" stroke="#6B7785" stroke-width="2"/>')
    text(bx + bw / 2, by + 6, "USB-C", 10, "700", "#2E3A48", "middle")
    add(f'<rect x="{bx + bw/2 - 40}" y="{by + 70}" width="80" height="80" rx="6" fill="#3E4B5B" stroke="#55647A"/>')
    text(bx + bw / 2, by + 115, name.split()[-1], 12, "600", "#C9D2DC", "middle")
    for i, (pin, gpio, kind, note) in enumerate(left):
        y = by + 40 + i * PITCH
        pin_marker(bx, y, kind)
        text(bx + 14, y + 5, pin, 12, "700", "#E9EEF3")
        label = f"{pin}" + (f" ({gpio})" if gpio else "") + f" · {note}"
        text(bx - 14, y + 5, label, 13, "600" if kind != "free" else "normal", kind_colour(kind) if kind != "free" else MUTED, "end")
    for i, (pin, gpio, kind, note) in enumerate(right):
        y = by + 40 + i * PITCH
        pin_marker(bx + bw, y, kind)
        text(bx + bw - 14, y + 5, pin, 12, "700", "#E9EEF3", "end")
        label = f"{pin}" + (f" ({gpio})" if gpio else "") + f" · {note}"
        text(bx + bw + 14, y + 5, label, 13, "600" if kind != "free" else "normal", kind_colour(kind) if kind != "free" else MUTED)
    for j, n in enumerate(notes):
        text(bx + bw / 2, by + bh + 30 + j * 19, n, 13, fill=INK if j == 0 else MUTED, anchor="middle")


board(240, "XIAO ESP32-C5", "receiver", C5_LEFT, C5_RIGHT,
      ["Back side: GPIO2 = MTMS pad ← link wire (1 kΩ)",
       "GPIO3–5 pads carry IQ: leave them free"])
board(990, "XIAO ESP32-S3", "Wi-Fi bridge", S3_LEFT, S3_RIGHT,
      ["Back side: GPIO42 = MTMS pad, sample-clock loop",
       "leave it unconnected"])

# legend
LX, LY = 640, ROW2 + 60
text(LX, LY, "Legend", 15, "700")
legend = [("bit0", "video bit"), ("gnd", "ground"), ("link", "key link (opt.)"),
          ("dnc", "never connect"), ("free", "unused")]
for i, (k, s) in enumerate(legend):
    pin_marker(LX + 8, LY + 26 + i * 28, k)
    text(LX + 26, LY + 31 + i * 28, s, 13)

# ---- row 3: optional DAC and rules ----------------------------------------
ROW3 = 1160
text(40, ROW3 - 20, "Optional: analog video for goggles (C5VRX's resistor DAC on the same pins)", 17, "700")
dac = [("D4", "8.2 kΩ"), ("D5", "3.9 kΩ"), ("D6", "2.0 kΩ"), ("D7", "1.0 kΩ"), ("D8", "470 Ω"), ("D9", "240 Ω")]
BUS_X = 330
for i, (pin, val) in enumerate(dac):
    y = ROW3 + 16 + i * 30
    col = BIT_COLOURS[i]
    pin_marker(70, y, f"bit{i}")
    text(84, y + 5, f"C5 {pin}", 13, "600", INK)
    add(f'<line x1="150" y1="{y}" x2="{BUS_X}" y2="{y}" stroke="{col}" stroke-width="2.5"/>')
    resistor_h(200, 300, y, col, val)
add(f'<line x1="{BUS_X}" y1="{ROW3 + 16}" x2="{BUS_X}" y2="{ROW3 + 196}" stroke="{INK}" stroke-width="2.5"/>')
add(f'<circle cx="{BUS_X}" cy="{ROW3 + 91}" r="4" fill="{INK}"/>')
add(f'<line x1="{BUS_X}" y1="{ROW3 + 91}" x2="560" y2="{ROW3 + 91}" stroke="{INK}" stroke-width="2.5"/>')
add(f'<circle cx="560" cy="{ROW3 + 91}" r="5" fill="#fff" stroke="{INK}" stroke-width="2"/>')
text(572, ROW3 + 87, "CVBS out", 14, "700")
text(572, ROW3 + 105, "goggles / monitor video (tip)", 12, fill=MUTED)
add(f'<rect x="{BUS_X - 9}" y="{ROW3 + 196}" width="18" height="44" rx="2" fill="#fff" stroke="{INK}" stroke-width="2"/>')
text(BUS_X + 16, ROW3 + 223, "200 Ω load", 12, "600", INK)
add(f'<line x1="{BUS_X}" y1="{ROW3 + 240}" x2="{BUS_X}" y2="{ROW3 + 252}" stroke="{INK}" stroke-width="2.5"/>')
add(f'<path d="M{BUS_X-16} {ROW3+252}H{BUS_X+16}M{BUS_X-10} {ROW3+258}H{BUS_X+10}M{BUS_X-4} {ROW3+264}H{BUS_X+4}" stroke="{INK}" stroke-width="2.5"/>')
text(BUS_X + 24, ROW3 + 262, "GND (also to the goggles' ground)", 12, fill=MUTED)

NX = 760
add(f'<rect x="{NX - 20}" y="{ROW3 - 44}" width="{W - NX - 20}" height="300" rx="12" fill="#FFF8F0" stroke="#F0C9A8"/>')
text(NX, ROW3 - 14, "Before you power up", 17, "700")
rules = [
    "Power each board from its own USB-C cable; share only GND.",
    "Never join the two 5V pins while both are on USB.",
    "Keep the video wires short (< 5 cm); extra GND wires help.",
    "Antennas on both boards before power, kept apart.",
    "C5 D0–D3, D10 and the GPIO3–5 pads: never connect.",
    "S3 GPIO42 back pad: never connect (sample clock).",
    "Link wire: only with this repo's receiver build;",
    "  the 1 kΩ stops the S3 powering an unpowered C5.",
    "Self-test (S3 console L) only with D0–D5 disconnected;",
    "  it refuses if it sees the C5 driving them.",
]
for i, r in enumerate(rules):
    cont = r.startswith("  ")   # continuation of the previous bullet: indent under its text
    text(NX + (12 if cont else 0), ROW3 + 16 + i * 23, r.strip() if cont else "•  " + r, 14, fill=INK)

text(W - 30, H - 16, "c5vrx-wifi · tools/make_wiring_svg.py · C5VRX by Twotoz and the C5VRX contributors",
     11, fill=MUTED, anchor="end")

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
       f'font-family="\'Segoe UI\', Arial, sans-serif">\n' + "\n".join(parts) + "\n</svg>\n")
OUT.parent.mkdir(exist_ok=True)
OUT.write_text(svg, encoding="utf-8", newline="\n")
print(f"wrote {OUT}")
