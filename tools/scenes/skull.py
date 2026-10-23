"""Skull features in frontal mm (shared by the fire and lightning skulls).

The wearer looks through the eye slit at about X = +-32 mm, Y = 0, so the eye sockets sit
around the slit: the wearer sees through the sockets and the dark slit disappears in them.
"""

import numpy as np

from maskvid import FX, sdf_box, sdf_ellipse, sdf_polygon

EYE_X = 33.0
EYE_Y = 9.0


def socket_sdf(x, y):
    """Both eye sockets: rounded, with a slanted brow (menacing)."""
    d = np.full(np.shape(x), np.inf)
    for s in (-1, 1):
        e = sdf_ellipse(x, y, s * EYE_X, EYE_Y, 23.0, 21.0, angle_deg=-s * 8)
        # brow: cut the top along a line falling towards the nose
        brow = (y - (EYE_Y + 13.0 + 0.28 * (s * x - EYE_X)))  # > 0 above the line
        d = np.minimum(d, np.maximum(e, brow))
    return d


def nose_sdf(x, y):
    """Nasal opening: a narrow, pointed pear (widest near the bottom, flat base), split by a
    septum into two angular nostrils. X = 0 lies between two LED columns, so the septum covers
    both (narrower would vanish between the LEDs)."""
    right = [(0.0, -20.0), (2.4, -24.5), (5.0, -30.5), (7.5, -36.5), (9.0, -41.0), (8.5, -44.5),
             (6.0, -46.2), (2.0, -46.5)]
    pts = right + [(-px, py) for px, py in reversed(right[1:])]
    aperture = sdf_polygon(x, y, pts)
    septum = sdf_box(x, y, 0.0, -40.0, 1.5, 7.0)  # from the base up to about two thirds
    return np.maximum(aperture, -septum)


# Teeth stand on a curved dental arch; seen from the front, equal teeth get narrower towards the
# sides (projected width ~ cos of the arch angle). The gaps are narrower than the LED pitch, so
# they show as soft dark lines; each is centred on an LED column, which then dims the most (a gap
# between two columns would hardly show). An odd number of teeth keeps that symmetric (X = 0 lies
# between two columns). ARCH_R and ARCH_DEG are chosen so the arch's tooth edges already fall
# within 0.4 mm of LED columns; otherwise the snapping makes the tooth widths uneven.
TEETH = 9                 # per row
ARCH_R = 31.25            # mm, lateral radius of the arch (projected span +-ARCH_R*sin(ARCH_DEG))
ARCH_DEG = 78.0           # arch angle of the outermost tooth edge
BITE_Y, ROW_H = -70.0, 11.0  # straight bite line between the rows
TOOTH_GAP = ROW_GAP = 1.6  # mm
_COLUMNS_X = FX[0]        # frontal X of every LED column (the same in all rows)


def _tooth_spans(gap=TOOTH_GAP):
    """(left, right) frontal X of each tooth in a row; the gaps centred on LED columns."""
    a = np.radians(np.linspace(-ARCH_DEG, ARCH_DEG, TEETH + 1))
    edge = ARCH_R * np.sin(a)
    gap_x = _COLUMNS_X[np.argmin(np.abs(_COLUMNS_X[None, :] - edge[1:-1, None]), axis=1)]
    left = np.concatenate([[edge[0]], gap_x + gap / 2])
    right = np.concatenate([gap_x - gap / 2, [edge[-1]]])
    return list(zip(left, right))


def teeth_sdf(x, y, gaps=True):
    """Two rows of teeth (each tooth one shape, gaps between them). gaps=False: the whole block."""
    gap, row_gap = (TOOTH_GAP, ROW_GAP) if gaps else (0.0, 0.0)
    d = np.full(np.shape(x), np.inf)
    for row in (1, -1):  # upper, lower
        edge = BITE_Y + row * row_gap / 2  # constant gap along the bite line
        for x0, x1 in _tooth_spans(gap):
            cx, hw = (x0 + x1) / 2, (x1 - x0) / 2
            # outer teeth shorter (towards the gums): the jaw curves away
            h = ROW_H * (1.0 - 0.3 * (abs(cx) / ARCH_R) ** 2)
            # straight biting edge, rounded towards the gums
            r = min(hw * 1.2, h / 2)
            body = sdf_box(x, y, cx, edge + row * (h - r) / 2, hw, (h - r) / 2)
            tip = sdf_ellipse(x, y, cx, edge + row * (h - r), hw, r)
            d = np.minimum(d, np.minimum(body, tip))
    return d


def features_sdf(x, y):
    return np.minimum(np.minimum(socket_sdf(x, y), nose_sdf(x, y)), teeth_sdf(x, y))


def head_sdf(x, y):
    """Outline of the whole skull (cranium + jaw), for where the fire burns."""
    cranium = sdf_ellipse(x, y, 0.0, 20.0, 84.0, 98.0)
    cheeks = sdf_ellipse(x, y, 0.0, -25.0, 74.0, 60.0)
    jaw = sdf_box(x, y, 0.0, -78.0, 52.0, 24.0, radius=18.0)
    return np.minimum(np.minimum(cranium, cheeks), jaw)
