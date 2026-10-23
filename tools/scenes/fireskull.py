"""Burning skull: the skull is made of fire; eye sockets, nose and teeth are black holes in it.

Particle system on the frontal canvas: particles are emitted inside the skull outline, rise with
turbulence and cool down. Their heat is splatted into a field, blurred and mapped through a
fire palette. The holes are cut out afterwards, with a hot rim so they read clearly. The loop
is closed by crossfading the end of the simulation into its beginning.

Intro: a fireball explodes behind the nose and the fire spreads from there over the skull.
Staff: the same kind of particle fire along the staff (one column per side); its intro is a
fireball shooting up from the bottom that sets the staff alight on its way.
"""

import math

import numpy as np
from scipy import ndimage

import staffled
from maskvid import CANVAS_H, CANVAS_W, CX, CY, coverage, mm_to_canvas, sample_canvas
from scenes import skull

SECONDS = 6.0
INTRO_SECONDS = 1.6
STAFF_SECONDS = 6.0
STAFF_INTRO_SECONDS = 1.2
CROSSFADE_S = 1.0
WARMUP_S = 2.0

EMIT_PER_S = 40000
LIFE_S = (0.35, 0.9)
RISE_MM_S = (70.0, 140.0)
SPOTS = 40
SPOT_LIFE_S = (0.4, 1.4)
TEETH_GAP_GLOW = 0.15   # brightness of the fire seen through the gaps between the teeth

BURST_CENTRE = (0.0, -35.0)   # mm, behind the nose
BURST_PARTICLES = 30000
BURST_S = 0.12
BURST_MM_S = 700.0
SPREAD_MM_S = 450.0           # how fast the fire spreads from the burst over the skull


def palette(heat):
    """Heat (0..~1.5) -> linear RGB: black, dark red, orange, yellow, white."""
    h = np.clip(heat, 0, None)
    r = np.clip(h * 2.2, 0, 1)
    g = np.clip((h - 0.30) * 1.6, 0, 1) ** 1.6
    b = np.clip((h - 0.85) * 1.5, 0, 1) ** 2
    return np.stack([r, g * 0.85, b * 0.6], axis=-1) ** 1.8


