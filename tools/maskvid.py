"""Mask geometry, frontal canvas, .vid files and previews (see docs/mask_geometry.md).

Content is drawn on a *frontal canvas*: a flat image of the mask as seen from the front, in mm
(X right, Y up, origin in the middle of the eye slit). Every panel pixel then samples the canvas
at its frontal position, so the lens-shaped eye slit, the bowed rows and the compressed outer
columns come out right without the content having to know about them.

Logical frame: 128x128, rows 0-63 = upper panel U, rows 64-127 = lower panel L (both with row 0
at the top), column 0 on the viewer's left. Which connector drives which panel is a firmware
setting.

Colour: scenes render linear light (what the panel emits, 0..1). Files store gamma-encoded
RGB565; the firmware's gamma LUT (2.2) turns that back into linear PWM values.

    python3 tools/maskvid.py preview media/sd_display/eyes.vid eyes.mp4    # .vid -> frontal preview video
    python3 tools/maskvid.py geometry                              # print the reference table
"""

from __future__ import annotations

import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy import ndimage

# --- geometry (design values, docs/mask_geometry.md) ---

PITCH = 1.875          # mm
R = 145.0              # bend radius at the PCB
EYE_GAP = 20.0         # slit at the centre (mm)
PANEL_W, PANEL_H = 128, 64
W, H = 128, 128        # logical frame
GAMMA = 2.2

PHI_MAX = 120.0 / R
ZC = R * math.cos(PHI_MAX)
THETA = math.asin(EYE_GAP / 2 / (R - ZC))


def panel_u_positions() -> tuple[np.ndarray, np.ndarray]:
    """Frontal X, Y (mm) of the upper panel's pixels, shape (64, 128)."""
    c = np.arange(PANEL_W)[None, :]
    r = np.arange(PANEL_H)[:, None]
    u = (c - 63.5) * PITCH
    v = (31.5 - r) * PITCH + 60.0
    x = R * np.sin(u / R)
    z0 = R * np.cos(u / R) - ZC
    X = np.broadcast_to(x, (PANEL_H, PANEL_W))
    Y = v * math.cos(THETA) + z0 * math.sin(THETA)
    return X.astype(np.float64), Y.astype(np.float64)


def frame_positions() -> tuple[np.ndarray, np.ndarray]:
    """Frontal X, Y (mm) of every pixel of the logical 128x128 frame."""
    xu, yu = panel_u_positions()
    # L is the mirror image of U: L row r sits at (X, -Y) of U row 63 - r.
    xl, yl = xu[::-1], -yu[::-1]
    return np.vstack([xu, xl]), np.vstack([yu, yl])


FX, FY = frame_positions()


def gap_rows(c: np.ndarray | float) -> np.ndarray:
    """Slit between the panel edges in pixel rows, per column."""
    u = (np.asarray(c, dtype=np.float64) - 63.5) * PITCH
    return 2 * math.sin(THETA) * R * (np.cos(u / R) - math.cos(PHI_MAX)) / PITCH


# --- frontal canvas ---

CANVAS_MM = PITCH / 2               # canvas resolution: 2x oversampled
CANVAS_X0, CANVAS_Y0 = -112.5, 131.25  # top left corner (mm)
CANVAS_W = int(round(-2 * CANVAS_X0 / CANVAS_MM))  # 240
CANVAS_H = int(round(2 * CANVAS_Y0 / CANVAS_MM))   # 280


def canvas_grid() -> tuple[np.ndarray, np.ndarray]:
    """X, Y (mm) of every canvas pixel centre, shape (CANVAS_H, CANVAS_W)."""
    x = CANVAS_X0 + (np.arange(CANVAS_W) + 0.5) * CANVAS_MM
    y = CANVAS_Y0 - (np.arange(CANVAS_H) + 0.5) * CANVAS_MM
    return np.meshgrid(x, y)


CX, CY = canvas_grid()


def mm_to_canvas(x, y):
    """Canvas pixel coordinates (column, row; float) of a frontal position."""
    return (np.asarray(x) - CANVAS_X0) / CANVAS_MM - 0.5, (CANVAS_Y0 - np.asarray(y)) / CANVAS_MM - 0.5


_SAMPLE_COL, _SAMPLE_ROW = mm_to_canvas(FX, FY)


