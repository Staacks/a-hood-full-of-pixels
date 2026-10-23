"""Rick Astley singing (fun playlist, for a Rick Roll).

With the music video at "nonfree original/rick-roll.mp4" (copyrighted, not in the repo), faces
tracked and held still on the mask (tools/facetrack.py), the background cut away:
- intro: the shot 0:18.24-0:22.92 (between two cuts), "We're no strangers to love ..."; Rick
  pops up from below while he starts singing. The scene's sound is the song from there to its
  end (tools/make_sounds.py), so the lips are in sync during the intro;
- loop: the close-up 1:09.12-1:12.60, its first 80 frames, the last 7 crossfaded over the
  start (no longer in sync with the song, which plays on until it ends or the next scene fires);
- staff: a neon equaliser driven by the song (low notes on the front, voice on the back): the
  staff intro covers the whole rest of the song, then the staff loops on the loop clip's audio;
  at the start both bars shoot up to the crystal.

Without the video: a drawn portrait with the big auburn quiff, singing and bobbing to the beat
(113 BPM, the song's tempo) in front of dim 80s-style spotlights, 16 beats per loop (not
lip-synced), and the equaliser bouncing on the beat.
"""

import functools
import math
from pathlib import Path

import numpy as np
from scipy import ndimage

import staffled
from maskvid import CANVAS_MM, CX, CY, coverage, sample_canvas, sdf_ellipse

VIDEO = Path(__file__).resolve().parents[2] / "nonfree original" / "rick-roll.mp4"
VIDEO_FPS = 25
SONG_START = 18.24      # s, the intro shot: the first frame after the cut, the sound starts here
SONG_FRAMES = 117       # up to the next cut (22.92 s)
CLIP_START = 69.12      # s, the loop's close-up: the first frame after the cut
CLIP_FRAMES = 87        # up to the next cut (72.60 s)
CLIP_LOOP = 80          # loop length in clip frames; the rest is crossfaded over the start
CLIP_EYES = (26.0, 22.0)   # mm: the face's eyes at (+-x, y), above the slit (visible)
CLIP_GAIN = 2.2
HAVE_VIDEO = VIDEO.exists()



def _song_seconds():
    """Length of the sound: from SONG_START to the end of the video."""
    import subprocess
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                          str(VIDEO)], check=True, capture_output=True, text=True).stdout
    return float(out) - SONG_START


BPM = 113
BEATS = 16
if HAVE_VIDEO:
    INTRO_SECONDS = SONG_FRAMES / VIDEO_FPS
    STAFF_INTRO_SECONDS = _song_seconds()
    SECONDS = CLIP_LOOP / VIDEO_FPS
else:
    INTRO_SECONDS = STAFF_INTRO_SECONDS = 1.0
    SECONDS = BEATS * 60 / BPM
STAFF_SECONDS = SECONDS


def srgb(r, g, b, level=1.0):
    return (np.array([r, g, b]) / 255.0) ** 2.2 * level


SKIN = srgb(235, 180, 150, 0.8)
SKIN_SHADOW = srgb(150, 85, 65, 0.8)
HAIR = srgb(175, 75, 30, 0.9)
HAIR_DARK = srgb(95, 35, 12, 0.9)
LIPS = srgb(180, 80, 75, 0.8)
MOUTH = srgb(40, 8, 8, 0.8)
TEETH = srgb(225, 220, 205, 0.7)
SCLERA = srgb(220, 220, 215, 0.6)
IRIS = srgb(60, 95, 130, 0.8)
COAT = srgb(30, 30, 38, 0.8)
SHIRT = srgb(210, 210, 200, 0.6)
SPOTS = [(srgb(255, 0, 170, 0.05), 0.0), (srgb(0, 200, 255, 0.05), 0.5)]

EYE_X, EYE_Y = 25.0, 17.0
PIVOT = (0.0, -110.0)   # the head tilts about the neck


def _rot(x, y, angle_deg, pivot):
    a = math.radians(angle_deg)
    dx, dy = x - pivot[0], y - pivot[1]
    return pivot[0] + dx * math.cos(a) + dy * math.sin(a), pivot[1] - dx * math.sin(a) + dy * math.cos(a)


