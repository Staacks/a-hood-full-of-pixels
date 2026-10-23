"""Creeper (fun playlist): the Minecraft creeper's head, a square of mottled green pixels with
the black face. The wearer looks out at the bottom edge of its eyes. Every loop it starts to
swell and flash white like before an explosion, then calms down again.

Intro: the head's pixels fly in from all sides and assemble. Staff: green pixel blocks that
flash white together with the head (same loop and intro length, so they stay in step); intro:
the blocks fall from the top and stack up.
"""

import math

import numpy as np

import staffled
from maskvid import CANVAS_H, CANVAS_W, CX, CY, mm_to_canvas, sample_canvas

SECONDS = 8.0
INTRO_SECONDS = 1.6
STAFF_SECONDS = 8.0
STAFF_INTRO_SECONDS = 1.6

HALF = 96.0                 # mm, half the head's width and height (8 face pixels)
CELLS = 16                  # texture cells across (2 per face pixel)
CELL = 2 * HALF / CELLS
FACE = ["........",
        "........",
        ".XX..XX.",
        ".XX..XX.",
        "...XX...",
        "..XXXX..",
        "..XXXX..",
        "..X..X.."]
# texture greens (sRGB) and their weights; a few pale and dark cells as in the game
GREENS = np.array([[95, 190, 70], [75, 165, 55], [55, 135, 40], [40, 105, 30], [150, 205, 140], [25, 70, 20]])
WEIGHTS = np.array([0.24, 0.26, 0.2, 0.15, 0.07, 0.08])
LEVEL = 0.75
FACE_COLOUR = np.array([0.003, 0.008, 0.003])
FUSE = (4.4, 6.2)           # s: swelling and flashing
FLASHES = 4
WHITE = 0.5
TEXTURE_SEED = 7            # the same texture in loop and intro


def _texture(rng):
    """(CELLS, CELLS, 3) linear colours of the texture cells, the face cut in."""
    idx = rng.choice(len(GREENS), (CELLS, CELLS), p=WEIGHTS)
    tex = (GREENS[idx] / 255.0) ** 2.2 * LEVEL
    for r, line in enumerate(FACE):
        for c, ch in enumerate(line):
            if ch == "X":
                tex[2 * r:2 * r + 2, 2 * c:2 * c + 2] = FACE_COLOUR
    return tex


def _head(tex, scale=1.0):
    """Canvas image of the head, scaled about the centre."""
    x, y = CX / scale, CY / scale
    c = np.floor((x + HALF) / CELL).astype(int)
    r = np.floor((HALF - y) / CELL).astype(int)
    inside = (c >= 0) & (c < CELLS) & (r >= 0) & (r < CELLS)
    img = np.zeros(CX.shape + (3,))
    img[inside] = tex[r[inside], c[inside]]
    return img, inside


def fuse(t):
    """(swelling 0..1, white flash 0..1) at loop time t."""
    a, b = FUSE
    if t < a:
        return 0.0, 0.0
    if t < b:
        u = (t - a) / (b - a)
        flash = (1 - math.cos(2 * math.pi * FLASHES * u)) / 2 * (0.35 + 0.65 * u)
        return u, flash
    return max(0.0, 1 - (t - b) / 0.3), 0.0  # fizzles out


def render(n, fps, rng):
    tex = _texture(np.random.default_rng(TEXTURE_SEED))
    for k in range(n):
        swell, flash = fuse(k / fps)
        img, inside = _head(tex, 1 + 0.06 * swell)
        img = img * (1 - flash * WHITE) + (inside * flash * WHITE)[..., None] * 0.8
        yield sample_canvas(img)


def render_intro(n, fps, rng, loop):
    """Every texture cell flies in from outside (random direction and delay) to its place."""
    tex = _texture(np.random.default_rng(TEXTURE_SEED))
    cells = [(r, c) for r in range(CELLS) for c in range(CELLS)]
    ang = rng.uniform(0, 2 * math.pi, len(cells))
    dist = rng.uniform(160, 260, len(cells))
    delay = rng.uniform(0, 0.45, len(cells))
    size = int(round(CELL / (CX[0, 1] - CX[0, 0])))
    for k in range(n):
        t = k / fps
        img = np.zeros((CANVAS_H, CANVAS_W, 3))
        for i, (r, c) in enumerate(cells):
            u = min(1.0, max(0.0, (t - delay[i]) / 0.5))
            if u <= 0:
                continue
            ease = 1 - (1 - u) ** 3
            x0 = -HALF + c * CELL + (1 - ease) * dist[i] * math.cos(ang[i])
            y0 = HALF - r * CELL + (1 - ease) * dist[i] * math.sin(ang[i])
            col, row = mm_to_canvas(x0, y0)
            c0, r0 = int(round(col + 0.5)), int(round(row + 0.5))
            img[max(0, r0):max(0, r0 + size), max(0, c0):max(0, c0 + size)] = tex[r, c]
        yield sample_canvas(img)


# --- staff: pixel blocks ---

BLOCK = 4  # heights per block


def _staff_blocks(rng):
    n = staffled.BODY // BLOCK + 1
    idx = rng.choice(len(GREENS), (n, 2), p=WEIGHTS)
    return (GREENS[idx] / 255.0) ** 2.2 * LEVEL * 0.8


def _staff_image(blocks):
    hs = staffled.heights().astype(int)
    return blocks[hs // BLOCK]  # (BODY, 2, 3)


def render_staff(n, fps, rng):
    img = _staff_image(_staff_blocks(rng))
    for k in range(n):
        _, flash = fuse(k / fps)
        yield img * (1 - flash * WHITE) + flash * WHITE * 0.6


def render_staff_intro(n, fps, rng, loop):
    """The blocks fall from the top one after the other and stack up from the bottom."""
    blocks = (loop[-1] if loop else _staff_image(_staff_blocks(rng)))
    count = staffled.BODY // BLOCK + 1
    hs = staffled.heights()
    for k in range(n):
        t = k / fps
        img = np.zeros((staffled.BODY, 2, 3))
        for b in range(count):
            start = b * 1.0 / count                     # one block after the other within 1 s
            fall = max(0.0, t - start)
            top = staffled.BODY + BLOCK - 400.0 * fall * fall   # accelerates down
            bottom = max(b * BLOCK, top - BLOCK)
            if t < start:
                continue
            sel = (hs >= bottom) & (hs < bottom + BLOCK)
            src = min(b * BLOCK, staffled.BODY - 1)
            img[sel] = blocks[src]
        yield img
