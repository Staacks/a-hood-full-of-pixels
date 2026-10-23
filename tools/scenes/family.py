"""Family (private playlist): faces from my own photos, picked with tools/family.py
(private/family/selection.json, never published). The faces take turns in a shuffled order
every few seconds (crossfades), each one blinks now and then and zooms in very slowly.

Blinking: the faces are stabilised (tools/facetrack.py), so the eyes sit at known canvas
positions. A closing lid is painted over each eye in the skin colour from just above it, with a
dark lash line at its edge.

Intro: the faces fade in from the dark. Staff: warm golden glitter; intro: a golden sweep up.
Without a selection the scene is skipped (tools/make_scenes.py).
"""

import functools
import json
import math
from pathlib import Path

import cv2
import numpy as np

import staffled
from maskvid import CANVAS_H, CANVAS_MM, CANVAS_W, CX, CY, coverage, mm_to_canvas, sample_canvas

SELECTION = Path(__file__).resolve().parents[2] / "private" / "family" / "selection.json"
AVAILABLE = SELECTION.exists()

HOLD = 4.0           # s per face (the crossfade included)
FADE = 0.6           # s crossfade
ZOOM = 0.04          # slow zoom per face
LEVEL = 0.4          # linear brightness of a face's bright parts (90th percentile)
BLINK_S = 0.22
EYES = (26.0, 22.0)  # mm, where tools/family.py puts the eyes
EYE_RX, EYE_RY = 10.5, 4.2   # mm, half size of the eye opening (about 0.4 x the eye distance)
ORDER_SEED = 11

_faces = json.loads(SELECTION.read_text())["faces"] if AVAILABLE else []
SECONDS = max(1, len(_faces)) * HOLD
INTRO_SECONDS = 1.0
STAFF_SECONDS = 8.0
STAFF_INTRO_SECONDS = 1.0


@functools.cache
def _portraits():
    """Linear canvases of the selected faces, brightness normalised, in the shuffled order (a
    face chosen with --first stays first: it follows the intro)."""
    import family
    sel = json.loads(SELECTION.read_text())
    photo_dir = Path(sel["photo_dir"])
    out = []
    for f in _faces:
        canvas = family.portrait(photo_dir, f)
        level = LEVEL / max(1e-3, np.percentile(canvas.sum(axis=2), 90) / 3)
        out.append((canvas * level).astype(np.float32))
    keep = 1 if sel.get("first") else 0
    order = list(range(keep)) + [keep + i for i in np.random.default_rng(ORDER_SEED).permutation(len(out) - keep)]
    return [out[i] for i in order]