def _layer(img, cov, colour):
    """Paints colour (array or per-pixel image) where cov (0..1)."""
    c = cov[..., None]
    return img * (1 - c) + c * colour


def portrait(dx=0.0, dy=0.0, tilt=0.0, mouth=0.0, closed=0.0, brows=0.0, beat=0.0, body_dy=None):
    """Canvas image. dx, dy: head offset (mm); tilt in degrees; mouth 0 (closed) .. 1 (wide
    open); closed: eyelids 0..1; brows: raised by up to 3 mm; beat: background phase; body_dy:
    body offset (default: half the head's bob)."""
    img = np.zeros(CX.shape + (3,))
    # background: two coloured spotlights circling slowly
    for colour, phase in SPOTS:
        a = 2 * math.pi * (beat / BEATS + phase)
        sx, sy = 70 * math.cos(a), 40 + 50 * math.sin(a)
        img += np.exp(-((CX - sx) ** 2 + (CY - sy) ** 2) / (2 * 55.0 ** 2))[..., None] * colour

    # body (moves half as much as the head): neck, dark coat with sloping shoulders, white shirt
    bx = CX - dx * 0.5
    by = CY - (dy * 0.5 if body_dy is None else body_dy)
    shoulders = -100.0 - 0.12 * np.abs(bx)
    img = _layer(img, coverage(np.abs(bx) - 25.0, soft=0.8) * (by > shoulders - 10) * (by < -60), SKIN_SHADOW)
    img = _layer(img, coverage(by - shoulders, soft=0.8), COAT)
    img = _layer(img, coverage(np.maximum(np.abs(bx) - 12.0 - 0.7 * (shoulders - by), by - shoulders), soft=0.8),
                 SHIRT)

    # head coordinates
    x, y = _rot(CX - dx, CY - dy, tilt, PIVOT)
    face_d = np.minimum(sdf_ellipse(x, y, 0.0, -8.0, 58.0, 74.0), sdf_ellipse(x, y, 0.0, -52.0, 46.0, 44.0))
    ears = np.minimum(sdf_ellipse(x, y, -58.0, 2.0, 9.0, 17.0), sdf_ellipse(x, y, 58.0, 2.0, 9.0, 17.0))
    img = _layer(img, coverage(ears, soft=0.8), SKIN_SHADOW)
    face = coverage(face_d, soft=0.8)
    nz = np.sqrt(np.clip(1 - (x / 64) ** 2 - ((y + 15) / 95) ** 2, 0, 1))
    shade = (0.45 + 0.55 * nz)[..., None]
    skin = SKIN_SHADOW + (SKIN - SKIN_SHADOW) * shade
    # cheeks a little pink, shadow under the nose and the lower lip, chin highlight
    skin = skin * (1 + 0.12 * np.exp(-(((np.abs(x) - 30) / 12) ** 2 + ((y + 25) / 10) ** 2)))[..., None]
    img = _layer(img, face, skin)

    # nose: shadow along the left side, nostrils, tip highlight
    nose_shadow = np.exp(-(((x + 7) / 3.0) ** 2 + ((y + 12) / 14) ** 2)) * 0.5 \
        + np.exp(-((x / 9) ** 2 + ((y + 33) / 3.5) ** 2)) * 0.45   # under the nose
    nostrils = np.exp(-(((np.abs(x) - 6) / 2.6) ** 2 + ((y + 28) / 1.8) ** 2)) * 0.7
    tip = np.exp(-((x / 6) ** 2 + ((y + 22) / 5) ** 2)) * 0.2
    img = img * (1 - (nose_shadow + nostrils) * face)[..., None] * (1 + tip * face)[..., None]

    # eyes
    for s in (-1, 1):
        ex = s * EYE_X
        almond = np.maximum(np.hypot(x - ex, y - (EYE_Y - 9)) - 13.5, np.hypot(x - ex, y - (EYE_Y + 9)) - 13.5)
        lid = EYE_Y + 4.5 - closed * 9
        eye = coverage(almond, soft=0.6) * coverage(y - lid, soft=0.6)
        iris = coverage(np.hypot(x - ex, y - EYE_Y) - 3.6, soft=0.6) * eye
        img = _layer(img, eye, SCLERA)
        img = _layer(img, iris, IRIS * (1 - 0.6 * coverage(np.hypot(x - ex, y - EYE_Y) - 1.4, soft=0.5))[..., None])
        upper = coverage(np.abs(y - lid - 0.6) - 1.0, soft=0.6) * coverage(np.abs(x - ex) - 11, soft=0.6)
        img = _layer(img, upper * face, HAIR_DARK * 0.6)
        brow_y = EYE_Y + 11 + brows - 0.012 * (x - ex - s * 2) ** 2
        brow = coverage(np.maximum(np.abs(y - brow_y) - 2.6, np.abs(x - ex - s * 2) - 13), soft=0.7)
        img = _layer(img, brow, HAIR_DARK)

    # mouth: lips ring, dark opening, upper teeth
    rx, ry = 15.0 - 4.0 * mouth, 2.0 + 11.0 * mouth
    my = -50.0 - 4.0 * mouth
    lips = coverage(sdf_ellipse(x, y, 0.0, my, rx + 3.0, ry + 2.5), soft=0.7)
    img = _layer(img, lips, LIPS)
    inner = coverage(sdf_ellipse(x, y, 0.0, my, rx, ry), soft=0.6)
    img = _layer(img, inner, MOUTH)
    teeth = inner * coverage(-(y - (my + ry - 3.5)), soft=0.6) * (mouth > 0.15)
    img = _layer(img, teeth, TEETH)

    # hair: big quiff (parted on his right, swept up), sides above the ears
    quiff = np.minimum.reduce([sdf_ellipse(x, y, 4.0, 66.0, 60.0, 26.0, angle_deg=-5),
                               sdf_ellipse(x, y, -6.0, 86.0, 44.0, 17.0, angle_deg=-10),
                               sdf_ellipse(x, y, -54.0, 34.0, 9.0, 26.0, angle_deg=-6),
                               sdf_ellipse(x, y, 55.0, 36.0, 8.0, 24.0, angle_deg=6)])
    hairline = y - (52.0 + 0.003 * x ** 2 - 0.08 * x)  # > 0 above the hairline
    hair_d = np.where(np.abs(x) > 42, quiff, np.maximum(quiff, -hairline))
    hair = coverage(hair_d, soft=0.8)
    strands = 0.7 + 0.3 * np.sin((x * 0.8 - y * 0.5) / 2.0)
    top_light = np.clip((y - 50) / 40, 0, 1) * 0.6 + 0.6
    img = _layer(img, hair, HAIR * (strands * top_light)[..., None])
    return img


