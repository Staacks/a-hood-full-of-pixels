#!/usr/bin/env python3
"""Generate the breadboard wiring diagrams as SVG (no dependencies).

  python3 tools/wiring_diagrams.py        # writes docs/wiring_display.svg, docs/wiring_controller.svg

Conventions: ICs are drawn in their physical DIP pin order (top view, notch up), the
Pico pinout inset shows the physical header (USB up). Pin numbers are printed on the
pin stubs; supply connections are drawn as +5V / 3V3 / GND flags. Wire colours match
between the schematic and the pinout insets.
"""

from pathlib import Path
from xml.sax.saxutils import escape

S = 22          # pin pitch in the schematic
STUB = 22       # pin stub length
FONT = "DejaVu Sans, Arial, Helvetica, sans-serif"

ADDR = "#b8860b"
SPI = "#0e9aa7"
COL = {
    "R1": "#d62728", "R2": "#d62728", "G1": "#2ca02c", "G2": "#2ca02c",
    "B1": "#1f77b4", "B2": "#1f77b4",
    "A": ADDR, "B": ADDR, "C": ADDR, "D": ADDR, "E": ADDR,
    "CLK": "#9467bd", "CLK2": "#c07ad8", "LAT": "#8c564b", "OE": "#e377c2",
    "MISO": SPI, "MOSI": SPI, "SCK": SPI, "CS": SPI,
    "SDA": "#6a3d9a", "SCL": "#6a3d9a", "INT1": "#e377c2",
    "DIN": "#2ca02c", "BTN": "#7f7f7f",
    "+5V": "#b00000", "3V3": "#ff7f0e", "GND": "#000000",
}


def col(sig):
    return COL.get(sig, "#555555")


class Drawing:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.el = []

    def line(self, pts, color="#000", width=2.0, dash=None):
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.el.append(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{width}" '
                       f'stroke-linejoin="round" stroke-linecap="round"{d}/>')

    def rect(self, x, y, w, h, fill="#fff", stroke="#000", width=1.5, rx=4, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.el.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
                       f'fill="{fill}" stroke="{stroke}" stroke-width="{width}"{d}/>')

    def circle(self, x, y, r, fill="#fff", stroke="#000", width=1.2):
        self.el.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}" stroke="{stroke}" '
                       f'stroke-width="{width}"/>')

    def path(self, d, fill="none", stroke="#000", width=1.5):
        self.el.append(f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{width}"/>')

    def text(self, x, y, s, size=12, anchor="start", color="#000", weight="normal", italic=False):
        st = ' font-style="italic"' if italic else ""
        self.el.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{FONT}" font-size="{size}" '
                       f'text-anchor="{anchor}" fill="{color}" font-weight="{weight}"{st}>{escape(s)}</text>')

    def lines(self, x, y, rows, size=12, gap=None, **kw):
        gap = gap or size * 1.35
        for i, r in enumerate(rows):
            self.text(x, y + i * gap, r, size=size, **kw)

    def dot(self, x, y, color="#000"):
        self.circle(x, y, 3.5, fill=color, stroke=color)

    def save(self, path):
        body = "\n".join(self.el)
        Path(path).write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}">\n<rect width="100%" height="100%" fill="#fff"/>\n{body}\n</svg>\n')


# ---------------------------------------------------------------- symbols

def flag(d, x, y, label, direction="left"):
    """Supply flag attached to point (x, y): +5V, 3V3 or GND."""
    c = col(label)
    w, h, lead = 8 + 8 * len(label), 18, 14
    txt = "#fff" if label in ("GND", "+5V") else "#000"
    if direction in ("left", "right"):
        sgn = -1 if direction == "left" else 1
        x2 = x + sgn * lead
        d.line([(x, y), (x2, y)], c, 2)
        bx = x2 - w if direction == "left" else x2
        d.rect(bx, y - h / 2, w, h, fill=c, stroke=c, rx=3)
        d.text(bx + w / 2, y + 4.5, label, size=12, anchor="middle", color=txt, weight="bold")
    else:
        sgn = 1 if direction == "down" else -1
        y2 = y + sgn * lead
        d.line([(x, y), (x, y2)], c, 2)
        by = y2 if direction == "down" else y2 - h
        d.rect(x - w / 2, by, w, h, fill=c, stroke=c, rx=3)
        d.text(x, by + 13.5, label, size=12, anchor="middle", color=txt, weight="bold")


def resistor(d, x1, y1, x2, y2, label, color="#000"):
    """Zigzag resistor on a horizontal or vertical segment."""
    vertical = x1 == x2
    length = (y2 - y1) if vertical else (x2 - x1)
    lead = (abs(length) - 30) / 2 * (1 if length > 0 else -1)
    pts = [(x1, y1)]
    seg = (length - 2 * lead) / 6
    for i in range(7):
        t = lead + i * seg
        off = 0 if i in (0, 6) else (6 if i % 2 else -6)
        pts.append((x1 + off, y1 + t) if vertical else (x1 + t, y1 + off))
    pts.append((x2, y2))
    d.line(pts, color, 1.8)
    if vertical:
        d.text(x1 + 10, (y1 + y2) / 2 + 4, label, size=11)
    else:
        d.text((x1 + x2) / 2, y1 - 10, label, size=11, anchor="middle")


def capacitor_v(d, x, y1, y2, label):
    """Capacitor between a +5V flag (top) and a GND flag (bottom)."""
    m = (y1 + y2) / 2
    d.line([(x, y1), (x, m - 4)], col("+5V"), 2)
    d.line([(x - 10, m - 4), (x + 10, m - 4)], "#000", 2.5)
    d.line([(x - 10, m + 4), (x + 10, m + 4)], "#000", 2.5)
    d.line([(x, m + 4), (x, y2)], "#000", 2)
    d.text(x + 14, m + 4, label, size=11)
    flag(d, x, y1, "+5V", "up")
    flag(d, x, y2, "GND", "down")


