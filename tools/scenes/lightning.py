"""Lightning skull: eye sockets, nose and teeth glow white-blue on black; lightning arcs crawl
along their edges, jump between them and spark out into the dark.

Arc ends sit on the feature outlines and slide along them (moved tangentially, then projected
back onto the outline with the signed distance field). The bolt between them is re-jagged every
frame. Bolts are random every frame, so the loop seam is invisible; the features are static.

Intro: a bolt strikes the skull from above with a flash, the features charge up, then the arcs
take over. Staff: dim blue glow with bolts flickering along parts of the staff; its intro is a
strike from the crystal to the ground.
"""

import math

import numpy as np
from scipy import ndimage

import staffled
from maskvid import CANVAS_H, CANVAS_W, CX, CY, coverage, mm_to_canvas, sample_canvas
from scenes import skull

SECONDS = 6.0
INTRO_SECONDS = 1.4
STAFF_SECONDS = 6.0
STAFF_INTRO_SECONDS = 1.0

ARCS = 5
ARC_LIFE_S = (0.25, 1.0)
SLIDE_MM_S = (40.0, 120.0)
FEATURE = np.array([0.55, 0.75, 1.0])
GLOW = np.array([0.10, 0.35, 1.0])
BOLT = np.array([0.80, 0.88, 1.0])


class Outline:
    """Signed distance field of the features, sampled at arbitrary points."""

    def __init__(self):
        self.d = skull.features_sdf(CX, CY)
        gy, gx = np.gradient(self.d)
        # canvas rows grow downwards: dY = -drow
        self.gx, self.gy = gx, -gy
        edge = np.abs(self.d) < 0.8
        self.edge_pts = np.stack([CX[edge], CY[edge]], axis=1)

    def _at(self, field, p):
        c, r = mm_to_canvas(p[0], p[1])
        return float(ndimage.map_coordinates(field, [[r], [c]], order=1, mode="nearest")[0])

    def sdf(self, p):
        return self._at(self.d, p)

    def project(self, p):
        for _ in range(3):
            d, g = self._at(self.d, p), np.array([self._at(self.gx, p), self._at(self.gy, p)])
            g /= max(np.hypot(*g), 1e-6)
            p = p - d * g
        return p

    def slide(self, p, dist):
        g = np.array([self._at(self.gx, p), self._at(self.gy, p)])
        g /= max(np.hypot(*g), 1e-6)
        return self.project(p + dist * np.array([-g[1], g[0]]))

    def normal(self, p):
        g = np.array([self._at(self.gx, p), self._at(self.gy, p)])
        return g / max(np.hypot(*g), 1e-6)


def bolt_points(a, b, rng, rough=0.3, levels=4):
    pts = np.array([a, b])
    for lvl in range(levels):
        mids = (pts[:-1] + pts[1:]) / 2
        seg = pts[1:] - pts[:-1]
        length = np.hypot(seg[:, 0], seg[:, 1])[:, None]
        normal = np.stack([-seg[:, 1], seg[:, 0]], axis=1) / np.maximum(length, 1e-6)
        mids += normal * rng.normal(0, rough, (len(mids), 1)) * length
        out = np.empty((len(pts) + len(mids), 2))
        out[0::2], out[1::2] = pts, mids
        pts = out
    return pts


def splat_polyline(field, pts, value):
    """Draws a polyline into the canvas field (densely sampled points)."""
    seg = np.diff(pts, axis=0)
    steps = np.maximum(1, (np.hypot(seg[:, 0], seg[:, 1]) / (0.4)).astype(int))
    xs = np.concatenate([np.linspace(p[0], p[0] + s[0], k, endpoint=False) for p, s, k in zip(pts[:-1], seg, steps)])
    ys = np.concatenate([np.linspace(p[1], p[1] + s[1], k, endpoint=False) for p, s, k in zip(pts[:-1], seg, steps)])
    c, r = mm_to_canvas(xs, ys)
    c, r = np.round(c).astype(int), np.round(r).astype(int)
    ok = (c >= 0) & (c < CANVAS_W) & (r >= 0) & (r < CANVAS_H)
    field[r[ok], c[ok]] = np.maximum(field[r[ok], c[ok]], value)


