"""Jack-o'-lantern / Pumpkinhead (serious playlist): a carved pumpkin with angry slanted eyes, a
triangle nose and a jagged grin. A candle burns inside: the carved holes glow and flicker, the
cut walls shine brighter at the rim, the dim skin catches some of the light near the holes.
Now and then the flame flares up.

Intro: a dark pumpkin; the candle catches (a few flickers), flares up and settles. Staff: a dim
ember glow with sparks rising slowly; intro: a flame shooting up the staff.
"""

import math

import numpy as np

import staffled
from maskvid import CX, CY, coverage, sample_canvas, sdf_box, sdf_ellipse, sdf_polygon

SECONDS = 8.0
INTRO_SECONDS = 1.4
STAFF_SECONDS = 8.0
STAFF_INTRO_SECONDS = 1.4

SKIN = np.array([0.20, 0.035, 0.0])
SKIN_DARK = np.array([0.025, 0.003, 0.0])
STEM = np.array([0.10, 0.10, 0.015])
FLAME = np.array([1.0, 0.72, 0.2])
FLAME_DEEP = np.array([1.0, 0.32, 0.02])
RIM = np.array([0.9, 0.3, 0.02])  # the cut wall
FLARES = (2.4, 6.3)   # s


def lobes():
    """(cx, rx, ry) of the pumpkin's ribs, centre first."""
    return [(0.0, 40.0, 98.0), (-42.0, 42.0, 95.0), (42.0, 42.0, 95.0), (-76.0, 30.0, 84.0), (76.0, 30.0, 84.0)]


CENTRE_Y = -18.0


def holes_sdf(x, y):
    eyes = []
    for s in (-1, 1):
        # angry: the top edge falls towards the nose
        pts = [(s * 10, 6), (s * 60, 34), (s * 52, -4), (s * 34, -10)]
        eyes.append(sdf_polygon(x, y, pts if s > 0 else pts[::-1]))
    nose = sdf_polygon(x, y, [(0, -16), (12, -38), (-12, -38)])
    # grin: between two curves rising to the sides, teeth left standing
    mouth = np.maximum(np.maximum(y - (-56 + 0.0042 * x ** 2), (-92 + 0.0075 * x ** 2) - y), np.abs(x) - 78)
    def top_at(cx):
        return -56 + 0.0042 * cx ** 2

    def bottom_at(cx):
        return -92 + 0.0075 * cx ** 2

    # blocky teeth left standing: wide at the gum, narrower at the tip
    teeth = [sdf_polygon(x, y, [(cx - 9, top_at(cx) + 4), (cx + 9, top_at(cx) + 4), (cx + 5, top_at(cx) - 15),
                                (cx - 5, top_at(cx) - 15)]) for cx in (-38, 0, 38)]
    teeth += [sdf_polygon(x, y, [(cx - 9, bottom_at(cx) - 4), (cx - 5, bottom_at(cx) + 14),
                                 (cx + 5, bottom_at(cx) + 14), (cx + 9, bottom_at(cx) - 4)]) for cx in (-58, -19, 19, 58)]
    for t in teeth:
        mouth = np.maximum(mouth, -t)
    return np.minimum.reduce(eyes + [nose, mouth])


class _Pumpkin:
    def __init__(self):
        body = np.full(CX.shape, np.inf)
        shade = np.zeros(CX.shape)
        for cx, rx, ry in lobes():
            d = sdf_ellipse(CX, CY, cx, CENTRE_Y, rx, ry)
            # each rib: round shading, darker towards its edges (the grooves between the ribs)
            nz = np.sqrt(np.clip(1 - ((CX - cx) / rx) ** 2 - ((CY - CENTRE_Y) / ry) ** 2, 0, 1))
            front = 1.0 - 0.25 * abs(cx) / 76
            shade = np.where(d < body, nz * front, shade)
            body = np.minimum(body, d)
        self.body = coverage(body, soft=0.8)
        self.shade = shade
        stem = sdf_polygon(CX, CY, [(-9, 74), (9, 74), (14, 102), (22, 112), (12, 114), (-4, 100)])
        self.stem = coverage(stem, soft=0.7)
        d = holes_sdf(CX, CY)
        self.hole = coverage(d, soft=0.7)
        self.rim = np.exp(-np.maximum(d, 0) / 1.2) * (d > -0.5) * self.body
        self.spill = np.exp(-np.maximum(d, 0) / 8.0) * (1 - self.hole) * self.body
        # inside: brighter towards the candle (low in the middle)
        self.inner = np.clip(1.2 - np.hypot(CX / 120, (CY + 45) / 90), 0.3, 1.0)

    def frame(self, light, dim=1.0):
        """light: candle brightness (1 = normal); dim: the skin's base light."""
        skin = (SKIN_DARK + (SKIN - SKIN_DARK) * self.shade[..., None] ** 1.5) * dim
        skin = skin + self.spill[..., None] * FLAME_DEEP * 0.1 * light
        img = skin * (self.body * (1 - self.hole))[..., None]
        img += (self.stem * 0.9)[..., None] * STEM * dim
        inside = FLAME_DEEP + (FLAME - FLAME_DEEP) * self.inner[..., None]
        img += (self.rim * (1 - self.hole))[..., None] * RIM * 0.5 * light
        img = img * (1 - self.hole[..., None]) + (self.hole * self.body)[..., None] * inside * 0.8 * light
        return np.clip(img, 0, 1)