class Fire:
    def __init__(self, rng, fps):
        self.rng = rng
        self.dt = 1.0 / fps
        self.t = 0.0
        self.p = np.zeros((0, 8))  # x, y, vx, vy, age, life, burst (1: explosion particle), hue
        self.hue = None            # hue(x, y, t) of new flame particles (rainbow fire)
        # emission density: inside the head, more towards the bottom of the features
        head = coverage(skull.head_sdf(CX, CY), soft=4.0)
        holes = skull.features_sdf(CX, CY)
        rim = np.exp(-np.maximum(holes, 0) / 4.0) * (holes > 0)   # around the holes
        self.density = (head * (0.6 + 1.6 * rim) * (holes > 0)).ravel()
        self.cdf = np.cumsum(self.density)
        self.cdf /= self.cdf[-1]
        self.emit_scale = 1.0
        self.holes = holes
        self.spots = np.zeros(SPOTS, dtype=int)
        self.spot_until = np.zeros(SPOTS)

    def _emit(self, n):
        # half spread evenly, half from hot spots that live for a while: their particles,
        # rising at different speeds, stretch into flame tongues
        dead = self.spot_until <= self.t
        self.spots[dead] = np.searchsorted(self.cdf, self.rng.random(dead.sum()))
        self.spot_until[dead] = self.t + self.rng.uniform(*SPOT_LIFE_S, dead.sum())
        n_even = n // 2
        idx = np.concatenate([np.searchsorted(self.cdf, self.rng.random(n_even)),
                              self.rng.choice(self.spots, n - n_even)])
        n = len(idx)
        x = CX.ravel()[idx] + self.rng.normal(0, 3.0, n)
        y = CY.ravel()[idx] + self.rng.normal(0, 1.5, n)
        vy = self.rng.uniform(*RISE_MM_S, n)
        vx = self.rng.normal(0, 8, n)
        life = self.rng.uniform(*LIFE_S, n)
        hue = self.hue(x, y, self.t) if self.hue else np.zeros(n)
        self.p = np.vstack([self.p, np.stack([x, y, vx, vy, np.zeros(n), life, np.zeros(n), hue], axis=1)])

    def set_reach(self, centre, radius):
        """Intro: emit only within radius (mm) of centre, as many particles as that area gets."""
        d = np.hypot(CX - centre[0], CY - centre[1]).ravel()
        dens = self.density * np.clip((radius - d) / 15.0 + 1, 0, 1)
        total = dens.sum()
        self.emit_scale = total / self.density.sum()
        if total > 0:
            self.cdf = np.cumsum(dens) / total

    def burst(self, centre, n):
        """Explosion: particles flying out of centre in all directions, slowed down by drag. Their
        hue follows the direction (a rainbow explosion for the rainbow fire)."""
        a = self.rng.uniform(0, 2 * math.pi, n)
        v = BURST_MM_S * self.rng.uniform(0.15, 1.0, n) ** 0.5
        x = centre[0] + self.rng.normal(0, 4, n)
        y = centre[1] + self.rng.normal(0, 4, n)
        life = self.rng.uniform(0.3, 0.8, n)
        self.p = np.vstack([self.p, np.stack([x, y, v * np.cos(a), v * np.sin(a), np.zeros(n), life, np.ones(n),
                                              a / (2 * math.pi)], axis=1)])

    def step(self):
        dt, t = self.dt, self.t
        self._emit(self.rng.poisson(EMIT_PER_S * dt * self.emit_scale))
        x, y = self.p[:, 0], self.p[:, 1]
        # turbulence: a few travelling waves, sideways sway grows with height above the emitter
        sway = (np.sin(y * 0.09 - t * 7.0 + np.sin(x * 0.05 + t)) * 40
                + np.sin(y * 0.21 + x * 0.07 - t * 11.0) * 25)
        flame = self.p[:, 6] == 0
        self.p[flame, 2] += (sway[flame] - self.p[flame, 2]) * min(1.0, dt * 6)
        self.p[~flame, 2:4] *= math.exp(-4.0 * dt)  # explosion particles: drag
        self.p[:, 3] += 60 * dt  # buoyancy
        self.p[:, 0] += self.p[:, 2] * dt
        self.p[:, 1] += self.p[:, 3] * dt
        self.p[:, 4] += dt
        self.p = self.p[self.p[:, 4] < self.p[:, 5]]
        self.t += dt

    def heat(self, colour=None):
        """Heat field; with colour(hue) -> (n, 3) also the heat-weighted colour sum (H, W, 3)."""
        col, row = mm_to_canvas(self.p[:, 0], self.p[:, 1])
        ci, ri = np.round(col).astype(int), np.round(row).astype(int)
        ok = (ci >= 0) & (ci < CANVAS_W) & (ri >= 0) & (ri < CANVAS_H)
        age = self.p[ok, 4] / self.p[ok, 5]
        w = (1 - age) ** 1.5
        field = np.zeros((CANVAS_H, CANVAS_W))
        np.add.at(field, (ri[ok], ci[ok]), w)
        heat = ndimage.gaussian_filter(field, 2.6) * 2.6
        if colour is None:
            return heat
        rgb = np.zeros((CANVAS_H, CANVAS_W, 3))
        c = colour(self.p[ok, 7]) * w[:, None]
        for ch in range(3):
            np.add.at(rgb[..., ch], (ri[ok], ci[ok]), c[:, ch])
            rgb[..., ch] = ndimage.gaussian_filter(rgb[..., ch], 2.6) * 2.6
        return heat, rgb


def _holes():
    holes = coverage(-skull.features_sdf(CX, CY), soft=0.7)       # 1 outside the holes, 0 inside
    # the gaps between the teeth only glow dimly, so the teeth read as one dark row
    return holes * (1 - (1 - TEETH_GAP_GLOW) * coverage(skull.teeth_sdf(CX, CY, gaps=False), soft=0.7))


def render(n, fps, rng):
    fire = Fire(rng, fps)
    for _ in range(int(WARMUP_S * fps)):
        fire.step()
    fade = int(CROSSFADE_S * fps)
    heats = []
    for _ in range(n + fade):
        fire.step()
        heats.append(fire.heat())
    holes = _holes()
    for k in range(n):
        h = heats[k]
        if k < fade:  # loop: frame n-1 -> frame 0 continues the simulation (n -> n + fade)
            a = k / fade
            h = heats[n + k] * (1 - a) + h * a
        img = palette(h) * holes[..., None]
        yield sample_canvas(img)


def render_intro(n, fps, rng, loop):
    fire = Fire(rng, fps)
    holes = _holes()
    per_frame = int(BURST_PARTICLES / max(1, round(BURST_S * fps)))
    for k in range(n):
        t = k / fps
        if t < BURST_S:
            fire.burst(BURST_CENTRE, per_frame)
        fire.set_reach(BURST_CENTRE, 30 + SPREAD_MM_S * t)
        fire.step()
        yield sample_canvas(palette(fire.heat()) * holes[..., None])