class _Lids:
    """Closing eyelids for one face: the closed eye is inpainted from the skin around it (OpenCV,
    so texture and glasses frames continue), the lid comes down over the eye, a dark lash line
    at its edge."""

    def __init__(self, canvas):
        self.canvas = canvas
        r = np.minimum(np.hypot((CX - EYES[0]) / EYE_RX, (CY - EYES[1]) / EYE_RY),
                       np.hypot((CX + EYES[0]) / EYE_RX, (CY - EYES[1]) / EYE_RY))
        self.area = coverage((r - 1.0) * EYE_RY, soft=0.8)
        enc = (np.clip(canvas, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
        hole = (r < 1.25).astype(np.uint8)
        closed = cv2.inpaint(enc, hole, 4, cv2.INPAINT_TELEA)
        self.closed = (closed / 255.0) ** 2.2
        self.dx = np.minimum(np.abs(CX - EYES[0]), np.abs(CX + EYES[0]))

    def frame(self, closed):
        if closed <= 0:
            return self.canvas
        top, bottom = EYES[1] + EYE_RY, EYES[1] - EYE_RY * 0.55
        edge = top - closed * (top - bottom) - 0.05 * self.dx ** 2 / EYE_RX   # curved like a lid
        cover = self.area * coverage(edge - CY, soft=0.6)
        img = self.canvas * (1 - cover[..., None]) + self.closed * cover[..., None]
        lash = np.exp(-((CY - edge) / 0.8) ** 2) * self.area * min(1.0, closed * 1.5)
        return img * (1 - 0.7 * lash[..., None])


def _blinks(rng):
    """Blink times within one face's hold time (one or two blinks, not in the crossfades)."""
    times = [rng.uniform(FADE + 0.3, HOLD - FADE - 0.3)]
    if rng.random() < 0.4:
        t2 = rng.uniform(FADE + 0.3, HOLD - FADE - 0.3)
        if abs(t2 - times[0]) > 1.0:
            times.append(t2)
    return times


def _closed(t, times):
    c = 0.0
    for b in times:
        u = (t - b) / BLINK_S
        if 0 <= u < 1:   # fast down, slower up
            c = max(c, u / 0.35 if u < 0.35 else 1 - (u - 0.35) / 0.65)
    return c


def _zoomed(img, z):
    """Scales the canvas by z about the eyes' midpoint."""
    c, r = mm_to_canvas(0.0, EYES[1])
    m = np.array([[z, 0, c * (1 - z)], [0, z, r * (1 - z)]], dtype=np.float32)
    return cv2.warpAffine(img, m, (CANVAS_W, CANVAS_H), flags=cv2.INTER_LINEAR, borderValue=0)


def render(n, fps, rng):
    faces = [_Lids(c) for c in _portraits()]
    blinks = [_blinks(rng) for _ in faces]
    count = len(faces)
    for k in range(n):
        t = (k / fps) % (count * HOLD)
        i = int(t // HOLD)
        u = t - i * HOLD                 # time within this face's turn
        img = _zoomed(faces[i].frame(_closed(u, blinks[i])), 1 + ZOOM * u / HOLD)
        if u > HOLD - FADE:              # crossfade into the next face (wraps to the first)
            j = (i + 1) % count
            a = (u - (HOLD - FADE)) / FADE
            a = a * a * (3 - 2 * a)
            nxt = _zoomed(faces[j].canvas, 1.0)
            img = img * (1 - a) + nxt * a
        yield sample_canvas(img)


def render_intro(n, fps, rng, loop):
    """The first face fades in from the dark; the loop starts with it."""
    first = sample_canvas(_portraits()[0])
    for k in range(n):
        u = min(1.0, k / fps / 0.7)
        yield first * (u * u * (3 - 2 * u))


# --- staff: golden glitter ---

GLOW = np.array([0.05, 0.025, 0.006])
SPARKLES = np.array([[1.0, 0.65, 0.25], [1.0, 0.35, 0.45], [1.0, 0.85, 0.6]])
SPARKLE_COUNT = 70          # per loop and side
SPARKLE_DECAY = 0.35        # s


def render_staff(n, fps, rng):
    hs = staffled.heights()
    loop_s = n / fps
    t0 = rng.uniform(0, loop_s, (2, SPARKLE_COUNT))
    h0 = rng.integers(0, staffled.BODY, (2, SPARKLE_COUNT))
    col = SPARKLES[rng.integers(0, len(SPARKLES), (2, SPARKLE_COUNT))] * rng.uniform(0.3, 0.8, (2, SPARKLE_COUNT, 1))
    for k in range(n):
        t = k / fps
        img = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            wave = 1 + 0.3 * np.sin(2 * math.pi * (hs / 40 - 2 * t / loop_s + side * 0.5))
            img[:, side] = wave[:, None] * GLOW
            age = (t - t0[side]) % loop_s            # periodic: sparkles wrap around the loop
            amp = np.where(age < 0.04, age / 0.04, np.exp(-(age - 0.04) / SPARKLE_DECAY))
            for j in np.nonzero(amp > 0.01)[0]:
                img[h0[side, j], side] += amp[j] * col[side, j]
        yield img


def render_staff_intro(n, fps, rng, loop):
    """A golden sweep runs up the staff and leaves the glitter behind."""
    hs = staffled.heights()[:, None, None]
    for k in range(n):
        t = k / fps
        front = t / 0.5 * (staffled.BODY + 10)
        f = loop[(len(loop) - n + k) % len(loop)]
        lit = np.clip((front - hs) / 8, 0, 1)
        head = np.exp(-((hs - front) / 3.0) ** 2) * SPARKLES[0] * 0.8
        yield f * lit + head