def _flicker(n, rng, depth=0.12):
    """Periodic candle flicker around 1 (whole cycles per loop)."""
    u = np.arange(n) / n
    f = np.zeros(n)
    for _ in range(8):
        k = rng.integers(5, 60)
        f += math.sqrt(2 / 8) * np.sin(2 * math.pi * (k * u + rng.random()))
    return 1 + depth * f


def _flare(t):
    return sum(0.45 * math.exp(-((t - f) / 0.25) ** 2) for f in FLARES)


def render(n, fps, rng):
    p = _Pumpkin()
    flicker = _flicker(n, rng)
    for k in range(n):
        yield sample_canvas(p.frame(flicker[k] + _flare(k / fps)))


def render_intro(n, fps, rng, loop):
    """Dark pumpkin; the candle catches with a few flickers (0.15-0.6 s), flares up, settles."""
    p = _Pumpkin()
    sparks = [(0.15, 0.25), (0.32, 0.4), (0.45, 0.6)]
    for k in range(n):
        t = k / fps
        light = 0.0
        for a, b in sparks:
            if a <= t < b:
                light = 0.5 * (0.6 + 0.4 * rng.random())
        if t >= 0.6:
            light = 1.0 + 0.9 * math.exp(-(t - 0.6) / 0.25)
        yield sample_canvas(p.frame(light, dim=min(1.0, 0.25 + light * 0.75)))


# --- staff: ember glow with rising sparks ---

GLOW = np.array([0.28, 0.06, 0.0])
SPARK = np.array([1.0, 0.45, 0.05])
SPARKS = 7          # per side
SPARK_SPEED = (8.0, 16.0)   # heights per second


def render_staff(n, fps, rng):
    hs = staffled.heights()
    flicker = _flicker(n, rng, depth=0.15)
    # glow: stronger at the bottom, slow waves moving up
    base = 0.35 + 0.65 * np.exp(-hs / 45.0)
    # sparks: each rises through the staff a whole number of times per loop (seamless)
    loop_s = n / fps
    speeds = rng.uniform(*SPARK_SPEED, (2, SPARKS))
    laps = np.maximum(1, np.round(speeds * loop_s / (staffled.BODY + 10)))
    phase = rng.random((2, SPARKS))
    for k in range(n):
        u = k / n
        img = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            wave = 1 + 0.25 * np.sin(2 * math.pi * (hs / 30 - 3 * u + side * 0.5))
            img[:, side] = (base * wave * flicker[k])[:, None] * GLOW
            for j in range(SPARKS):
                h = ((phase[side, j] + laps[side, j] * u) % 1.0) * (staffled.BODY + 10) - 5
                fade = 1 - h / (staffled.BODY + 5)
                img[:, side] += (np.exp(-((hs - h) / 0.9) ** 2) * max(0.0, fade))[:, None] * SPARK * 0.8
        yield img


def render_staff_intro(n, fps, rng, loop):
    """A flame shoots up the staff when the candle catches (0.6 s), leaving the glow behind."""
    hs = staffled.heights()[:, None, None]
    for k in range(n):
        t = k / fps
        front = (t - 0.55) / 0.4 * staffled.BODY
        f = loop[(len(loop) - n + k) % len(loop)]
        lit = np.clip((front - hs) / 10, 0, 1)
        tongue = np.exp(-((hs - front) / 3.0) ** 2) * (t > 0.55) * SPARK
        yield f * lit + tongue
