"""Me (fun playlist): my own face, talking, from a recording ("video original/me.mp4":
the longest stretch looking straight at the camera, 7.8 s cut from raw footage), tracked
and held still on the mask, background cut away (tools/facetrack.py). The sound is my
signature jingle ("sound original/signature.wav").

Intro: dark until the jingle's first accent (0.4 s), then the face pops in with a flash and
keeps talking; the loop continues the clip (its last frames crossfaded over the start). Staff:
an equaliser driven by the jingle while it plays, then slow teal waves with a white pulse
running up.
"""

import functools
import math
import subprocess
from pathlib import Path

import numpy as np
from scipy import ndimage

import staffled
from maskvid import CANVAS_MM, CX, CY, sample_canvas

ROOT = Path(__file__).resolve().parents[2]
VIDEO = ROOT / "video original" / "me.mp4"
JINGLE = ROOT / "sound original" / "signature.wav"
AVAILABLE = VIDEO.exists()

VIDEO_FPS = 25
CLIP_FRAMES = 196
CLIP_LOOP = 188          # loop length in clip frames; the last 8 are crossfaded over the start
EYES = (31.0, 25.0)      # mm: large (short hair: the head fits, the neck runs off the bottom)
LEVEL = 0.6             # linear brightness of the face's bright parts (90th percentile)
HIT = 0.4                # s, the jingle's first accent: the face appears

SECONDS = CLIP_LOOP / VIDEO_FPS
INTRO_SECONDS = 1.0
STAFF_SECONDS = 6.0
STAFF_INTRO_SECONDS = 6.0   # the jingle


@functools.cache
def _clip():
    import facetrack
    frames = facetrack.read_clip(VIDEO, 0.0, CLIP_FRAMES)
    canvases = facetrack.stabilise(frames, EYES)
    sel = (np.abs(CX) < 30) & (np.abs(CY - EYES[1] + 25) < 30)   # the middle of the face
    level = np.median([np.percentile(c[sel].sum(axis=1), 90) / 3 for c in canvases])
    return [(c * LEVEL / level).astype(np.float32) for c in canvases]


def _cyclic(seq, i):
    """Item i (0..CLIP_LOOP-1) of the looped clip: the frames after CLIP_LOOP are crossfaded
    over the first ones, so the last loop frame runs into the first."""
    seam = len(seq) - CLIP_LOOP
    if i < seam:
        a = (i + 1) / (seam + 1)
        return seq[CLIP_LOOP + i] * (1 - a) + seq[i] * a
    return seq[i]


def render(n, fps, rng):
    clip = _clip()
    for k in range(n):   # continues where the intro ends
        yield sample_canvas(_cyclic(clip, int(round((INTRO_SECONDS + k / fps) * VIDEO_FPS)) % CLIP_LOOP))


def render_intro(n, fps, rng, loop):
    clip = _clip()
    eye_row = (131.25 - EYES[1]) / CANVAS_MM
    for k in range(n):
        t = k / fps
        if t < HIT:
            yield np.zeros_like(loop[0])
            continue
        u = (t - HIT) / 0.35
        scale = 1 - 0.25 * math.exp(-5 * u) * math.cos(6 * u)      # pops up with a little overshoot
        img = _cyclic(clip, int(round(t * VIDEO_FPS)) % CLIP_LOOP)
        rows, cols = img.shape[:2]
        zoom = ndimage.affine_transform(img, np.diag([1 / scale, 1 / scale, 1]),
                                        offset=((1 - 1 / scale) * eye_row, (1 - 1 / scale) * cols / 2, 0),
                                        order=1, mode="constant")
        flash = 0.6 * math.exp(-(t - HIT) / 0.12)
        lit = np.clip((zoom.sum(axis=2, keepdims=True) / 3 - 0.01) / 0.04, 0, 1)   # only the face, not faint edges
        yield sample_canvas(zoom * (1 + 2 * flash) + flash * 0.15 * lit)


# --- staff ---

TEAL = np.array([0.0, 0.55, 0.45])
ORANGE = np.array([1.0, 0.35, 0.0])
EQ_BANDS = ((40.0, 250.0), (250.0, 4000.0))   # Hz: front = low notes, back = the melody
EQ_RELEASE = 0.15


def _colour(hs):
    """Teal at the bottom, white in the middle, orange at the top."""
    u = (hs / staffled.BODY)[:, None]
    white = np.array([0.8, 0.8, 0.8])
    return np.where(u < 0.5, TEAL + (white - TEAL) * (u / 0.5), white + (ORANGE - white) * ((u - 0.5) / 0.5)) * 0.7


@functools.cache
def _jingle_levels(fps):
    """(frames, 2) bar heights 0..1 of the jingle."""
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", str(JINGLE), "-ac", "1", "-ar", "44100", "-f", "f32le",
                          "-"], check=True, capture_output=True).stdout
    audio = np.frombuffer(raw, dtype=np.float32)
    win = 2048
    hann = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1 / 44100)
    n = int(round(STAFF_INTRO_SECONDS * fps))
    db = np.zeros((n, 2))
    for k in range(n):
        c = int(k / fps * 44100)
        seg = np.zeros(win)
        part = audio[max(0, c - win // 2):c + win // 2]
        seg[:len(part)] = part
        power = np.abs(np.fft.rfft(seg * hann)) ** 2
        for side, (lo, hi) in enumerate(EQ_BANDS):
            db[k, side] = 10 * np.log10(power[(freqs >= lo) & (freqs < hi)].sum() + 1e-12)
    loud = db[:int(5.5 * fps)]
    lo, hi = np.percentile(loud, 10, axis=0), np.percentile(loud, 98, axis=0)
    raw_level = np.clip((db - lo) / (hi - lo), 0, 1) ** 1.5
    level = np.zeros_like(raw_level)
    fall = math.exp(-1 / fps / EQ_RELEASE)
    for k in range(n):
        level[k] = np.maximum(raw_level[k], level[k - 1] * fall if k else 0)
    return level


def render_staff(n, fps, rng):
    hs = staffled.heights()
    for k in range(n):
        u = k / n
        img = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            wave = 0.06 + 0.04 * np.sin(2 * math.pi * (hs / 35 - 2 * u + side * 0.5))
            pulse = np.exp(-((hs - ((3 * u + side * 0.5) % 1.0) * (staffled.BODY + 20) + 10) / 2.5) ** 2)
            img[:, side] = wave[:, None] * TEAL + pulse[:, None] * np.array([0.5, 0.6, 0.6])
        yield img


def render_staff_intro(n, fps, rng, loop):
    hs = staffled.heights()
    colour = _colour(hs)
    levels = _jingle_levels(fps)
    for k in range(n):
        img = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            on = np.clip(levels[k, side] * staffled.BODY - hs + 0.5, 0, 1)
            img[:, side] = colour * on[:, None] + 0.03 * colour
        yield img
