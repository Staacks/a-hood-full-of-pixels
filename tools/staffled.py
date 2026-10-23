"""Staff strip: geometry, .led files, current estimate and preview (see docs/design.md).

Animations are rendered as frames of (PER_SIDE, 2, 3) linear light: height 0 = bottom (the
electronics), side 0 = front, side 1 = back. The strip shows linear light (WS2812 PWM), so a
value is stored as linear * 255; the controller applies its brightness and current limiter.

.led file: 512-byte header (little-endian: magic "PLED", version, LED count, LEDs per side,
fps, frame count), zero-padded; then frames of LED count x 3 bytes RGB in physical order (LED 0
at the bottom of the front, up the front, down the back). Parsed by controller/staffanim.c.

The crystal (top pairs) is drawn by the firmware; animations leave it dark. Dead heights (grip,
connector rings) are switched off by the firmware; renders may ignore them.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

PER_SIDE = 90
CRYSTAL_PAIRS = 3
BODY = PER_SIDE - CRYSTAL_PAIRS      # heights 0 .. BODY-1 are animated
PITCH_MM = 1000.0 / 60               # 60 LEDs/m
DEAD = sorted(set(range(64, 72)) | {86})  # firmware heights of "65-72, 87" (controller/strip.h)

# current model (controller/strip.h): base + sum of channel values / 255 * mA per channel
BASE_MA = 136
MA_PER_CHANNEL = 8.931
BUDGET_MA = 2600

LED_MAGIC = b"PLED"
LED_VERSION = 1
LED_HEADER = 512


def heights() -> np.ndarray:
    """Height index of every body LED (0 .. BODY-1), as a float column for numpy maths."""
    return np.arange(BODY, dtype=np.float64)


def full_frame(body: np.ndarray) -> np.ndarray:
    """(BODY, 2, 3) -> (PER_SIDE, 2, 3) with a dark crystal."""
    f = np.zeros((PER_SIDE, 2, 3))
    f[:BODY] = body
    return f


def encode(frame: np.ndarray) -> np.ndarray:
    """(PER_SIDE, 2, 3) linear -> (2 * PER_SIDE, 3) uint8 in physical order."""
    v = np.round(np.clip(frame, 0, 1) * 255).astype(np.uint8)
    return np.concatenate([v[:, 0], v[::-1, 1]])


def strip_ma(frame: np.ndarray, brightness: int = 255) -> float:
    """Estimated current at the power bank (controller model), dead LEDs and crystal excluded."""
    v = np.round(np.clip(frame[:BODY], 0, 1) * 255)
    v[DEAD] = 0
    return BASE_MA + MA_PER_CHANNEL * v.sum() / 255 * brightness / 255


class LedWriter:
    def __init__(self, path: str | Path, fps: int):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fps = fps
        self.count = 0
        self.f = open(self.path, "wb")
        self.f.write(b"\0" * LED_HEADER)

    def add(self, frame: np.ndarray) -> None:
        self.f.write(encode(frame).tobytes())
        self.count += 1

    def close(self) -> None:
        self.f.seek(0)
        self.f.write(struct.pack("<4sHHHHI", LED_MAGIC, LED_VERSION, 2 * PER_SIDE, PER_SIDE, self.fps, self.count))
        self.f.close()


# --- preview: both sides as two columns of dots, bottom at the bottom ---

PREVIEW_W = 80


def preview_image(frame: np.ndarray, height_px: int) -> np.ndarray:
    """(PER_SIDE, 2, 3) linear -> uint8 image (height_px x PREVIEW_W), gamma-encoded for a monitor."""
    img = np.full((height_px, PREVIEW_W, 3), 12, dtype=np.uint8)
    step = (height_px - 20) / PER_SIDE
    dot = max(2, int(step) - 1)
    enc = (np.clip(frame, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    for h in range(PER_SIDE):
        y = int(height_px - 10 - (h + 1) * step)
        for side, x in ((0, 22), (1, 50)):
            c = enc[h, side]
            if h in DEAD:
                c = np.array([40, 0, 0], dtype=np.uint8)   # hidden LED
            elif h >= BODY:
                c = np.array([70, 70, 90], dtype=np.uint8)  # crystal (drawn by the firmware)
            else:
                c = np.maximum(c, 28)
            img[y:y + dot, x:x + 8] = c
    return img
