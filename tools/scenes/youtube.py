"""YouTube face (fun playlist): the red rounded box of the YouTube logo with white eyes and a
white smiling mouth (the error-page face, but happy). Speech bubbles pop up above it, taking
turns: "like!" and "subscribe!". The face says them (the mouth talks), bounces when a bubble
pops up and blinks.

Intro: the logo pops up from nothing with an overshoot, the eyes open. Staff: a YouTube
progress bar filling up the staff (red, scrubber dot, buffered part in grey); intro: a red
"play" flash running up.
"""

import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

import staffled
from maskvid import CANVAS_H, CANVAS_MM, CANVAS_W, CX, CY, coverage, mm_to_canvas, sample_canvas, sdf_box, \
    sdf_ellipse, sdf_polygon

SECONDS = 6.0
INTRO_SECONDS = 1.0
STAFF_SECONDS = 6.0
STAFF_INTRO_SECONDS = 1.0

RED = np.array([0.75, 0.0, 0.0])
WHITE = 0.7
# The logo is wide, the mask tall: the face sits below the slit (|y| < 11 mm), the bubbles above
# it, away from the edges the hood covers; neither crosses the slit.
BOX = (0.0, -62.0, 66.0, 45.0, 26.0)        # centre x, y, half width, half height, corner radius
EYE_X, EYE_Y, EYE_RX, EYE_RY = 22.0, -35.0, 7.5, 10.0
MOUTH_Y, MOUTH_RX, MOUTH_RY = -68.0, 25.0, 19.0
BUBBLE = (0.0, 50.0, 86.0, 22.0, 14.0)      # centre x, y, half width, half height, radius
TAIL = [(-36.0, 30.0), (-14.0, 30.0), (-40.0, 16.0)]   # last point = tip (the bubble grows from it)
BUBBLE_WHITE = 0.55
TEXT_MARGIN = 8.0                          # mm inside the bubble
TEXT_H = 30.0                              # mm, at most
# (text, pops up at, pops away at) in seconds; the loop's last 0.5 s stay empty, because the
# intro is crossfaded into them (tools/make_scenes.py)
SAYS = [("like!", 0.3, 2.5), ("subscribe!", 3.0, 5.2)]
POP_S = 0.25
BLINKS = (1.7, 4.6)
BLINK_S = 0.2
FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]


def _font(px):
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, px)
    return ImageFont.load_default(size=px)


def text_mask(text, cx, cy, max_w, max_h):
    """Canvas-sized coverage (0..1) of text, as large as fits into max_w x max_h mm at (cx, cy)."""
    px = int(max_h / CANVAS_MM * 1.4)
    while True:
        font = _font(px)
        l, t, r, b = font.getbbox(text)
        if (r - l) * CANVAS_MM <= max_w and (b - t) * CANVAS_MM <= max_h:
            break
        px -= 1
    img = Image.new("L", (CANVAS_W, CANVAS_H))
    col, row = mm_to_canvas(cx, cy)
    ImageDraw.Draw(img).text((col - (l + r) / 2, row - (t + b) / 2), text, fill=255, font=font)
    return np.asarray(img, dtype=np.float64) / 255


def _bubble_layer(text):
    """RGB and alpha of a speech bubble with its text."""
    cx, cy, hw, hh, r = BUBBLE
    shape = np.minimum(sdf_box(CX, CY, cx, cy, hw, hh, radius=r), sdf_polygon(CX, CY, TAIL))
    alpha = coverage(shape, soft=0.7)
    ink = text_mask(text, cx, cy, 2 * (hw - TEXT_MARGIN), min(TEXT_H, 2 * hh - TEXT_MARGIN))
    rgb = (alpha * (1 - ink) * BUBBLE_WHITE)[..., None] * np.ones(3)
    return rgb, alpha


def _scaled(layer, s):
    """Scales a canvas layer (H, W[, 3]) by s around the bubble's tail tip."""
    if s <= 0.01:
        return np.zeros_like(layer)
    tc, tr = mm_to_canvas(*TAIL[-1])
    rows, cols = np.mgrid[0:CANVAS_H, 0:CANVAS_W].astype(np.float64)
    src = [(rows - tr) / s + tr, (cols - tc) / s + tc]
    if layer.ndim == 2:
        return ndimage.map_coordinates(layer, src, order=1, mode="constant")
    return np.stack([ndimage.map_coordinates(layer[..., c], src, order=1, mode="constant") for c in range(3)],
                    axis=-1)


def _pop(t):
    """Scale of a popping bubble t seconds after it starts to appear: 0 -> overshoot -> 1."""
    if t <= 0:
        return 0.0
    u = t / POP_S
    return 1 - math.exp(-5 * u) * math.cos(4.5 * u)


