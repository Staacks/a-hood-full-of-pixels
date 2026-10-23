#!/usr/bin/env python3
"""Generate the system overview (staff, mask, speaker and how they connect) as SVG.

  python3 tools/system_overview.py        # writes docs/system_overview.svg
"""

from pathlib import Path

from wiring_diagrams import Drawing

BLE = "#1f77b4"
AUDIO = "#9467bd"
POWER = "#b00000"
WIRE = "#444"
GREY = "#555"

W, H = 1260, 620


def box(d, x, y, w, h, title, sub=None, fill="#fff"):
    d.rect(x, y, w, h, fill=fill, stroke="#333", width=1.6, rx=6)
    ty = y + h / 2 + (-3 if sub else 5)
    d.text(x + w / 2, ty, title, size=14, anchor="middle", weight="bold")
    if sub:
        d.text(x + w / 2, ty + 18, sub, size=12, anchor="middle", color=GREY)


def area(d, x, y, w, h, title, fill, right=False):
    d.rect(x, y, w, h, fill=fill, stroke="#999", width=1.2, rx=12)
    d.text(x + w - 18 if right else x + 18, y + 28, title, size=18, weight="bold", color="#222",
           anchor="end" if right else "start")


def radio_waves(d, x, y, color, direction=1):
    for r in (7, 12, 17):
        d.path(f"M{x:.1f},{y - r * 0.8:.1f} A{r},{r} 0 0 {1 if direction > 0 else 0} "
               f"{x:.1f},{y + r * 0.8:.1f}", stroke=color, width=1.6)


def radio_link(d, a, b, color, title, sub):
    radio_waves(d, a[0] + 8, a[1], color)
    radio_waves(d, b[0] - 8, b[1], color, -1)
    d.line([a, b], color, 3, dash="10,6")
    x = 650
    y = a[1] + (x - a[0]) * (b[1] - a[1]) / (b[0] - a[0])
    d.rect(x - 90, y - 22, 180, 44, fill="#fff", stroke=color, width=1.6, rx=8)
    d.text(x, y - 3, title, size=13, anchor="middle", weight="bold", color=color)
    d.text(x, y + 14, sub, size=11, anchor="middle", color=GREY)


def staff(d):
    area(d, 30, 70, 520, 530, "STAFF", "#f6f1e7")
    pico = (180, 290, 200, 80)
    box(d, 50, 130, 210, 60, "LED strip", "180 LEDs + crystal")
    box(d, 300, 130, 230, 60, "Controls", "rotary encoder, 2 buttons")
    box(d, *pico, "Raspberry Pi Pico 2 W", "controller", fill="#e9f2e4")
    box(d, 50, 470, 150, 60, "Power bank", "USB-C, 5 V")
    box(d, 215, 470, 150, 60, "microSD card", "animations, sounds")
    box(d, 380, 470, 150, 60, "Accelerometer", "stomp detection")
    d.line([(155, 190), (155, 240), (230, 240), (230, 290)], WIRE, 2)
    d.line([(415, 190), (415, 240), (330, 240), (330, 290)], WIRE, 2)
    d.line([(455, 470), (455, 420), (350, 420), (350, 370)], WIRE, 2)
    d.line([(290, 470), (290, 370)], WIRE, 2)
    d.line([(125, 470), (125, 420), (230, 420), (230, 370)], POWER, 2, dash="6,4")
    d.line([(50, 500), (40, 500), (40, 110), (100, 110), (100, 130)], POWER, 2, dash="6,4")


def mask(d):
    area(d, 750, 70, 480, 350, "MASK", "#eef1f8")
    box(d, 770, 110, 180, 55, "microSD card", "videos")
    box(d, 770, 205, 180, 80, "Raspberry Pi Pico 2 W", "display", fill="#e9f2e4")
    box(d, 770, 325, 180, 55, "Power bank", "USB-C, 5 V")
    px, pw, ph, gap = 990, 220, 110, 16
    for py in (120, 120 + ph + gap):
        d.rect(px, py, pw, ph, fill="#111", stroke="#000", width=1.5, rx=2)
        d.text(px + 8, py + 16, "LED panel 128 × 64", size=11, color="#bbb")
    ey = 120 + ph - 22
    for ex in (px + pw / 2 - 40, px + pw / 2 + 40):
        d.el.append(f'<ellipse cx="{ex:.1f}" cy="{ey:.1f}" rx="20" ry="11" fill="#ff8c1a" opacity="0.35"/>')
        d.el.append(f'<ellipse cx="{ex:.1f}" cy="{ey:.1f}" rx="12" ry="6" fill="#ffd27a"/>')
    d.text(px + pw / 2, 120 + ph + gap / 2 + 4, "eye slit", size=10, anchor="middle", color=GREY)
    d.line([(860, 165), (860, 205)], WIRE, 2)
    d.line([(950, 245), (970, 245), (970, 175), (px, 175)], WIRE, 2)
    d.line([(970, 245), (970, 301), (px, 301)], WIRE, 2)
    d.line([(860, 325), (860, 285)], POWER, 2, dash="6,4")
    d.line([(950, 368), (px + pw / 2, 368), (px + pw / 2, 120 + 2 * ph + gap)], POWER, 2, dash="6,4")


def speaker(d):
    area(d, 750, 450, 480, 150, "UNDER THE ROBE", "#f3eef8", right=True)
    sx, sy = 830, 495
    d.rect(sx, sy, 150, 80, fill="#2b2b2b", stroke="#000", width=1.5, rx=38)
    d.circle(sx + 38, sy + 40, 24, fill="#444", stroke="#111", width=2)
    d.circle(sx + 112, sy + 40, 24, fill="#444", stroke="#111", width=2)
    for i in range(5):
        d.line([(sx + 63 + i * 6, sy + 24), (sx + 63 + i * 6, sy + 56)], "#555", 2)
    d.text(sx + 170, sy + 46, "Bluetooth speaker", size=14, weight="bold")


def main():
    d = Drawing(W, H)
    d.text(30, 42, "A hood full of pixels — system overview", size=24, weight="bold")
    staff(d)
    mask(d)
    speaker(d)
    radio_link(d, (380, 315), (762, 235), BLE, "Bluetooth LE", "scene changes, clock sync")
    radio_link(d, (380, 350), (822, 535), AUDIO, "Bluetooth audio", "sound effects")
    out = Path(__file__).resolve().parent.parent / "docs" / "system_overview.svg"
    d.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