def _timeline(rng):
    """Mouth opening per half beat (0 = rest) and a few raised-brow notes, periodic."""
    open_ = rng.uniform(0.25, 1.0, 2 * BEATS)
    open_[rng.random(2 * BEATS) < 0.2] = 0.0   # breaths
    open_[[2 * BEATS - 1, 2 * BEATS - 2]] = 0.0  # the loop ends (and the intro joins) mouth closed
    return open_


def _pose(u, open_):
    """Pose at loop position u (0..1)."""
    beat = u * BEATS
    half = beat * 2
    i = int(half) % (2 * BEATS)
    frac = half - int(half)
    mouth = open_[i] * math.sin(math.pi * frac) ** 0.6
    dy = -3.0 * abs(math.sin(math.pi * beat))
    dx = 6.0 * math.sin(math.pi * beat / 2)
    tilt = 3.0 * math.sin(math.pi * beat / 2 + 0.6)
    closed = 0.0
    for b in (5.5, 13.2):
        d = abs(beat - b) / 0.35
        if d < 1:
            closed = max(closed, 1 - d * d)
    brows = 3.0 * mouth if open_[i] > 0.85 else 0.0
    return dict(dx=dx, dy=dy, tilt=tilt, mouth=mouth, closed=closed, brows=brows, beat=beat)


def _render_drawn(n, fps, rng):
    open_ = _timeline(rng)
    for k in range(n):
        yield sample_canvas(portrait(**_pose(k / n, open_)))


