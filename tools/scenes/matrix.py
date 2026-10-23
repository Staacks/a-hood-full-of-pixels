"""Green digital glyph rain; the glyphs are brighter at the eyes and the mouth, forming a face.

Rendered directly in panel pixels so the glyphs stay sharp. Each glyph column falls through
*virtual rows*: the upper panel, then the slit (as many rows as the gap in that column), then
the lower panel, so a drop keeps its speed across the slit. Everything is periodic in the loop
length (drop speeds, glyph changes), so the loop is seamless.

Intro: a curtain of bright heads falls down every glyph column and leaves the rain behind it.
Staff: drops falling down both sides, the trail flickering like changing glyphs; its intro is
the same curtain from the crystal down.
"""

import numpy as np

import staffled
from maskvid import FX, FY, H, W, coverage, gap_rows, sdf_ellipse

SECONDS = 8.0
INTRO_SECONDS = 1.6
STAFF_SECONDS = 8.0
STAFF_INTRO_SECONDS = 1.4

# Small glyphs: they are not readable at this size, but the rain reads as one texture and the
# face (made of brightness) shows through. Large glyphs (5x7) were seen as single characters.
GW, GH = 3, 5          # glyph size, plus 1 pixel spacing each way
CELL_W, CELL_H = GW + 1, GH + 1
COLS = W // CELL_W     # 32 glyph columns
X0 = (W - COLS * CELL_W + 1) // 2
GREEN = np.array([0.05, 1.0, 0.15])
HEAD = np.array([0.55, 1.0, 0.6])
DROPS_PER_COLUMN = 1
TRAIL_ROWS = (32, 100)  # length of a drop's trail (pixels, 1/e)