def decoupling_cap(d, x, y, supply, name, value, where, polarized=False):
    """Capacitor from a supply flag (top) to a GND flag (bottom); y = top connection point."""
    m = y + 24
    flag(d, x, y, supply, "up")
    d.line([(x, y), (x, m - 4)], col(supply), 2)
    d.line([(x - 10, m - 4), (x + 10, m - 4)], "#000", 2.5)
    if polarized:
        d.path(f"M {x - 10} {m + 7} Q {x} {m + 1} {x + 10} {m + 7}", stroke="#000", width=2.5)
        d.text(x - 15, m - 7, "+", size=12, anchor="middle", weight="bold")
    else:
        d.line([(x - 10, m + 4), (x + 10, m + 4)], "#000", 2.5)
    d.line([(x, m + (6 if polarized else 4)), (x, m + 28)], "#000", 2)
    flag(d, x, m + 28, "GND", "down")
    d.lines(x + 16, m - 8, [name, value, where], size=11, gap=14)


def decoupling(d, x, y, caps, columns, dx=125, dy=150):
    """Grid of decoupling capacitors. caps: (supply, name, value, where, polarized)."""
    d.text(x - 20, y, "Decoupling capacitors", size=14, weight="bold")
    d.text(x - 20, y + 16, "short leads, right at the part", size=11, color="#555")
    for i, (supply, name, value, where, pol) in enumerate(caps):
        decoupling_cap(d, x + (i % columns) * dx, y + 72 + (i // columns) * dy, supply, name, value, where, pol)


def diode(d, xa, xk, y, label):
    """Diode on a horizontal line, anode at xa (left), cathode (stripe) at xk (right)."""
    m = (xa + xk) / 2
    d.line([(xa, y), (m - 8, y)], col("+5V"), 2)
    d.path(f"M {m - 8} {y - 8} L {m + 6} {y} L {m - 8} {y + 8} Z", fill="#000", stroke="#000", width=1)
    d.line([(m + 7, y - 8), (m + 7, y + 8)], "#000", 2.5)
    d.line([(m + 7, y), (xk, y)], "#000", 2)
    d.text(m, y + 22, label, size=10.5, anchor="middle", weight="bold")


def button(d, x1, x2, y, color):
    """Push button between (x1, y) and (x2, y)."""
    a, b = x1 + 12, x2 - 12
    d.line([(x1, y), (a, y)], color, 2)
    d.line([(b, y), (x2, y)], "#000", 2)
    d.circle(a, y, 3)
    d.circle(b, y, 3)
    m = (a + b) / 2
    d.line([(a - 2, y - 9), (b + 2, y - 9)], "#000", 2)
    d.line([(m, y - 9), (m, y - 16)], "#000", 2)
    d.line([(m - 6, y - 16), (m + 6, y - 16)], "#000", 2)


def nc(d, x, y):
    d.text(x, y + 4, "nc", size=10, color="#888", italic=True)


# ---------------------------------------------------------------- blocks

def block(d, x, y_top, y_bot, w, title, left=(), right=(), subtitle=None, fill="#f7f7f2", notch=False):
    """Rectangle with pins. left/right: lists of (y, number, name). Returns {name: (x, y)} of stub ends."""
    d.rect(x, y_top, w, y_bot - y_top, fill=fill, stroke="#333", width=1.8)
    if notch:
        cx = x + w / 2
        d.path(f"M {cx - 10} {y_top} A 10 10 0 0 0 {cx + 10} {y_top}", fill="#fff", stroke="#333", width=1.5)
    ty = y_top - 10
    if subtitle:
        d.text(x + w / 2, ty, subtitle, size=11, anchor="middle", color="#555")
        ty -= 16
    d.text(x + w / 2, ty, title, size=14, anchor="middle", weight="bold")
    pins = {}
    for y, num, name in left:
        d.line([(x - STUB, y), (x, y)], "#333", 1.5)
        d.text(x - 4, y - 4, str(num), size=10, anchor="end", color="#666")
        d.text(x + 6, y + 4, name, size=12)
        pins[name] = (x - STUB, y)
    for y, num, name in right:
        d.line([(x + w, y), (x + w + STUB, y)], "#333", 1.5)
        d.text(x + w + 4, y - 4, str(num), size=10, color="#666")
        d.text(x + w - 6, y + 4, name, size=12, anchor="end")
        pins[name] = (x + w + STUB, y)
    return pins


def hct245(d, x, y0, title):
    """74HCT245 in physical DIP-20 order, top view. Row k (0..9) at y0 + k*S."""
    w = 120
    left = [(y0, 1, "DIR")] + [(y0 + k * S, k + 1, f"A{k}") for k in range(1, 9)] + [(y0 + 9 * S, 10, "GND")]
    right = [(y0, 20, "VCC"), (y0 + S, 19, "/OE")] + [(y0 + k * S, 20 - k, f"B{k - 1}") for k in range(2, 10)]
    pins = block(d, x, y0 - 20, y0 + 9 * S + 20, w, title, left, right, subtitle="74HCT245, top view", notch=True)
    pins["DIR"], pins["GND_L"] = pins["DIR"], pins["GND"]
    return pins


def ahct125(d, x, y0, title):
    """74AHCT125 in physical DIP-14 order, top view. Row k (0..6) at y0 + k*S."""
    w = 120
    left = [(y0 + k * S, k + 1, n) for k, n in enumerate(["/1OE", "1A", "1Y", "/2OE", "2A", "2Y", "GND"])]
    right = [(y0 + k * S, 14 - k, n) for k, n in enumerate(["VCC", "/4OE", "4A", "4Y", "/3OE", "3A", "3Y"])]
    return block(d, x, y0 - 20, y0 + 6 * S + 20, w, title, left, right, subtitle="74AHCT125, top view", notch=True)


def wire(d, a, b, sig, label_x=None, label=None):
    """Straight (or L-shaped) wire between two points, with a signal label."""
    c = col(sig)
    if a[1] == b[1] or a[0] == b[0]:
        d.line([a, b], c, 2.2)
    else:
        mx = (a[0] + b[0]) / 2
        d.line([a, (mx, a[1]), (mx, b[1]), b], c, 2.2)
    if label_x is not None:
        d.text(label_x, a[1] - 4, label or sig, size=10.5, anchor="middle", color=c, weight="bold")


# ---------------------------------------------------------------- insets

PICO_PINS = {
    1: "GP0", 2: "GP1", 3: "GND", 4: "GP2", 5: "GP3", 6: "GP4", 7: "GP5", 8: "GND", 9: "GP6", 10: "GP7",
    11: "GP8", 12: "GP9", 13: "GND", 14: "GP10", 15: "GP11", 16: "GP12", 17: "GP13", 18: "GND",
    19: "GP14", 20: "GP15", 21: "GP16", 22: "GP17", 23: "GND", 24: "GP18", 25: "GP19", 26: "GP20",
    27: "GP21", 28: "GND", 29: "GP22", 30: "RUN", 31: "GP26", 32: "GP27", 33: "AGND", 34: "GP28",
    35: "ADC_VREF", 36: "3V3(OUT)", 37: "3V3_EN", 38: "GND", 39: "VSYS", 40: "VBUS",
}


def pico_pinout(d, cx, y0, used, title):
    """Physical Pico 2 W header, top view, USB up. used: {pin: (function, signal_for_colour)}."""
    pitch, bw = 22, 104
    x0 = cx - bw / 2
    d.text(cx, y0 - 44, title, size=14, anchor="middle", weight="bold")
    d.text(cx, y0 - 28, "top view, USB connector up", size=11, anchor="middle", color="#555")
    d.rect(x0, y0 - 14, bw, 20 * pitch + 28, fill="#e9f2e4", stroke="#2b6b2b", width=2, rx=6)
    d.rect(cx - 16, y0 - 22, 32, 16, fill="#bbb", stroke="#555", rx=2)
    d.text(cx, y0 + 20 * pitch + 6, "Pico 2 W", size=12, anchor="middle", color="#2b6b2b", weight="bold")
    for i in range(20):
        for pin, side in ((i + 1, "L"), (40 - i, "R")):
            y = y0 + i * pitch
            x = x0 + 12 if side == "L" else x0 + bw - 12
            name = PICO_PINS[pin]
            if pin in used:
                fn, sig = used[pin]
                c = col(sig)
                label, weight = f"{fn}  {name}" if side == "R" else f"{name}  {fn}", "bold"
            elif "GND" in name:
                c, label, weight = "#000", name, "normal"
            else:
                c, label, weight = "#bbb", name, "normal"
            d.rect(x - 6, y - 6, 12, 12, fill=c, stroke="#333", width=1, rx=2)
            d.text(x + (14 if side == "L" else -14), y + 4, str(pin), size=9,
                   anchor="start" if side == "L" else "end", color="#2b6b2b")
            tc = c if pin in used else ("#000" if "GND" in name else "#999")
            if side == "L":
                d.text(x0 - 8, y + 4, f"{fn}  {name}" if pin in used else label, size=11, anchor="end",
                       color=tc, weight=weight)
            else:
                d.text(x0 + bw + 8, y + 4, f"{name}  {fn}" if pin in used else label, size=11,
                       color=tc, weight=weight)


def hub75_socket(d, x, y0, column_labels=None):
    """HUB75E IN socket pinout (2 x 8). column_labels: what feeds the odd / even row."""
    pins = [("R1", 1), ("G1", 2), ("B1", 3), ("GND", 4), ("R2", 5), ("G2", 6), ("B2", 7), ("E", 8),
            ("A", 9), ("B", 10), ("C", 11), ("D", 12), ("CLK", 13), ("LAT", 14), ("OE", 15), ("GND", 16)]
    pitch = 24
    d.text(x + 18, y0 - 34, "HUB75E socket (IN)", size=14, anchor="middle", weight="bold")
    d.text(x + 18, y0 - 18, "pin 1 is marked on the socket", size=11, anchor="middle", color="#555")
    d.rect(x - 14, y0 - 12, 64, 8 * pitch, fill="#333", stroke="#000", rx=4)
    d.rect(x - 20, y0 + 3 * pitch, 6, 2 * pitch - 12, fill="#333", stroke="#000", rx=1)  # key notch
    for i, (name, num) in enumerate(pins):
        row, colm = i // 2, i % 2
        px, py = x + colm * 36, y0 + row * pitch
        c = col(name)
        d.circle(px, py, 7, fill=c, stroke="#fff", width=1.5)
        d.text(px, py + 3.5, str(num), size=8, anchor="middle", color="#fff", weight="bold")
        if colm == 0:
            d.text(x - 28, py + 4, name, size=12, anchor="end", color=c, weight="bold")
        else:
            d.text(x + 36 + 22, py + 4, name, size=12, color=c, weight="bold")
    if column_labels:
        for colm, label in enumerate(column_labels):
            d.text(x + colm * 36, y0 + 8 * pitch + 6, label, size=12, anchor="middle", weight="bold")
        d.text(x + 18, y0 + 8 * pitch + 22, "odd row / even row", size=10.5, anchor="middle", color="#555")


def placement(d, sx, y0, u1_map, u2_map):
    """Breadboard placement, top view: U1 (notch up) | HUB75 socket | U2 (rotated, notch down).
    sx = x of the socket's odd column, y0 = y of socket row 0 (pins 1 and 2)."""
    P = 24
    d.text(sx + P / 2, y0 - 92, "Breadboard placement (top view)", size=14, anchor="middle", weight="bold")
    d.text(sx + P / 2, y0 - 76, "U2 rotated 180° (notch down), placed two rows lower than U1",
           size=11, anchor="middle", color="#555")

    def chip(x, rows, notch_top, left_pins, right_pins, title):
        """DIP body spanning the given row range; *_pins: {row: (pin, name, gpio_label, colour)}."""
        top, bot = y0 + rows[0] * P - P / 2, y0 + rows[1] * P + P / 2
        d.rect(x, top, 56, bot - top, fill="#333", stroke="#000", rx=3)
        ny = top if notch_top else bot
        sweep = 0 if notch_top else 1
        d.path(f"M {x + 18} {ny} A 10 10 0 0 {sweep} {x + 38} {ny}", fill="#fff", stroke="#000", width=1.2)
        d.text(x + 28, (top + bot) / 2 + 5, title, size=13, anchor="middle", color="#fff", weight="bold")
        for side, pins in (("L", left_pins), ("R", right_pins)):
            for r, (pin, name, label, c) in pins.items():
                y = y0 + r * P
                px = x - 4 if side == "L" else x + 60
                d.rect(px - 4, y - 4, 8, 8, fill="#ccc", stroke="#555", width=1, rx=1)
                inner = x + 4 if side == "L" else x + 52
                d.text(inner, y + 3.5, str(pin), size=8.5, anchor="start" if side == "L" else "end", color="#ddd")
                if label:
                    d.text(px - 10 if side == "L" else px + 10, y + 4, label, size=10.5,
                           anchor="end" if side == "L" else "start", color=c, weight="bold")

    # socket
    d.rect(sx - 14, y0 - 14, P + 28, 8 * P + 4, fill="#333", stroke="#000", rx=3)
    names = {1: "R1", 2: "G1", 3: "B1", 4: "GND", 5: "R2", 6: "G2", 7: "B2", 8: "E", 9: "A", 10: "B",
             11: "C", 12: "D", 13: "CLK", 14: "LAT", 15: "OE", 16: "GND"}
    for r in range(8):
        for c, pin in ((0, 2 * r + 1), (1, 2 * r + 2)):
            d.circle(sx + c * P, y0 + r * P, 7, fill=col(names[pin]), stroke="#fff", width=1.2)
            d.text(sx + c * P, y0 + r * P + 3, str(pin), size=7.5, anchor="middle", color="#fff", weight="bold")
    d.text(sx + P / 2, y0 + 8 * P + 6, "HUB75E", size=11, anchor="middle", weight="bold")

    # U1: notch up, rows -2..7; B_k (pin 19-k) at row k-1 -> odd pin 2k-1
    u1x = sx - 14 - 60 - 56
    u1_left, u1_right = {}, {}
    for k in range(1, 9):
        m = u1_map[k - 1]
        u1_left[k - 2] = (k + 1, f"A{k}", f"GP{m[0]}" if m else "GND", col(m[1]) if m else "#000")
        u1_right[k - 1] = (19 - k, f"B{k}", "", "#000")
        if m:
            d.line([(u1x + 64, y0 + (k - 1) * P), (sx - 7, y0 + (k - 1) * P)], col(m[1]), 2.4)
    u1_left[-2] = (1, "DIR", "+5V", col("+5V"))
    u1_left[7] = (10, "GND", "GND", "#000")
    u1_right[-2] = (20, "VCC", "", "#000")
    u1_right[-1] = (19, "/OE", "", "#000")
    chip(u1x, (-2, 7), True, u1_left, u1_right, "U1")
    d.text(u1x + 64 + 8, y0 - 2 * P + 4, "+5V", size=10.5, color=col("+5V"), weight="bold")
    d.text(u1x + 64 + 8, y0 - P + 4, "GND", size=10.5, weight="bold")

    # U2: rotated 180°, rows 0..9; B_k (pin 19-k) at row 8-k -> even pin 18-2k; A_k (pin k+1) at row 9-k
    u2x = sx + P + 14 + 60
    u2_left, u2_right = {}, {}
    for k in range(1, 9):
        m = u2_map[k - 1]
        u2_left[8 - k] = (19 - k, f"B{k}", "", "#000")
        u2_right[9 - k] = (k + 1, f"A{k}", f"GP{m[0]}" if m else "GND", col(m[1]) if m else "#000")
        if m and m[2]:
            d.line([(sx + P + 7, y0 + (8 - k) * P), (u2x - 8, y0 + (8 - k) * P)], col(m[1]), 2.4)
    u2_left[8] = (19, "/OE", "", "#000")
    u2_left[9] = (20, "VCC", "", "#000")
    u2_right[0] = (10, "GND", "GND", "#000")
    u2_right[9] = (1, "DIR", "+5V", col("+5V"))
    chip(u2x, (0, 9), False, u2_left, u2_right, "U2")
    d.text(u2x - 12, y0 + 8 * P + 4, "GND", size=10.5, anchor="end", weight="bold")
    d.text(u2x - 12, y0 + 9 * P + 4, "+5V", size=10.5, anchor="end", color=col("+5V"), weight="bold")
    d.text(sx + P / 2, y0 + 9 * P + 22, "socket pins 4 and 16: wire to GND", size=10.5, anchor="middle",
           color="#555", italic=True)
    d.text(sx + P / 2, y0 + 9 * P + 38, "U2 B1 (CLK2) goes to panel 2's socket, pin 13", size=10.5,
           anchor="middle", color=col("CLK2"), italic=True)


def power_section(d, x, y, loads, note_lines):
    """Power bank -> USB-C meter -> 5 V breakout -> +5V / GND to the loads."""
    d.text(x, y - 18, "Power (load side)", size=14, weight="bold")
    boxes = [("USB-C power bank", "5 V / 3 A"), ("USB-C meter", "measure here"), ("USB-C 5 V breakout", "or cut cable")]
    bx = x
    for i, (t, s) in enumerate(boxes):
        d.rect(bx, y, 150, 46, fill="#fff8e6", stroke="#a67c00")
        d.text(bx + 75, y + 19, t, size=12, anchor="middle", weight="bold")
        d.text(bx + 75, y + 36, s, size=11, anchor="middle", color="#555")
        if i < 2:
            d.line([(bx + 150, y + 23), (bx + 190, y + 23)], "#555", 3)
        bx += 190
    sx = x + 2 * 190 + 150
    v5, gnd = y + 12, y + 34
    end = sx + 70 + 160 * len(loads)
    d.line([(sx, v5), (end, v5)], col("+5V"), 3)
    d.line([(sx, gnd), (end, gnd)], col("GND"), 3)
    d.text(sx + 8, v5 - 6, "+5V", size=11, color=col("+5V"), weight="bold")
    d.text(sx + 8, gnd + 16, "GND", size=11, weight="bold")
    lx = sx + 70
    for name, sub in loads:
        d.dot(lx + 20, v5, col("+5V"))
        d.dot(lx + 40, gnd, col("GND"))
        d.line([(lx + 20, v5), (lx + 20, y + 80)], col("+5V"), 3)
        d.line([(lx + 40, gnd), (lx + 40, y + 80)], col("GND"), 3)
        d.rect(lx, y + 80, 140, 44, fill="#fff", stroke="#333")
        d.text(lx + 70, y + 98, name, size=12, anchor="middle", weight="bold")
        d.text(lx + 70, y + 115, sub, size=11, anchor="middle", color="#555")
        lx += 160
    d.lines(x, y + 80, note_lines, size=12)


def legend(d, x, y, items):
    d.text(x, y, "Wire colours", size=13, weight="bold")
    for i, (sig, label) in enumerate(items):
        yy = y + 20 + i * 19
        d.line([(x, yy - 4), (x + 30, yy - 4)], col(sig), 4)
        d.text(x + 38, yy, label, size=11.5)


# ---------------------------------------------------------------- sheets

def display_sheet(path):
    d = Drawing(1860, 1190)
    d.text(30, 40, "Display Pico (mask) — breadboard wiring", size=22, weight="bold")
    d.text(30, 62, "Pico → 2 × 74HCT245 (3.3 V → 5 V) → HUB75E inputs of both panels (in parallel, separate "
                   "CLK); microSD direct at 3.3 V. Pin numbers = physical pins.", size=13, color="#444")

    Y1 = 150
    Y2 = Y1 + 10 * S + 70
    Y3 = Y2 + 10 * S + 70
    px, pw = 100, 180
    cx = 470

    # 245 channel k (A_k -> B_k) feeds one row of the HUB75 socket 1:1. On the breadboard U1
    # sits left of the socket (notch up), U2 right of it rotated by 180° (notch down, two rows
    # lower), so:
    # U1 B1..B8 -> odd pins 1..15, U2 B1..B8 -> even pins 16..2 (reversed by the rotation;
    # channels B1 and B7 sit at the GND pins 16 and 4: B7 is unused, B1 carries CLK2 to
    # panel 2's socket).
    # (GPIO, signal, socket pin) per channel; None = unused channel, socket None = panel 2 only.
    u1_map = [(0, "R1", 1), (2, "B1", 3), (3, "R2", 5), (5, "B2", 7),
              (6, "A", 9), (8, "C", 11), (11, "CLK", 13), (14, "OE", 15)]
    u2_map = [(12, "CLK2", None), (13, "LAT", 14), (9, "D", 12), (7, "B", 10),
              (10, "E", 8), (4, "G2", 6), None, (1, "G1", 2)]
    sd = [("MISO", 16, 21), ("CS", 17, 22), ("SCK", 18, 24), ("MOSI", 19, 25)]
    pico_phys = {0: 1, 1: 2, 2: 4, 3: 5, 4: 6, 5: 7, 6: 9, 7: 10, 8: 11, 9: 12, 10: 14, 11: 15, 12: 16, 13: 17,
                 14: 19}

    right = [(Y1 + (k + 1) * S, pico_phys[m[0]], f"GP{m[0]}") for k, m in enumerate(u1_map)]
    right += [(Y2 + (k + 1) * S, pico_phys[m[0]], f"GP{m[0]}") for k, m in enumerate(u2_map) if m]
    right += [(Y3 + k * S, phys, f"GP{gp}") for k, (_, gp, phys) in enumerate(sd)]
    left = [(Y1 + S, 36, "3V3(OUT)"), (Y1 + 3 * S, 38, "GND"), (Y1 + 6 * S, 39, "VSYS")]
    pico = block(d, px, Y1 - 30, Y3 + 3 * S + 30, pw, "Raspberry Pi Pico 2 W", left, right,
                 subtitle="display Pico", fill="#e9f2e4")
    flag(d, *pico["3V3(OUT)"], "3V3", "left")
    flag(d, *pico["GND"], "GND", "left")
    vx, vy = pico["VSYS"]
    diode(d, vx - 46, vx, vy, "D1 1N5819")
    flag(d, vx - 46, vy, "+5V", "up")
    d.lines(px + 10, Y1 + 10 * S, ["USB → PC: serial console,", "can stay plugged in",
                                 "(D1 and the Pico's own", "diode keep the two", "supplies apart)"],
            size=11, color="#555")
    d.lines(px + 10, Y2 + 10 * S + 10, ["GPIOs are listed in the", "order of the 245 inputs.",
                                        "Physical positions:", "see the pinout on the right."],
            size=11, color="#555", italic=True)

    u1 = hct245(d, cx, Y1, "U1")
    u2 = hct245(d, cx, Y2, "U2")
    for u in (u1, u2):
        flag(d, *u["DIR"], "+5V", "left")
        flag(d, *u["GND"], "GND", "down")
        flag(d, *u["VCC"], "+5V", "right")
        flag(d, *u["/OE"], "GND", "right")

    # Pico -> 245 inputs; unused inputs to GND
    for u, mapping in ((u1, u1_map), (u2, u2_map)):
        for k, m in enumerate(mapping):
            if m:
                wire(d, pico[f"GP{m[0]}"], u[f"A{k + 1}"], m[1], label_x=355)
            else:
                flag(d, *u[f"A{k + 1}"], "GND", "left")

    # OE pull-up
    oy = pico["GP14"][1]
    rx = 395
    d.dot(rx, oy, col("OE"))
    resistor(d, rx, oy, rx, oy + 52, "10k")
    flag(d, rx, oy + 52, "3V3", "down")
    d.lines(rx - 22, oy + 66, ["keeps panels", "dark at boot"], size=10.5, anchor="end", color="#555", italic=True)

    # HUB75 panel connector: odd row from U1, even row from U2
    hx, hw = 830, 160
    hub_left = [(u1[f"B{k + 1}"][1], m[2], m[1]) for k, m in enumerate(u1_map)]
    gnd_rows = []
    for k, m in enumerate(u2_map):
        y = u2[f"B{k + 1}"][1]
        if m and m[2]:
            hub_left.append((y, m[2], m[1]))
        else:
            name = "GND" + " " * (len(gnd_rows) + 1)  # unique pin keys
            hub_left.append((y, 16 - 2 * k, name))
            gnd_rows.append((k, name))
    hub = block(d, hx, Y1 + S, Y2 + 10 * S, hw, "Panel 1 (top) — HUB75E IN", hub_left, [],
                subtitle="U1 → odd row, U2 (rotated) → even row", fill="#eef3fb")
    d.text(hx + hw - 8, u1["B1"][1] + 4, "odd", size=10.5, anchor="end", color="#555", italic=True)
    d.text(hx + hw - 8, u2["B1"][1] + 4, "even", size=10.5, anchor="end", color="#555", italic=True)
    d.line([(hx, (u1["B8"][1] + u2["B1"][1]) / 2), (hx + hw, (u1["B8"][1] + u2["B1"][1]) / 2)], "#999", 1, dash="4 3")
    for u, mapping in ((u1, u1_map), (u2, u2_map)):
        for k, m in enumerate(mapping):
            if m and m[2]:
                wire(d, u[f"B{k + 1}"], hub[m[1]], m[1], label_x=720)
    for k, name in gnd_rows:
        flag(d, *hub[name], "GND", "left")
        if not u2_map[k]:
            nc(d, u2[f"B{k + 1}"][0] + 4, u2[f"B{k + 1}"][1])

    # panel 2: same socket, all pins in parallel to panel 1 except pin 13 (CLK2 from U2 B1)
    p2y = Y2 + 10 * S + 60
    hub2 = block(d, hx, p2y - 30, p2y + 60, hw, "Panel 2 (bottom) — HUB75E IN", [(p2y, 13, "CLK2")], [],
                 fill="#eef3fb")
    bx, by = u2["B1"]
    rx = 760
    d.line([(bx, by), (rx, by), (rx, p2y), hub2["CLK2"]], col("CLK2"), 2.2)
    d.text(720, by - 4, "CLK2", size=10.5, anchor="middle", color=col("CLK2"), weight="bold")
    d.lines(hx + 10, p2y + 24, ["all other pins: wired 1:1", "to panel 1's socket"], size=11, color="#333")
    d.lines(hx + hw + 14, p2y - 8, ["The panels have no OUT", "connector: both sockets", "share every line except", "CLK (pin 13)."],
            size=11, color="#555", italic=True)

    # microSD
    sdb = block(d, cx, Y3 - 22, Y3 + 3 * S + 22, 150, "microSD breakout",
                [(Y3 + k * S, "", n) for k, (n, _, _) in enumerate(sd)],
                [(Y3, "", "VCC"), (Y3 + S, "", "GND")], subtitle="3.3 V type, SPI", fill="#eef7f7")
    for n, gp, _ in sd:
        wire(d, pico[f"GP{gp}"], sdb[n], n, label_x=355)
    flag(d, *sdb["VCC"], "3V3", "right")
    flag(d, *sdb["GND"], "GND", "right")
    d.lines(cx + 175, Y3 + 2 * S + 4, ["5 V modules with regulator +", "level shifter: VCC to +5V instead"],
            size=10.5, color="#555")

    # insets
    hub75_socket(d, 1110, 150, ("U1", "U2"))
    used = {pico_phys[m[0]]: (m[1], m[1]) for m in u1_map + u2_map if m}
    used.update({phys: (n, n) for n, _, phys in sd})
    used.update({36: ("→ 3V3 flag", "3V3"), 39: ("← D1 ← +5V", "+5V")})
    pico_pinout(d, 1480, 150, used, "Pico 2 W pinout (display)")

    placement(d, 1330, 775, u1_map, u2_map)
    decoupling(d, 1600, 650, [("+5V", "C1", "100 nF", "U1 pin 20–10", False),
                              ("+5V", "C2", "100 nF", "U2 pin 20–10", False),
                              ("3V3", "C3", "10 µF", "microSD VCC", True),
                              ("+5V", "C4", "2200 µF ≥10 V", "panel power split", True)], columns=2)
    legend(d, 1050, 420, [("R1", "red data R1/R2"), ("G1", "green data G1/G2"), ("B1", "blue data B1/B2"),
                          ("A", "row address A–E"), ("CLK", "CLK (panel 1)"), ("CLK2", "CLK2 (panel 2)"),
                          ("LAT", "LAT (strobe)"), ("OE", "OE"),
                          ("MISO", "SD card SPI"), ("+5V", "+5V (power bank)"), ("3V3", "3V3 (Pico 3V3(OUT))"),
                          ("GND", "GND (all connected)")])

    power_section(d, 30, Y3 + 4 * S + 90,
                  [("Panel 1 power", "C4 2200 µF at the split"), ("Panel 2 power", "VCC / GND terminals"),
                   ("+5V / GND flags", "U1, U2, D1 → VSYS")],
                  ["• All GND flags are one net: power bank GND, panels, 245s, Pico GND.",
                   "• Pico powered from +5V only through D1 (1N5819, stripe towards VSYS), never directly.",
                   "• Decoupling capacitors C1–C4: see the right-hand side. C4 buffers the",
                   "\u00a0\u00a0\u00a0panels' pulsed current where +5V splits to panel 1 and panel 2.",
                   "• Start at low brightness; full white of both panels exceeds 3 A."])
    d.save(path)


def controller_sheet(path):
    d = Drawing(1700, 1290)
    d.text(30, 40, "Controller Pico (bottom of the staff) — breadboard wiring", size=22, weight="bold")
    d.text(30, 62, "Pico → 74AHCT125 → WS2812 strip; encoder and buttons to GND (internal pull-ups); LIS3DH on I2C0; "
                   "microSD direct at 3.3 V. Pin numbers = physical pins.", size=13, color="#444")

    Y1 = 150
    YB = Y1 + 10 * S + 80           # buttons
    YS = YB + 5 * 34 + 100          # SD
    YL = YS + 4 * S + 70            # LIS3DH
    px, pw = 100, 180
    cx = 470

    buttons = [(2, 4, "Encoder A"), (3, 5, "Encoder B"), (4, 6, "Encoder push"), (5, 7, "Button 1"), (6, 9, "Button 2 / pairing")]
    sd = [("MISO", 16, 21), ("CS", 17, 22), ("SCK", 18, 24), ("MOSI", 19, 25)]
    lis = [("SDA", 20, 26), ("SCL", 21, 27), ("INT", 22, 29)]

    right = [(Y1 + S, 1, "GP0")]
    right += [(YB + k * 34, phys, f"GP{gp}") for k, (gp, phys, _) in enumerate(buttons)]
    right += [(YS + k * S, phys, f"GP{gp}") for k, (_, gp, phys) in enumerate(sd)]
    right += [(YL + k * S, phys, f"GP{gp}") for k, (_, gp, phys) in enumerate(lis)]
    left = [(Y1 + S, 36, "3V3(OUT)"), (Y1 + 3 * S, 38, "GND"), (Y1 + 6 * S, 39, "VSYS")]
    pico = block(d, px, Y1 - 30, YL + 2 * S + 30, pw, "Raspberry Pi Pico 2 W", left, right,
                 subtitle="controller Pico", fill="#e9f2e4")
    flag(d, *pico["3V3(OUT)"], "3V3", "left")
    flag(d, *pico["GND"], "GND", "left")
    vx, vy = pico["VSYS"]
    diode(d, vx - 46, vx, vy, "D1 1N5819")
    flag(d, vx - 46, vy, "+5V", "up")
    d.lines(px + 10, Y1 + 10 * S, ["USB → PC: serial console,", "can stay plugged in",
                                 "(D1 and the Pico's own", "diode keep the two", "supplies apart)"],
            size=11, color="#555")

    # 74AHCT125 level shifter for the strip: channel 1 used, channels 2-4 disabled
    u1 = ahct125(d, cx, Y1, "U1")
    flag(d, *u1["/1OE"], "GND", "left")
    flag(d, *u1["/2OE"], "+5V", "left")
    flag(d, *u1["2A"], "GND", "left")
    nc(d, u1["2Y"][0] - 18, u1["2Y"][1])
    flag(d, *u1["GND"], "GND", "left")
    flag(d, *u1["VCC"], "+5V", "right")
    flag(d, *u1["/4OE"], "+5V", "right")
    flag(d, *u1["4A"], "GND", "right")
    nc(d, u1["4Y"][0] + 4, u1["4Y"][1])
    flag(d, *u1["/3OE"], "+5V", "right")
    flag(d, *u1["3A"], "GND", "right")
    nc(d, u1["3Y"][0] + 4, u1["3Y"][1])
    wire(d, pico["GP0"], u1["1A"], "DIN", label_x=355, label="strip data")

    # strip; the 5 V data leaves 1Y (left side) and runs under the chip
    sx, sw = 1000, 230
    by = Y1 + 6 * S + 50
    rx_route = cx - 90
    d.line([u1["1Y"], (rx_route, u1["1Y"][1]), (rx_route, by), (cx + 210, by)], col("DIN"), 2.2)
    d.text(cx + 60, by - 6, "5 V data from 1Y (pin 3)", size=10.5, anchor="middle", color=col("DIN"), weight="bold")
    d.text(cx + 60, by + 18, "channels 2–4 unused: /OE → +5V, A → GND", size=10.5, anchor="middle",
           color="#555", italic=True)
    strip = block(d, sx, by - 30, by + 7 * S + 20, sw, "WS2812 strip, 2 × N LEDs",
                  [(by, "", "DIN (LED 0)"), (by + 2 * S, "", "+5V start"), (by + 3 * S, "", "GND start"),
                   (by + 5 * S, "", "+5V end"), (by + 6 * S, "", "GND end")], [],
                  subtitle="folded in the tube, both ends at the bottom", fill="#f3eefb")
    rx1, rx2 = 680, 760
    d.line([(cx + 210, by), (rx1, by)], col("DIN"), 2.2)
    resistor(d, rx1, by, rx2, by, "330 Ω", col("DIN"))
    d.line([(rx2, by), strip["DIN (LED 0)"]], col("DIN"), 2.2)
    d.text(900, by - 5, "DIN", size=10.5, anchor="middle", color=col("DIN"), weight="bold")
    pdx = 840
    d.dot(pdx, by, col("DIN"))
    resistor(d, pdx, by, pdx, by + 56, "10k")
    flag(d, pdx, by + 56, "GND", "down")
    for name, f in (("+5V start", "+5V"), ("GND start", "GND"), ("+5V end", "+5V"), ("GND end", "GND")):
        flag(d, *strip[name], f, "left")
    capacitor_v(d, sx + sw + 60, by + 2 * S, by + 5 * S, "C3 1000 µF")
    d.text(sx + sw + 60, by + 7 * S + 24, "at the strip's", size=10.5, anchor="middle", color="#555")
    d.text(sx + sw + 60, by + 7 * S + 38, "+5V start", size=10.5, anchor="middle", color="#555")
    d.lines(sx, by + 7 * S + 46, ["Short piece of strip is fine for the first test.",
                                  "Feed +5V/GND at both ends in the staff."], size=11.5, color="#333")

    # buttons
    bgx = 560
    for k, (gp, phys, label) in enumerate(buttons):
        y = YB + k * 34
        a = pico[f"GP{gp}"]
        d.line([a, (420, y)], col("BTN"), 2.2)
        button(d, 420, 500, y, col("BTN"))
        d.line([(500, y), (bgx, y)], "#000", 2)
        d.dot(bgx, y)
        d.text(355, y - 5, label, size=10.5, anchor="middle", color="#555", weight="bold")
    d.line([(bgx, YB), (bgx, YB + 4 * 34 + 14)], "#000", 2)
    flag(d, bgx, YB + 4 * 34 + 14, "GND", "down")
    d.lines(bgx + 30, YB + 10, ["switches to GND (encoder:", "A, B, push; C pin = GND),", "no resistors needed", "(internal pull-ups,", " software debounce)"], size=11, color="#555")

    # microSD
    sdb = block(d, cx, YS - 22, YS + 3 * S + 22, 150, "microSD breakout",
                [(YS + k * S, "", n) for k, (n, _, _) in enumerate(sd)],
                [(YS, "", "VCC"), (YS + S, "", "GND")], subtitle="3.3 V type, SPI", fill="#eef7f7")
    for n, gp, _ in sd:
        wire(d, pico[f"GP{gp}"], sdb[n], n, label_x=355)
    flag(d, *sdb["VCC"], "3V3", "right")
    flag(d, *sdb["GND"], "GND", "right")

    # LIS3DH
    lb = block(d, cx, YL - 22, YL + 5 * S + 22, 150, "Adafruit LIS3DH",
               [(YL, "", "SDA"), (YL + S, "", "SCL"), (YL + 2 * S, "", "INT")],
               [(YL, "", "Vin"), (YL + S, "", "GND"), (YL + 2 * S, "", "3Vo"), (YL + 3 * S, "", "SDO"),
                (YL + 4 * S, "", "CS")], subtitle="I2C address 0x18", fill="#fbf3ee")
    for n, gp, _ in lis:
        wire(d, pico[f"GP{gp}"], lb[n], "INT1" if n == "INT" else n, label_x=355, label=n)
    flag(d, *lb["Vin"], "3V3", "right")
    flag(d, *lb["GND"], "GND", "right")
    for n in ("3Vo", "SDO", "CS"):
        nc(d, lb[n][0] + 4, lb[n][1])
    d.lines(cx + 220, YL + 3 * S + 4, ["leave open"], size=10.5, color="#555")

    # insets
    used = {1: ("strip data", "DIN"), 36: ("→ 3V3 flag", "3V3"), 39: ("← D1 ← +5V", "+5V")}
    used.update({phys: (label, "BTN") for _, phys, label in buttons})
    used.update({phys: (n, n) for n, _, phys in sd})
    used.update({phys: (n, "INT1" if n == "INT" else n) for n, _, phys in lis})
    pico_pinout(d, 1480, 560, used, "Pico 2 W pinout (controller)")

    decoupling(d, 1020, 820, [("+5V", "C1", "100 nF", "U1 pin 14–7", False),
                              ("3V3", "C2", "10 µF", "microSD VCC", True)], columns=3, dx=118)
    legend(d, 1000, 600, [("DIN", "strip data"), ("BTN", "buttons"), ("SDA", "I2C SDA / SCL"),
                          ("INT1", "LIS3DH INT1"), ("MISO", "SD card SPI"), ("+5V", "+5V (power bank)"),
                          ("3V3", "3V3 (Pico 3V3(OUT))"), ("GND", "GND (all connected)")])

    power_section(d, 30, YL + 5 * S + 110,
                  [("Strip start", "+5V / GND at LED 0"), ("Strip end", "+5V / GND at LED 2N-1"),
                   ("+5V / GND flags", "U1, D1 → VSYS, 1000 µF")],
                  ["• All GND flags are one net: power bank GND, strip, 74AHCT125, Pico GND.",
                   "• Pico powered from +5V only through D1 (1N5819, stripe towards VSYS), never directly.",
                   "• Decoupling capacitors C1, C2: see above right; C3 1000 µF at the strip.",
                   "• Strip full white (~5 A) exceeds the power bank; the firmware limits to ~2.6 A."])
    d.save(path)


if __name__ == "__main__":
    docs = Path(__file__).resolve().parent.parent / "docs"
    display_sheet(docs / "wiring_display.svg")
    controller_sheet(docs / "wiring_controller.svg")
    print("wrote", docs / "wiring_display.svg", "and", docs / "wiring_controller.svg")