def sample_canvas(canvas: np.ndarray) -> np.ndarray:
    """Logical 128x128x3 frame from a frontal canvas (CANVAS_H x CANVAS_W x 3, linear light).

    Averages over roughly one pixel pitch (pre-blur), then samples at the pixel positions."""
    out = np.empty((H, W, 3))
    for ch in range(3):
        blurred = ndimage.gaussian_filter(canvas[..., ch], 0.8, mode="constant")
        out[..., ch] = ndimage.map_coordinates(blurred, [_SAMPLE_ROW, _SAMPLE_COL], order=1, mode="constant")
    return out


# --- shapes (signed distance in mm, negative inside) ---

def sdf_ellipse(x, y, cx, cy, rx, ry, angle_deg=0.0):
    a = math.radians(angle_deg)
    dx, dy = x - cx, y - cy
    u = dx * math.cos(a) + dy * math.sin(a)
    v = -dx * math.sin(a) + dy * math.cos(a)
    # approximate distance: scaled radial distance
    k = np.sqrt((u / rx) ** 2 + (v / ry) ** 2)
    return (k - 1.0) * min(rx, ry)


def sdf_box(x, y, cx, cy, hw, hh, radius=0.0):
    qx = np.abs(x - cx) - hw + radius
    qy = np.abs(y - cy) - hh + radius
    outside = np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2)
    return outside + np.minimum(np.maximum(qx, qy), 0) - radius