def make_glyphs(rng, count=64):
    """Katakana-like glyphs from random strokes on the GW x GH grid (mirrored, as in the films)."""
    mr, mc, br, bc = GH // 2, GW // 2, GH - 1, GW - 1
    strokes = []
    for r in (0, mr, br):
        strokes.append([(r, c) for c in range(GW)])            # horizontal
    for c in (0, mc, bc):
        strokes.append([(r, c) for r in range(GH)])            # vertical
    strokes.append([(r, r * GW // GH) for r in range(GH)])             # diagonal
    strokes.append([(r, bc - r * GW // GH) for r in range(GH)])        # other diagonal
    strokes.append([(r, mc) for r in range(mr)] + [(mr, c) for c in range(GW)])  # T
    strokes.append([(0, c) for c in range(GW)] + [(r, bc) for r in range(mr + 1)]
                   + [(mr + 1 + i, bc - 1 - i) for i in range(min(bc, br - mr))])  # hook
    strokes.append([(r, 0) for r in range(mr + 1)] + [(mr, c) for c in range(mc + 1)])  # corner
    lo, hi = 0.35 * GW * GH, 0.75 * GW * GH
    glyphs = []
    while len(glyphs) < count:
        g = np.zeros((GH, GW))
        for i in rng.choice(len(strokes), rng.integers(2, 4), replace=False):
            for r, c in strokes[i]:
                g[r, c] = 1
        if lo <= g.sum() <= hi:
            glyphs.append(g[:, ::-1] if rng.random() < 0.5 else g)
    return np.array(glyphs)


def face_map():
    """Per-pixel boost 0..1: eyes and mouth (frontal mm). Menacing: the eyes slant down towards
    the nose under a straight brow, the mouth is a grim slit with drooping corners."""
    eyes = np.full(FX.shape, np.inf)
    for side in (-1, 1):
        e = sdf_ellipse(FX, FY, side * 33, 18, 22, 12, angle_deg=side * 12)
        brow = (FY - (24 + 0.45 * (side * FX - 33))) * 0.9  # cuts the top, falling to the nose
        eyes = np.minimum(eyes, np.maximum(e, brow))
    top, bottom = -56 - 0.005 * FX ** 2, -70 - 0.002 * FX ** 2
    mouth = np.maximum(np.maximum(FY - top, bottom - FY), np.abs(FX) - 40)
    return np.maximum(coverage(eyes, 4.0), coverage(mouth, 4.0))


def _layout():
    """Pixel -> glyph column, column within the glyph, virtual row (the slit counts as rows)."""
    x = np.arange(W)
    col = np.clip((x - X0) // CELL_W, 0, COLS - 1)
    cx = (x - X0) - col * CELL_W
    valid_x = (x >= X0) & (cx < GW)
    gap = np.round(gap_rows(X0 + col * CELL_W + GW / 2)).astype(int)
    y = np.arange(H)[:, None]
    v = np.where(y < 64, y, y + gap[None, :])
    cell = v // CELL_H
    ry = v % CELL_H
    lit_px = valid_x[None, :] & (ry < GH)
    return col, cx, v, cell, ry, lit_px, gap


def render(n, fps, rng):
    glyphs = make_glyphs(rng)
    face = face_map()
    col, cx, v, cell, ry, lit_px, gap = _layout()
    cells = (H + gap.max()) // CELL_H + 2

    # drops: head position (virtual rows) periodic in n frames
    # a cycle is longer than the column (with its trail), so there are pauses between drops
    min_period = cells * CELL_H + 48
    speed = rng.uniform(0.7, 1.6, (COLS, DROPS_PER_COLUMN))       # rows per frame
    cycles = np.maximum(1, np.floor(speed * n / min_period))
    period = np.round(speed * n / cycles)                          # rows per cycle
    speed = period * cycles / n
    phase = rng.uniform(0, 1, (COLS, DROPS_PER_COLUMN)) * period
    trail = rng.uniform(*TRAIL_ROWS, (COLS, DROPS_PER_COLUMN)) / CELL_H  # cells

    # glyph changes: each cell changes every `every` frames (divides n), at its own offset
    divisors = [d for d in range(4, n + 1) if n % d == 0 and d <= 50] or [n]
    every = rng.choice(divisors, (COLS, cells))
    offset = rng.integers(0, n, (COLS, cells))
    seeds = rng.integers(0, 1 << 30, (COLS, cells))

    cell_idx = np.arange(cells)[None, :]
    for k in range(n):
        bright = np.full((COLS, cells), 0.03)
        head = np.zeros((COLS, cells), dtype=bool)
        for d in range(DROPS_PER_COLUMN):
            pos = (phase[:, d] + speed[:, d] * k) % period[:, d]    # virtual row of the head
            hc = (pos // CELL_H)[:, None]
            dist = hc - cell_idx                                   # cells above the head (>0)
            b = np.where(dist >= 0, np.exp(-dist / trail[:, d, None]), 0.0)
            bright = np.maximum(bright, b)
            head |= dist == 0
        epoch = (k + offset) // every
        gid = (seeds * 2654435761 + epoch * 40503) % len(glyphs)
        # gather per pixel
        cg = np.broadcast_to(col[None, :], (H, W))
        cc = np.clip(cell, 0, cells - 1)
        pix = glyphs[gid[cg, cc], np.minimum(ry, GH - 1), np.minimum(cx, GW - 1)[None, :]] * lit_px
        b = bright[cg, cc]
        is_head = head[cg, cc]
        boost = 0.18 + 0.82 * face
        level = np.where(is_head, 0.8, np.maximum(b * boost, 0.4 * face)) * pix
        colour = np.where(is_head[..., None], HEAD, GREEN)
        yield level[..., None] * colour


def render_intro(n, fps, rng, loop):
    """The loop's frames revealed from the top by a curtain of falling heads (one per column)."""
    col, cx, v, cell, ry, lit_px, gap = _layout()
    speed = rng.uniform(3.0, 5.0, COLS)[col]             # rows per frame
    delay = rng.uniform(0, 0.25 * fps, COLS)[col]        # frames
    for k in range(n):
        front = (k - delay) * speed                      # virtual row of the curtain per pixel column
        f = loop[(len(loop) - n + k) % len(loop)]
        shown = (v < front[None, :]).astype(np.float32)
        head = (v < front[None, :]) & (v >= front[None, :] - CELL_H) & lit_px
        yield f * shown[..., None] + head[..., None] * HEAD * 0.9


# --- staff ---

STAFF_DROPS = 3      # per side
STAFF_TRAIL = (8.0, 22.0)   # heights


def _staff_drops(n, rng, count):
    """Periodic drop positions: speed (heights per frame), period, phase for count drops."""
    min_period = staffled.BODY + 30
    speed = rng.uniform(0.45, 1.1, count)
    cycles = np.maximum(1, np.floor(speed * n / min_period))
    period = np.round(speed * n / cycles)
    return period * cycles / n, period, rng.uniform(0, 1, count) * period


def render_staff(n, fps, rng):
    hs = staffled.heights()
    speed, period, phase = _staff_drops(n, rng, 2 * STAFF_DROPS)
    trail = rng.uniform(*STAFF_TRAIL, 2 * STAFF_DROPS)
    # glyph flicker: every LED changes its brightness every `every` frames (a divisor of n)
    divisors = [d for d in range(3, n + 1) if n % d == 0 and d <= 25] or [n]
    every = rng.choice(divisors, (staffled.BODY, 2))
    offset = rng.integers(0, n, (staffled.BODY, 2))
    seeds = rng.integers(0, 1 << 30, (staffled.BODY, 2))
    for k in range(n):
        epoch = (k + offset) // every
        flicker = 0.45 + 0.55 * (((seeds * 2654435761 + epoch * 40503) % 1000) / 1000.0)
        frame = np.zeros((staffled.BODY, 2, 3))
        for side in (0, 1):
            level = np.full(staffled.BODY, 0.02)
            head = np.zeros(staffled.BODY, dtype=bool)
            for d in range(side * STAFF_DROPS, (side + 1) * STAFF_DROPS):
                # the drop falls from above the crystal to below the bottom
                pos = staffled.BODY + 10 - (phase[d] + speed[d] * k) % period[d]
                above = hs - np.round(pos)
                level = np.maximum(level, np.where(above > 0, np.exp(-above / trail[d] * 2.5), 0))
                head |= above == 0
            frame[:, side] = (level * flicker[:, side])[:, None] * GREEN
            frame[head, side] = HEAD
        yield frame


def render_staff_intro(n, fps, rng, loop):
    """The loop revealed from the crystal down by a bright front."""
    hs = staffled.heights()[:, None, None]
    speed = staffled.BODY / (0.9 * fps)                  # down in 0.9 s
    for k in range(n):
        front = staffled.BODY - k * speed
        f = loop[(len(loop) - n + k) % len(loop)]
        yield f * (hs >= front) + HEAD * np.exp(-np.abs(hs - front) / 1.5) * (hs >= front - 1)