def _render_intro_drawn(n, fps, rng, loop):
    """Pops up from below the mask with an overshoot (0.55 s), then smiles into the loop."""
    for k in range(n):
        t = k / fps
        dy = _pop_dy(t)
        yield sample_canvas(portrait(dy=dy, beat=0.0, body_dy=dy))


def _pop_dy(t):
    """Vertical offset (mm) popping up from below the mask with an overshoot within 0.55 s."""
    u = min(1.0, t / 0.55)
    back = 1 + 2.2 * (u - 1) ** 3 + 1.2 * (u - 1) ** 2   # ease out with overshoot
    return -240.0 * (1 - back)


# --- the video clips ---

def _face_level(canvas):
    """Brightness of the face's middle (for matching the two clips)."""
    sel = (np.abs(CX) < 30) & (np.abs(CY - CLIP_EYES[1] + 25) < 30)
    return float(np.median(canvas[sel].sum(axis=1)))


@functools.cache
def _clip(start, count):
    """Stabilised frontal canvases (linear) of count frames from start (s)."""
    import facetrack
    frames = facetrack.read_clip(VIDEO, start, count)
    return [c.astype(np.float32) for c in facetrack.stabilise(frames, CLIP_EYES, gain=CLIP_GAIN)]


def _loop_clip():
    return _clip(CLIP_START, CLIP_FRAMES)


@functools.cache
def _intro_clip():
    """The intro shot, brightness matched to the loop clip."""
    song, loop = _clip(SONG_START, SONG_FRAMES), _loop_clip()
    gain = np.median([_face_level(c) for c in loop]) / np.median([_face_level(c) for c in song])
    return [c * gain for c in song]


def _cyclic(seq, i):
    """Item i (0..CLIP_LOOP-1) of the looped clip: the frames after CLIP_LOOP are crossfaded
    over the first ones, so the last loop frame runs into the first."""
    seam = len(seq) - CLIP_LOOP
    if i < seam:
        a = (i + 1) / (seam + 1)
        return seq[CLIP_LOOP + i] * (1 - a) + seq[i] * a
    return seq[i]


def _clip_index(t):
    """Loop clip frame index at time t after the start of the loop."""
    return int(round(t * VIDEO_FPS)) % CLIP_LOOP


def render(n, fps, rng):
    if not HAVE_VIDEO:
        yield from _render_drawn(n, fps, rng)
        return
    clip = _loop_clip()
    for k in range(n):
        yield sample_canvas(_cyclic(clip, _clip_index(k / fps)))


def render_intro(n, fps, rng, loop):
    """The intro shot from its first frame, in sync with the sound; pops up from below."""
    if not HAVE_VIDEO:
        yield from _render_intro_drawn(n, fps, rng, loop)
        return
    clip = _intro_clip()
    for k in range(n):
        t = k / fps
        img = clip[min(len(clip) - 1, int(round(t * VIDEO_FPS)))]
        shift = -_pop_dy(t) / CANVAS_MM          # canvas rows down
        if shift > 0.01:
            img = ndimage.shift(img, (shift, 0, 0), order=1, mode="constant")
        yield sample_canvas(img)


# --- staff: neon equaliser (on the clip's audio, or on the beat) ---

def _neon(hs):
    """Colour by height: cyan at the bottom, magenta, yellow at the top."""
    u = hs / staffled.BODY
    stops = np.array([srgb(0, 220, 255), srgb(255, 0, 200), srgb(255, 220, 0)])
    i = np.clip((u * 2).astype(int), 0, 1)
    f = (u * 2 - i)[:, None]
    return stops[i] * (1 - f) + stops[i + 1] * f


def _render_staff_drawn(n, fps, rng):
    hs = staffled.heights()
    colour = _neon(hs) * 0.75
    peaks = rng.uniform(0.55, 1.0, (BEATS * 2, 2))   # per half beat and side
    peaks[::2] = np.maximum(peaks[::2], 0.85)       # on the beats higher
    for k in range(n):
        half = k / n * BEATS * 2
        i = int(half) % (BEATS * 2)
        frac = half - int(half)
        level = peaks[i] * math.exp(-frac * 2.2)    # jumps up on every half beat, decays
        img = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            top = level[side] * staffled.BODY
            on = np.clip(top - hs + 0.5, 0, 1)
            img[:, side] = colour * on[:, None] + 0.03 * colour
        yield img