# --- staff: one column of particle fire per side ---

STAFF_EMIT_PER_S = 900          # per side
STAFF_LIFE_S = (0.2, 0.5)
STAFF_RISE = (15.0, 35.0)      # heights per second
STAFF_SPOTS = 6                 # per side
STAFF_HEAT = 0.38
BALL_SPEED = 160.0              # heights per second (intro fireball)


class StaffFire:
    def __init__(self, rng, fps):
        self.rng = rng
        self.dt = 1.0 / fps
        self.t = 0.0
        self.p = np.zeros((0, 6))  # side, height, speed, age, life, hue
        self.hue = None            # hue(height, t) of new particles (rainbow fire)
        self.reach = float(staffled.BODY)
        self.spots = np.zeros((2, STAFF_SPOTS))
        self.spot_until = np.zeros((2, STAFF_SPOTS))

    def _heights(self, n):
        # more fire at the bottom, flames reaching up to the crystal
        h = staffled.BODY * (1 - np.sqrt(self.rng.random(n)) * 0.999) - 0.5
        return h * (self.reach / staffled.BODY)

    def emit(self, n):
        for side in (0, 1):
            dead = self.spot_until[side] <= self.t
            self.spots[side, dead] = self._heights(dead.sum())
            self.spot_until[side, dead] = self.t + self.rng.uniform(0.15, 0.4, dead.sum())
            k = self.rng.poisson(n * self.reach / staffled.BODY)
            h = np.where(self.rng.random(k) < 0.5, self._heights(k), self.rng.choice(self.spots[side], k))
            h += self.rng.normal(0, 0.4, k)
            hue = self.hue(h, self.t) if self.hue else np.zeros(k)
            new = np.stack([np.full(k, side), h, self.rng.uniform(*STAFF_RISE, k), np.zeros(k),
                            self.rng.uniform(*STAFF_LIFE_S, k), hue], axis=1)
            self.p = np.vstack([self.p, new])

    def step(self):
        self.emit(STAFF_EMIT_PER_S * self.dt)
        self.p[:, 1] += self.p[:, 2] * self.dt
        self.p[:, 2] += 8 * self.dt  # buoyancy
        self.p[:, 3] += self.dt
        self.p = self.p[self.p[:, 3] < self.p[:, 4]]
        self.t += self.dt

    def heat(self, colour=None):
        """Heat (2, BODY); with colour(hue) -> (n, 3) also the heat-weighted colour sum (2, BODY, 3)."""
        side = self.p[:, 0].astype(int)
        h = np.clip(self.p[:, 1], 0, staffled.BODY)
        lo = np.floor(h).astype(int)
        f = h - lo
        w = (1 - self.p[:, 3] / self.p[:, 4]) ** 1.5

        def splat(weight):
            field = np.zeros((2, staffled.BODY + 2))
            np.add.at(field, (side, lo), weight * (1 - f))
            np.add.at(field, (side, lo + 1), weight * f)
            return ndimage.gaussian_filter1d(field[:, :staffled.BODY], 0.8, axis=1) * STAFF_HEAT

        if colour is None:
            return splat(w)
        c = colour(self.p[:, 5])
        return splat(w), np.stack([splat(w * c[:, ch]) for ch in range(3)], axis=-1)


def _staff_frame(heat):
    return palette(heat).transpose(1, 0, 2)  # (2, BODY, 3) -> (BODY, 2, 3)


def render_staff(n, fps, rng):
    fire = StaffFire(rng, fps)
    for _ in range(int(WARMUP_S * fps)):
        fire.step()
    fade = int(CROSSFADE_S * fps)
    heats = []
    for _ in range(n + fade):
        fire.step()
        heats.append(fire.heat())
    for k in range(n):
        h = heats[k]
        if k < fade:
            a = k / fade
            h = heats[n + k] * (1 - a) + h * a
        yield _staff_frame(h)


def render_staff_intro(n, fps, rng, loop):
    """A fireball shoots up from the bottom; the staff catches fire behind it."""
    fire = StaffFire(rng, fps)
    hs = staffled.heights()[None, :]
    for k in range(n):
        t = k / fps
        ball = BALL_SPEED * t
        fire.reach = float(np.clip(ball, 1, staffled.BODY))
        fire.step()
        heat = fire.heat()
        if ball < staffled.BODY + 8:
            heat = heat + 2.2 * np.exp(-((hs - ball) / 2.5) ** 2) + 0.8 * np.exp(-np.clip(ball - hs, 0, None) / 6.0) \
                * (hs < ball)
        yield _staff_frame(heat)