def face(sx=1.0, sy=1.0, closed=0.0, mouth=0.0, anchor_y=None):
    """The logo face; sx, sy scale it about anchor_y (default: its bottom edge, for the bounce),
    closed = eyelids 0..1, mouth = talking 0 (smile) .. 1 (wide open)."""
    cx, cy, hw, hh, r = BOX
    anchor = cy - hh if anchor_y is None else anchor_y
    sx, sy = max(sx, 0.01), max(sy, 0.01)
    x = CX / sx
    y = (CY - anchor) / sy + anchor
    box = coverage(sdf_box(x, y, cx, cy, hw, hh, radius=r), soft=0.7)
    ry = EYE_RY * max(0.08, 1 - closed)
    eyes = coverage(np.minimum(sdf_ellipse(x, y, -EYE_X, EYE_Y, EYE_RX, ry),
                               sdf_ellipse(x, y, EYE_X, EYE_Y, EYE_RX, ry)), soft=0.7)
    # smile: the lower half of an ellipse; talking opens it further down
    m_ry = MOUTH_RY * (1 + 0.35 * mouth)
    smile = np.maximum(sdf_ellipse(x, y, 0.0, MOUTH_Y, MOUTH_RX * (1 - 0.15 * mouth), m_ry), y - MOUTH_Y)
    mouth_c = coverage(smile, soft=0.7)
    white = np.maximum(eyes, mouth_c) * box
    return box[..., None] * RED + white[..., None] * (WHITE - RED)


def _closed(t):
    c = 0.0
    for b in BLINKS:
        d = abs(t - b) / (BLINK_S / 2)
        if d < 1:
            c = max(c, 1 - d * d)
    return c


def render(n, fps, rng):
    layers = [_bubble_layer(text) for text, _, _ in SAYS]
    for k in range(n):
        t = k / fps
        img = None
        sy = 1.0
        mouth = 0.0
        bubble = np.zeros((CANVAS_H, CANVAS_W, 3))
        alpha = np.zeros((CANVAS_H, CANVAS_W))
        for (rgb, a), (_, t_in, t_out) in zip(layers, SAYS):
            if t_in <= t <= t_out + POP_S:
                s = _pop(t - t_in) if t < t_out else max(0.0, 1 - (t - t_out) / (0.6 * POP_S))
                bubble = _scaled(rgb, s)
                alpha = _scaled(a, s)
            dt = t - t_in
            if 0 <= dt < 1.0:  # bounce when the bubble pops up, the mouth says it
                sy += 0.05 * math.exp(-dt * 6) * math.sin(dt * 25)
            if 0 <= dt < 0.6:
                mouth = max(mouth, abs(math.sin(dt / 0.6 * 3 * math.pi)) * (1 - dt / 0.6 * 0.3))
        img = face(1 / math.sqrt(sy), sy, _closed(t), mouth)
        img = img * (1 - alpha[..., None]) + bubble
        yield sample_canvas(img)


def render_intro(n, fps, rng, loop):
    for k in range(n):
        t = k / fps
        s = _pop(t * POP_S / 0.45)        # pops up within ~0.45 s
        opening = min(1.0, max(0.0, (t - 0.45) / 0.15))
        yield sample_canvas(face(s, s, 1 - opening, 0.0, anchor_y=BOX[1]))


# --- staff: progress bar ---

BAR_RED = np.array([0.8, 0.0, 0.0])
BAR_GREY = 0.035
BUFFER_GREY = 0.09
FILL_S = 5.4      # the bar fills within this time, then fades out (next video)
BUFFER_AHEAD = 14


def render_staff(n, fps, rng):
    hs = staffled.heights()
    for k in range(n):
        t = k / fps
        head = staffled.BODY * min(1.0, t / FILL_S)
        fade = 1.0 if t < FILL_S else max(0.0, 1 - (t - FILL_S) / (SECONDS - FILL_S))
        buffered = np.clip(head + BUFFER_AHEAD - hs, 0, 1) * fade
        grey = BAR_GREY + (BUFFER_GREY - BAR_GREY) * buffered
        fill = np.clip(head - hs + 0.5, 0, 1) * fade
        img = grey[:, None] * (1 - fill[:, None]) + fill[:, None] * BAR_RED
        dot = np.exp(-((hs - head) / 1.2) ** 2) * fade * (t < FILL_S)  # scrubber
        img = img + dot[:, None] * np.array([0.6, 0.3, 0.3])
        yield np.stack([img, img], axis=1)


def render_staff_intro(n, fps, rng, loop):
    hs = staffled.heights()[:, None, None]
    for k in range(n):
        t = k / fps
        front = staffled.BODY * t / 0.35
        trail = np.exp(-np.clip(front - hs, 0, None) / 18.0) * (hs <= front) * math.exp(-max(0.0, t - 0.35) / 0.2)
        yield trail * BAR_RED + np.exp(-((hs - front) / 2.0) ** 2) * 0.7
