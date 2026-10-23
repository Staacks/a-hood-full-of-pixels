"""Rainbow fire skull (fun playlist): the fire skull, but every flame particle has a colour of
the rainbow. The hue is given at the particle's birth (bands across the skull that drift over
time), so neighbouring particles share a colour and the flame tongues carry it upwards.

Intro: the fire skull's fireball, the explosion's particles coloured by their direction. Staff:
the staff fire in rainbow colours; intro: a white fireball shooting up, leaving rainbow flames.
"""

import numpy as np

import staffled
from maskvid import sample_canvas
from scenes import fireskull

SECONDS = 6.0
INTRO_SECONDS = 1.6
STAFF_SECONDS = 6.0
STAFF_INTRO_SECONDS = 1.2

CYCLES = 2         # hue turns per loop (whole turns: the loop's crossfade joins equal colours)
BAND_X_MM = 200.0  # one rainbow across this distance
BAND_Y_MM = 250.0
STAFF_BAND = 50.0  # heights per rainbow on the staff
LEVEL = 0.85


def rainbow(hue):
    """Hue (any real, 1 = full turn) -> saturated linear RGB."""
    h = (np.asarray(hue) % 1.0) * 6
    rgb = np.stack([np.clip(np.abs(h - 3) - 1, 0, 1), np.clip(2 - np.abs(h - 2), 0, 1),
                    np.clip(2 - np.abs(h - 4), 0, 1)], axis=-1)
    return rgb ** 2.2  # the ramps are perceptual; the panels and the strip want linear light


def shade(heat, rgb):
    """Heat + heat-weighted colour sum -> linear RGB: coloured flames with white-hot cores."""
    colour = rgb / np.maximum(heat, 1e-6)[..., None]
    level = np.clip(heat * 1.8, 0, 1) ** 1.8 * LEVEL
    core = np.clip((heat - 0.8) * 1.2, 0, 1) ** 2 * 0.5
    return colour * level[..., None] + core[..., None]


def _fire(rng, fps):
    fire = fireskull.Fire(rng, fps)
    fire.hue = lambda x, y, t: x / BAND_X_MM + y / BAND_Y_MM - t * CYCLES / SECONDS
    return fire


def render(n, fps, rng):
    fire = _fire(rng, fps)
    for _ in range(int(fireskull.WARMUP_S * fps)):
        fire.step()
    fade = int(fireskull.CROSSFADE_S * fps)
    fields = []
    for _ in range(n + fade):
        fire.step()
        fields.append(fire.heat(rainbow))
    holes = fireskull._holes()[..., None]
    for k in range(n):
        h, c = fields[k]
        if k < fade:  # loop: frame n-1 -> frame 0 continues the simulation (n -> n + fade)
            a = k / fade
            h = fields[n + k][0] * (1 - a) + h * a
            c = fields[n + k][1] * (1 - a) + c * a
        yield sample_canvas(shade(h, c) * holes)


def render_intro(n, fps, rng, loop):
    fire = _fire(rng, fps)
    holes = fireskull._holes()[..., None]
    per_frame = int(fireskull.BURST_PARTICLES / max(1, round(fireskull.BURST_S * fps)))
    for k in range(n):
        t = k / fps
        if t < fireskull.BURST_S:
            fire.burst(fireskull.BURST_CENTRE, per_frame)
        fire.set_reach(fireskull.BURST_CENTRE, 30 + fireskull.SPREAD_MM_S * t)
        fire.step()
        yield sample_canvas(shade(*fire.heat(rainbow)) * holes)


def _staff_fire(rng, fps):
    fire = fireskull.StaffFire(rng, fps)
    fire.hue = lambda h, t: h / STAFF_BAND - t * CYCLES / STAFF_SECONDS
    return fire


def render_staff(n, fps, rng):
    fire = _staff_fire(rng, fps)
    for _ in range(int(fireskull.WARMUP_S * fps)):
        fire.step()
    fade = int(fireskull.CROSSFADE_S * fps)
    fields = [None] * (n + fade)
    for k in range(n + fade):
        fire.step()
        fields[k] = fire.heat(rainbow)
    for k in range(n):
        h, c = fields[k]
        if k < fade:
            a = k / fade
            h = fields[n + k][0] * (1 - a) + h * a
            c = fields[n + k][1] * (1 - a) + c * a
        yield shade(h, c).transpose(1, 0, 2)


def render_staff_intro(n, fps, rng, loop):
    """A white fireball shoots up from the bottom; rainbow flames catch behind it."""
    fire = _staff_fire(rng, fps)
    hs = staffled.heights()[:, None]
    for k in range(n):
        t = k / fps
        ball = fireskull.BALL_SPEED * t
        fire.reach = float(np.clip(ball, 1, staffled.BODY))
        fire.step()
        img = shade(*fire.heat(rainbow)).transpose(1, 0, 2)
        if ball < staffled.BODY + 8:
            img = img + (1.0 * np.exp(-((hs - ball) / 2.5) ** 2))[..., None]
        yield img