class Arc:
    """bridge: between two outline points through the dark; crawl: hugs an outline, bulging
    outwards, and travels along it; spark: from an outline out into the dark."""

    def __init__(self, outline, rng, t):
        self.o, self.rng = outline, rng
        self.a = outline.edge_pts[rng.integers(len(outline.edge_pts))].copy()
        r = rng.random()
        self.kind = "bridge" if r < 0.35 else "crawl" if r < 0.75 else "spark"
        self.va = rng.uniform(*SLIDE_MM_S) * rng.choice([-1, 1])
        self.vb = rng.uniform(*SLIDE_MM_S) * rng.choice([-1, 1])
        if self.kind == "bridge":
            cand = outline.edge_pts[rng.integers(len(outline.edge_pts), size=400)]
            d = np.hypot(*(cand - self.a).T)
            mids = (cand + self.a) / 2
            clear = np.array([outline.sdf(m) for m in mids])
            ok = np.nonzero((d > 15) & (d < 75) & (clear > 2.5))[0]
            if len(ok):
                self.b = cand[rng.choice(ok)].copy()
            else:
                self.kind = "crawl"
        if self.kind == "crawl":
            self.vb = self.va  # both ends travel together around the outline
            self.b = outline.slide(self.a, rng.uniform(15, 35) * rng.choice([-1, 1]))
        if self.kind == "spark":
            self.b = self.a + outline.normal(self.a) * rng.uniform(15, 45) + rng.normal(0, 8, 2)
        self.until = t + rng.uniform(*ARC_LIFE_S)

    def step(self, dt):
        self.a = self.o.slide(self.a, self.va * dt)
        if self.kind == "spark":
            self.b = self.b + self.o.normal(self.a) * 0.0 + self.rng.normal(0, 3, 2)
        else:
            self.b = self.o.slide(self.b, self.vb * dt)

    def draw(self, field):
        intensity = self.rng.uniform(0.5, 1.0)
        # start just outside the outline, so the bolt is in the dark
        a = self.a + self.o.normal(self.a) * 1.5
        b = self.b + (self.o.normal(self.b) * 1.5 if self.kind != "spark" else 0)
        if self.kind == "crawl":
            mid = self.o.project((self.a + self.b) / 2)
            c = mid + self.o.normal(mid) * self.rng.uniform(5, 10)
            pts = np.vstack([bolt_points(a, c, self.rng), bolt_points(c, b, self.rng)[1:]])
        else:
            pts = bolt_points(a, b, self.rng)
        splat_polyline(field, pts, intensity)
        # a short fork now and then
        if self.rng.random() < 0.4:
            i = self.rng.integers(len(pts) // 4, 3 * len(pts) // 4)
            d = pts[-1] - pts[0]
            end = pts[i] + np.array([d[1], -d[0]]) * self.rng.uniform(-0.4, 0.4) + d * 0.2
            splat_polyline(field, bolt_points(pts[i], end, self.rng, levels=3), intensity * 0.6)
        return intensity


def _image(features, halo, bolts, pulse, charge=1.0):
    glow = ndimage.gaussian_filter(bolts, 3.0) * 3.0
    core = bolts * 2.5  # thin: the sampling blur spreads it over about one LED
    return (features[..., None] * FEATURE * pulse * charge
            + halo[..., None] * GLOW * 0.45 * pulse * charge
            + glow[..., None] * GLOW
            + core[..., None] * BOLT)


def render(n, fps, rng):
    outline = Outline()
    features = coverage(outline.d, soft=0.7)
    halo = np.exp(-np.maximum(outline.d, 0) / 3.0) * (outline.d > 0)
    dt = 1.0 / fps
    arcs = []
    for k in range(n):
        t = k * dt
        arcs = [a for a in arcs if a.until > t]
        while len(arcs) < ARCS:
            arcs.append(Arc(outline, rng, t))
        bolts = np.zeros((CANVAS_H, CANVAS_W))
        energy = 0.0
        for a in arcs:
            a.step(dt)
            energy += a.draw(bolts)
        pulse = 0.8 + 0.12 * math.sin(t * 2 * math.pi / 3.0) + 0.08 * energy / ARCS
        yield sample_canvas(_image(features, halo, bolts, pulse))


STRIKE_S = 0.35
FLASH = np.array([0.35, 0.45, 0.7])


def render_intro(n, fps, rng, loop):
    outline = Outline()
    features = coverage(outline.d, soft=0.7)
    halo = np.exp(-np.maximum(outline.d, 0) / 3.0) * (outline.d > 0)
    # the strike hits the top of an eye socket (its brow) from above the mask
    top = outline.edge_pts[np.argsort(outline.edge_pts[:, 1])[-40:]]
    target = top[rng.integers(len(top))]
    start = np.array([target[0] + rng.uniform(-40, 40), 140.0])
    dt = 1.0 / fps
    arcs = []
    for k in range(n):
        t = k * dt
        bolts = np.zeros((CANVAS_H, CANVAS_W))
        if t < STRIKE_S and rng.random() < 0.85:  # flickering main bolt with forks
            pts = bolt_points(start, target, rng, rough=0.25, levels=6)
            splat_polyline(bolts, pts, 1.0)
            for _ in range(3):
                i = rng.integers(len(pts) // 5, 4 * len(pts) // 5)
                end = pts[i] + rng.normal(0, 25, 2) + np.array([0, -20])
                splat_polyline(bolts, bolt_points(pts[i], end, rng, levels=4), 0.6)
        if t > 0.15:  # the charged skull starts arcing, more than in the loop at first
            arcs = [a for a in arcs if a.until > t]
            while len(arcs) < ARCS * 2 - int(ARCS * min(1.0, t / INTRO_SECONDS)):
                arcs.append(Arc(outline, rng, t))
            for a in arcs:
                a.step(dt)
                a.draw(bolts)
        charge = min(1.0, max(0.0, (t - 0.03) / 0.35)) * (1 + 0.6 * math.exp(-max(0.0, t - 0.35) / 0.2))
        img = _image(features, halo, bolts, 0.9, charge)
        img += FLASH * 0.6 * math.exp(-t / 0.1)
        yield sample_canvas(img)


# --- staff ---

STAFF_BOLTS_PER_S = 2.5
STAFF_GLOW = np.array([0.004, 0.012, 0.05])


def _staff_glow(n, k, hs):
    """Dim blue base with slow waves, periodic in n frames."""
    w = (np.sin(2 * math.pi * (hs / 30.0 - 2 * k / n)) + np.sin(2 * math.pi * (hs / 13.0 + 3 * k / n))) * 0.25 + 0.6
    return np.stack([w, w[::-1]], axis=1)[..., None] * STAFF_GLOW


def render_staff(n, fps, rng):
    hs = staffled.heights()
    events = []  # (until frame, low, high, side mask)
    for k in range(n):
        if rng.random() < STAFF_BOLTS_PER_S / fps:
            length = rng.integers(10, 50)
            lo = rng.integers(0, staffled.BODY - length // 2)
            events.append((k + rng.integers(2, 6), lo, lo + length, rng.choice([0, 1, 2])))
        events = [e for e in events if e[0] > k]
        frame = _staff_glow(n, k, hs)
        for _, lo, hi, sides in events:
            if rng.random() < 0.25:
                continue  # flicker
            jag = rng.uniform(0.3, 1.0, staffled.BODY) * ((hs >= lo) & (hs < hi))
            jag = np.maximum(jag, ndimage.gaussian_filter1d(jag, 2.0) * 0.8)
            for side in ((0, 1) if sides == 2 else (sides,)):
                frame[:, side] += jag[:, None] * BOLT
        # single sparks
        for _ in range(rng.poisson(1.5)):
            frame[rng.integers(staffled.BODY), rng.integers(2)] += BOLT * 0.6
        yield frame


def render_staff_intro(n, fps, rng, loop):
    """A strike runs from the crystal to the ground, the staff flashes and flickers out."""
    hs = staffled.heights()[:, None, None]
    for k in range(n):
        t = k / fps
        front = staffled.BODY * (1 - t / 0.12)
        flash = math.exp(-max(0.0, t - 0.12) / 0.15) if t > 0.12 else 0.0
        restrike = 0.7 if (0.3 < t < 0.36 or 0.5 < t < 0.54) else 0.0
        level = np.where(hs >= front, 1.0, 0.0) if t < 0.12 else max(flash, restrike)
        jag = rng.uniform(0.6, 1.0, (staffled.BODY, 2, 1))
        f = loop[(len(loop) - n + k) % len(loop)]
        yield f + level * jag * BOLT
