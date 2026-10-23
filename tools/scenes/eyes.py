"""Base scene: two glowing eyes under the hood, nothing else.

The eyes sit just above the slit at the wearer's eye columns (X = +-32 mm). They breathe slowly,
flicker a little and blink twice per loop.

Intro: the eyes open and flare up. The staff stays dark (the controller's built-in dark effect);
its intro is an ember glow sinking from the crystal to the ground.
"""

import math

import numpy as np

import staffled
from maskvid import CX, CY, coverage, sample_canvas, sdf_ellipse

SECONDS = 10.0
INTRO_SECONDS = 1.0
STAFF_INTRO_SECONDS = 1.0

EYE_X = 32.0
EYE_Y = 20.0           # centre, the lower lid ends just above the slit
COLOUR_CORE = np.array([1.0, 0.85, 0.35])   # hot yellow centre
COLOUR_GLOW = np.array([1.0, 0.25, 0.02])   # orange-red halo
BLINKS = (3.1, 8.2, 8.55)  # seconds (a double blink at the end)
BLINK_S = 0.22


def _eye_sdf(x, y, side):
    # almond, inner corner lower (looks down at the viewer, angry)
    a = side * 10  # rotate so that the inner corner (towards X = 0) is lower
    upper = sdf_ellipse(x, y, side * EYE_X, EYE_Y - 6.0, 18.0, 11.0, angle_deg=a)
    lower = sdf_ellipse(x, y, side * EYE_X, EYE_Y + 6.0, 18.0, 11.0, angle_deg=a)
    # straight brow: cuts the top along a line falling towards the nose
    brow = (y - (EYE_Y + 2.5 + 0.45 * (side * x - EYE_X))) * 0.9
    return np.maximum(np.maximum(upper, lower), brow)


def _lid(t):
    """Eyelid closure 0 (open) .. 1 (closed)."""
    c = 0.0
    for b in BLINKS:
        d = abs(t - b) / (BLINK_S / 2)
        if d < 1:
            c = max(c, 1 - d * d)
    return c


class _Eyes:
    def __init__(self):
        eye = np.minimum(_eye_sdf(CX, CY, -1), _eye_sdf(CX, CY, 1))
        self.core = coverage(eye + 2.0, soft=2.5)          # bright centre
        self.body = coverage(eye, soft=0.8)
        self.glow = np.exp(-np.maximum(eye, 0) / 5.0) * 0.3

    def frame(self, closed, level):
        # the lid comes down from above, the lower lid up a little
        top = EYE_Y + 12.0 - closed * 20.0
        bottom = EYE_Y - 8.0 + closed * 5.0
        open_mask = coverage(CY - top, 1.0) * coverage(bottom - CY, 1.0)
        img = (self.body[..., None] * COLOUR_GLOW * 0.9 + self.core[..., None] * COLOUR_CORE) * open_mask[..., None]
        img += self.glow[..., None] * COLOUR_GLOW * (1 - 0.7 * closed)
        return sample_canvas(img * level)


def render(n, fps, rng):
    eyes = _Eyes()
    flicker = np.convolve(rng.normal(0, 1, n + 8), np.ones(8) / 8, mode="valid")[:n]
    for k in range(n):
        t = k / fps
        breathe = 0.82 + 0.18 * math.sin(2 * math.pi * t / 5.0)
        yield eyes.frame(_lid(t), breathe * (1 + 0.05 * flicker[k]))


def render_intro(n, fps, rng, loop):
    """Closed -> open within 0.35 s (from 0.1 s on), flaring up to 1.5x and settling."""
    eyes = _Eyes()
    for k in range(n):
        t = k / fps
        opening = min(1.0, max(0.0, (t - 0.1) / 0.35))
        closed = 1 - opening * opening * (3 - 2 * opening)
        flare = 1 + 0.6 * math.exp(-max(0.0, t - 0.4) / 0.2) * opening
        yield eyes.frame(closed, 0.82 * flare)


def render_staff_intro(n, fps, rng, loop):
    """An ember glow sinks from the crystal to the ground and dies out (the staff is dark)."""
    hs = staffled.heights()[:, None, None]
    for k in range(n):
        t = k / fps
        front = staffled.BODY * (1 - t / 0.7)
        tail = np.exp(-np.clip(hs - front, 0, None) / 10.0) * (hs >= front - 1)
        yield tail * COLOUR_GLOW * 0.6 * math.exp(-t / 0.5)