def sdf_polygon(x, y, pts):
    """Signed distance to a closed polygon (list of (x, y))."""
    pts = np.asarray(pts, dtype=np.float64)
    d = np.full(np.shape(x), np.inf)
    sign = np.ones(np.shape(x))
    n = len(pts)
    for i in range(n):
        ax, ay = pts[i - 1]
        bx, by = pts[i]
        ex, ey = bx - ax, by - ay
        wx, wy = x - ax, y - ay
        t = np.clip((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1)
        d = np.minimum(d, np.hypot(wx - ex * t, wy - ey * t))
        c1 = y >= ay
        c2 = y < by
        c3 = ex * wy > ey * wx
        flip = (c1 & c2 & c3) | (~c1 & ~c2 & ~c3)
        sign = np.where(flip, -sign, sign)
    return sign * d


def coverage(sdf, soft=0.6):
    """Antialiased fill (1 inside, 0 outside) from a signed distance in mm."""
    return np.clip(0.5 - np.asarray(sdf) / (2 * soft), 0, 1)


# --- colour ---

def encode_rgb565(frame: np.ndarray) -> bytes:
    """Linear 128x128x3 frame (0..1) -> RGB565 little-endian bytes."""
    enc = np.clip(frame, 0, 1) ** (1 / GAMMA)
    r = np.round(enc[..., 0] * 31).astype(np.uint16)
    g = np.round(enc[..., 1] * 63).astype(np.uint16)
    b = np.round(enc[..., 2] * 31).astype(np.uint16)
    return ((r << 11) | (g << 5) | b).astype("<u2").tobytes()


def decode_rgb565(data: bytes) -> np.ndarray:
    """RGB565 frame -> gamma-encoded 128x128x3 (0..1), as it looks on a monitor."""
    v = np.frombuffer(data, dtype="<u2").reshape(H, W)
    return np.stack([(v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0], axis=-1)


def linear_load(frame: np.ndarray) -> float:
    """Average linear load of a frame (fraction of full white), as the firmware's limiter sees it."""
    return float(np.clip(frame, 0, 1).mean())


# --- .vid files (docs/design.md "Media formats"; parsed by display/player.c) ---

VID_MAGIC = b"PVID"
VID_VERSION = 1
VID_HEADER = 512
VID_RGB565 = 1
FRAME_BYTES = W * H * 2


class VidWriter:
    def __init__(self, path: str | Path, fps: int):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fps = fps
        self.count = 0
        self.f = open(self.path, "wb")
        self.f.write(b"\0" * VID_HEADER)

    def add(self, frame: np.ndarray) -> None:
        self.f.write(encode_rgb565(frame))
        self.count += 1

    def close(self) -> None:
        self.f.seek(0)
        self.f.write(struct.pack("<4sHHHHIH", VID_MAGIC, VID_VERSION, W, H, self.fps, self.count, VID_RGB565))
        self.f.close()


def read_vid(path: str | Path):
    """Yields (fps, frame_count) first, then gamma-encoded frames."""
    with open(path, "rb") as f:
        head = f.read(VID_HEADER)
        magic, version, w, h, fps, count, fmt = struct.unpack_from("<4sHHHHIH", head)
        if magic != VID_MAGIC or (w, h) != (W, H) or fmt != VID_RGB565:
            raise ValueError(f"{path}: not a 128x128 RGB565 .vid file")
        yield fps, count
        for _ in range(count):
            yield decode_rgb565(f.read(FRAME_BYTES))


# --- preview: the frame as seen from the front, with the slit ---

PREVIEW_SCALE = 2.5  # preview pixels per mm
PREVIEW_W = int(math.ceil(230 * PREVIEW_SCALE)) // 2 * 2
PREVIEW_H = int(math.ceil(270 * PREVIEW_SCALE)) // 2 * 2
_PX = np.round(FX * PREVIEW_SCALE + PREVIEW_W / 2).astype(int)
_PY = np.round(-FY * PREVIEW_SCALE + PREVIEW_H / 2).astype(int)
PREVIEW_DOT = 3


def preview_image(encoded: np.ndarray) -> np.ndarray:
    """Gamma-encoded 128x128x3 frame -> uint8 frontal preview (LEDs as dots, slit visible)."""
    img = np.full((PREVIEW_H, PREVIEW_W, 3), 12, dtype=np.uint8)
    # panel outline: dark grey dots for unlit LEDs
    for dy in range(PREVIEW_DOT):
        for dx in range(PREVIEW_DOT):
            img[_PY + dy - 1, _PX + dx - 1] = np.maximum(
                (np.clip(encoded, 0, 1) * 255).astype(np.uint8), 28)
    return img


class PreviewWriter:
    """Writes preview frames to an .mp4 (ffmpeg) or, for a .png path, a contact sheet."""

    def __init__(self, path: str | Path, fps: int, sheet_every: int = 0, size: tuple[int, int] = (0, 0)):
        self.w, self.h = size if size[0] else (PREVIEW_W, PREVIEW_H)
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.sheet = self.path.suffix.lower() == ".png"
        self.every = sheet_every or fps
        self.frames: list[np.ndarray] = []
        self.n = 0
        if not self.sheet:
            self.proc = subprocess.Popen(
                ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s",
                 f"{self.w}x{self.h}", "-r", str(fps), "-i", "-", "-pix_fmt", "yuv420p", "-crf", "18",
                 str(self.path)], stdin=subprocess.PIPE)

    def add(self, encoded: np.ndarray) -> None:
        self.add_image(preview_image(encoded))

    def add_image(self, img: np.ndarray) -> None:
        """A finished uint8 image of the writer's size (e.g. mask and staff side by side)."""
        if self.sheet:
            if self.n % self.every == 0 and len(self.frames) < 12:
                self.frames.append(img)
        else:
            self.proc.stdin.write(img.tobytes())
        self.n += 1

    def close(self) -> None:
        if not self.sheet:
            self.proc.stdin.close()
            self.proc.wait()
            return
        from PIL import Image
        cols = min(4, len(self.frames))
        rows = (len(self.frames) + cols - 1) // cols
        h, w = self.h, self.w
        sheet = np.zeros((rows * h, cols * w, 3), dtype=np.uint8)
        for i, f in enumerate(self.frames):
            sheet[(i // cols) * h:(i // cols + 1) * h, (i % cols) * w:(i % cols + 1) * w] = f
        Image.fromarray(sheet).save(self.path)


def encoded_view(frame: np.ndarray) -> np.ndarray:
    """What the panel shows, gamma-encoded for a monitor (with RGB565 quantisation)."""
    return decode_rgb565(encode_rgb565(frame))


# --- CLI ---

def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[0] == "preview":
        frames = read_vid(argv[1])
        fps, count = next(frames)
        pw = PreviewWriter(argv[2], fps)
        for f in frames:
            pw.add(f)
        pw.close()
        print(f"{argv[1]}: {count} frames at {fps} fps -> {argv[2]}")
        return 0
    if argv and argv[0] == "geometry":
        print("column   X(mm)  gap(rows)  Y U row 63  Y U row 0")
        for c in (0, 8, 16, 24, 32, 40, 48, 56, 63):
            print(f"{c:6d} {FX[0, c]:7.1f} {gap_rows(c):10.1f} {FY[63, c]:11.1f} {FY[0, c]:10.1f}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