def _shoot_up(t):
    """Bar height (in heights) of the intro's shot up to the crystal and back."""
    return staffled.BODY * min(1.0, t / 0.25) * (1 if t < 0.4 else math.exp(-(t - 0.4) / 0.25))


def _render_staff_intro_drawn(n, fps, rng, loop):
    hs = staffled.heights()
    colour = _neon(hs) * 0.75
    for k in range(n):
        t = k / fps
        top = _shoot_up(t)
        on = np.clip(top - hs + 0.5, 0, 1)[:, None]
        img = colour * on + 0.03 * colour
        yield np.stack([img, img], axis=1)


EQ_BANDS = ((40.0, 250.0), (250.0, 4000.0))   # Hz: front = low notes, back = the voice
EQ_RELEASE = 0.15                              # s, the bars fall back
AUDIO_RATE = 44100


def _band_db(start, seconds):
    """(frames, 2) band levels in dB per video frame of the audio from start (s)."""
    import subprocess
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-ss", str(start), "-i", str(VIDEO),
                          "-t", str(seconds + 0.1), "-vn", "-ac", "1", "-ar", str(AUDIO_RATE),
                          "-f", "f32le", "-"], check=True, capture_output=True).stdout
    audio = np.frombuffer(raw, dtype=np.float32)
    win = 2048
    hann = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1 / AUDIO_RATE)
    bands = [(freqs >= lo) & (freqs < hi) for lo, hi in EQ_BANDS]
    n = int(round(seconds * VIDEO_FPS))
    db = np.zeros((n, 2))
    for k in range(n):
        c = int(k / VIDEO_FPS * AUDIO_RATE)
        seg = np.zeros(win)
        part = audio[max(0, c - win // 2):c + win // 2]
        seg[:len(part)] = part
        power = np.abs(np.fft.rfft(seg * hann)) ** 2
        for side, band in enumerate(bands):
            db[k, side] = 10 * np.log10(power[band].sum() + 1e-12)
    return db


@functools.cache
def _eq_levels():
    """Bar heights 0..1 per video frame and side: (song from SONG_START, loop clip), both
    scaled by the song's levels."""
    song = _band_db(SONG_START, STAFF_INTRO_SECONDS)
    clip = _band_db(CLIP_START, CLIP_FRAMES / VIDEO_FPS)
    lo, hi = np.percentile(song, 10, axis=0), np.percentile(song, 98, axis=0)
    fall = math.exp(-1 / VIDEO_FPS / EQ_RELEASE)
    out = []
    for db in (song, clip):
        raw_level = np.clip((db - lo) / (hi - lo), 0, 1) ** 1.5
        level = np.zeros_like(raw_level)
        for k in range(len(db)):   # jumps up, falls back slowly
            level[k] = np.maximum(raw_level[k], level[k - 1] * fall if k else 0)
        out.append(level)
    return out


def _eq_image(level, hs, colour):
    img = np.zeros((staffled.BODY, 2, 3))
    for side in (0, 1):
        on = np.clip(0.05 + level[side] * 0.95 * staffled.BODY - hs + 0.5, 0, 1)
        img[:, side] = colour * on[:, None] + 0.03 * colour
    return img


def render_staff(n, fps, rng):
    if not HAVE_VIDEO:
        yield from _render_staff_drawn(n, fps, rng)
        return
    hs = staffled.heights()
    colour = _neon(hs) * 0.75
    levels = _eq_levels()[1]
    for k in range(n):
        yield _eq_image(_cyclic(levels, _clip_index(k / fps)), hs, colour)


def render_staff_intro(n, fps, rng, loop):
    if not HAVE_VIDEO:
        yield from _render_staff_intro_drawn(n, fps, rng, loop)
        return
    hs = staffled.heights()
    colour = _neon(hs) * 0.75
    levels = _eq_levels()[0]
    for k in range(n):
        t = k / fps
        level = np.maximum(levels[min(len(levels) - 1, int(round(t * VIDEO_FPS)))], _shoot_up(t) / staffled.BODY)
        yield _eq_image(level, hs, colour)
