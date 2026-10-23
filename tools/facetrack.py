"""Stabilised face clips: finds a face in every frame of a video and maps the frames onto the
frontal canvas so that the eyes stay at fixed positions (the face holds still on the mask,
the background moves instead). The background is faded out around the head.

Face landmarks (eyes, nose tip, mouth corners) come from OpenCV's YuNet face detector
(MIT licence). The model, 345 KB, is downloaded once to ~/.cache/matrixmask/.

    from facetrack import read_clip, stabilise
    frames = read_clip("clip.mp4", start=69.12, count=87)    # sRGB uint8 frames
    canvases = stabilise(frames)                              # linear frontal canvases

Find cuts with --cuts, e.g. python3 tools/facetrack.py clip.mp4 --cuts 60 20 (hard cuts between
1:00 and 1:20, frame accurate).
"""

import argparse
import json
import math
import subprocess
import sys
import urllib.request
from pathlib import Path

import cv2
import numpy as np
from scipy import ndimage

from maskvid import CANVAS_H, CANVAS_MM, CANVAS_W, CX, CY, GAMMA, mm_to_canvas

MODEL_URL = ("https://media.githubusercontent.com/media/opencv/opencv_zoo/7e062e54cf5410c09b795ff71b4a255e58498c79/"
             "models/face_detection_yunet/face_detection_yunet_2022mar.onnx")
MODEL = Path.home() / ".cache" / "matrixmask" / "face_detection_yunet_2022mar.onnx"
DETECT_W = 960   # frames are scaled to this width for the detector


def _model() -> str:
    if not MODEL.exists():
        print(f"downloading the face detector model to {MODEL}", file=sys.stderr)
        MODEL.parent.mkdir(parents=True, exist_ok=True)
        tmp = MODEL.with_suffix(".part")
        urllib.request.urlretrieve(MODEL_URL, tmp)
        tmp.rename(MODEL)
    return str(MODEL)


def video_info(path) -> tuple[int, int, float]:
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height,r_frame_rate", "-of", "json", str(path)],
                         check=True, capture_output=True, text=True).stdout
    s = json.loads(out)["streams"][0]
    num, den = s["r_frame_rate"].split("/")
    return s["width"], s["height"], int(num) / int(den)


def read_clip(path, start: float, count: int) -> list[np.ndarray]:
    """count frames (sRGB uint8, H x W x 3) from start (s), frame accurate."""
    w, h, _ = video_info(path)
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-ss", str(start), "-i", str(path), "-frames:v", str(count),
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], check=True, capture_output=True).stdout
    frames = np.frombuffer(raw, dtype=np.uint8).reshape(-1, h, w, 3)
    return list(frames)


def find_cuts(path, start: float, seconds: float, threshold: float = 20.0) -> list[float]:
    """Times (s) of hard cuts: frames whose mean difference to the previous one is large."""
    _, _, fps = video_info(path)
    w, h = 192, 108
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-ss", str(start), "-i", str(path), "-t", str(seconds),
                          "-vf", f"scale={w}:{h}", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         check=True, capture_output=True).stdout
    f = np.frombuffer(raw, dtype=np.uint8).reshape(-1, h, w).astype(np.float32)
    diff = np.abs(np.diff(f, axis=0)).mean(axis=(1, 2))
    return [round(start + (k + 1) / fps, 3) for k in np.nonzero(diff > threshold)[0]]


def landmarks(frames: list[np.ndarray]) -> np.ndarray:
    """(n, 5, 2) landmarks per frame in pixels: the face's right eye (left in the image), left
    eye, nose tip, right and left mouth corner. Frames without a face are interpolated."""
    h, w = frames[0].shape[:2]
    scale = DETECT_W / w
    size = (DETECT_W, int(round(h * scale)))
    det = cv2.FaceDetectorYN.create(_model(), "", size, 0.5)
    out = np.full((len(frames), 5, 2), np.nan)
    prev = None
    for k, f in enumerate(frames):
        _, faces = det.detect(cv2.cvtColor(cv2.resize(f, size, interpolation=cv2.INTER_AREA), cv2.COLOR_RGB2BGR))
        if faces is None:
            continue
        # the confident detections (hands and jackets come out below ~0.7), the one nearest to
        # the face in the previous frame
        sure = faces[faces[:, 14] >= 0.8]
        if len(sure) == 0:
            sure = faces[[np.argmax(faces[:, 14])]]
        pts = sure[:, 4:14].reshape(-1, 5, 2) / scale
        if prev is not None:
            best = np.argmin(np.hypot(*(pts.mean(axis=1) - prev).T))
        else:
            best = np.argmax(sure[:, 14])
        out[k] = pts[best]
        prev = out[k].mean(axis=0)
    # outliers: frames whose eye midpoint is far from its neighbours' median are dropped
    eyes = (out[:, 0] + out[:, 1]) / 2
    iod = np.hypot(*(out[:, 1] - out[:, 0]).T)
    med = np.stack([ndimage.median_filter(np.nan_to_num(eyes[:, c], nan=np.nanmedian(eyes[:, c])), 5,
                                          mode="nearest") for c in range(2)], axis=1)
    jump = np.hypot(*(eyes - med).T) > 0.5 * np.nanmedian(iod)
    out[jump] = np.nan
    found = ~np.isnan(out[:, 0, 0])
    if not found.any():
        raise ValueError("no face found")
    idx = np.arange(len(frames))
    for p in range(5):
        for c in range(2):
            out[:, p, c] = np.interp(idx, idx[found], out[found, p, c])
    return out


def poses(lm: np.ndarray, smooth: float = 1.0) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Per frame: eye midpoint (n, 2), eye line angle (rad), face size (px), smoothed over time.
    The size combines the eye distance and the eye-mouth distance, so turning or nodding the
    head changes it less than either alone."""
    eyes = (lm[:, 0] + lm[:, 1]) / 2
    mouth = (lm[:, 3] + lm[:, 4]) / 2
    d = lm[:, 1] - lm[:, 0]
    angle = np.arctan2(d[:, 1], d[:, 0])
    iod = np.hypot(d[:, 0], d[:, 1])
    em = np.hypot(*(mouth - eyes).T)
    ratio = np.median(iod / em)
    size = np.sqrt((iod ** 2 + (ratio * em) ** 2) / 2)
    if smooth > 0:
        eyes = ndimage.gaussian_filter1d(eyes, smooth, axis=0, mode="nearest")
        angle = ndimage.gaussian_filter1d(angle, smooth, mode="nearest")
        size = ndimage.gaussian_filter1d(size, smooth, mode="nearest")
    return eyes, angle, size


def head_zones(eye_x: float, eye_y: float) -> tuple[np.ndarray, np.ndarray]:
    """Canvas seeds for the segmentation, distances in units of the zone size (< 1 inside):
    sure: face and hair, surely the person; head: the head with hair and the neck, maybe."""
    iod = 2 * eye_x
    sure = np.hypot(CX / (0.72 * iod), (CY - (eye_y + 0.3 * iod)) / (1.2 * iod))
    head = np.hypot(CX / (1.2 * iod), (CY - (eye_y + 0.35 * iod)) / (1.9 * iod))
    neck = np.hypot(CX / (0.75 * iod), (CY - (eye_y - 1.9 * iod)) / (0.9 * iod))
    return sure, np.minimum(head, neck)


def segment(img: np.ndarray, zones, iterations: int = 3) -> np.ndarray:
    """Foreground 0/1 of a canvas (linear RGB) by GrabCut, seeded by the head zones: surely
    the person in the sure zone, surely background well outside the head, GrabCut decides the
    rest."""
    sure, head = zones
    enc = (np.clip(img, 0, 1) ** (1 / GAMMA) * 255).astype(np.uint8)
    seed = np.full(head.shape, cv2.GC_PR_BGD, np.uint8)
    seed[head < 1.0] = cv2.GC_PR_FGD
    seed[sure < 1.0] = cv2.GC_FGD
    seed[head > 1.25] = cv2.GC_BGD
    bgd, fgd = np.zeros((1, 65)), np.zeros((1, 65))
    cv2.grabCut(cv2.cvtColor(enc, cv2.COLOR_RGB2BGR), seed, None, bgd, fgd, iterations, cv2.GC_INIT_WITH_MASK)
    fg = (seed == cv2.GC_FGD) | (seed == cv2.GC_PR_FGD)
    # keep the blob connected to the face, fill holes
    lab, _ = ndimage.label(fg)
    core = lab[sure < 1.0]
    if core.size and core.max() > 0:
        fg = np.isin(lab, np.unique(core[core > 0]))
    return ndimage.binary_fill_holes(fg).astype(np.float32)


def stabilise(frames: list[np.ndarray], eyes_mm=(26.0, 14.0), smooth: float = 1.0, gain: float = 1.0,
              lm: np.ndarray | None = None, background: float = 0.0, feather: float = 2.0,
              shrink: int = 2) -> list[np.ndarray]:
    """Linear frontal canvases with the face's eyes at (-x, y) and (x, y) mm (x, y = eyes_mm).
    The person is cut out (GrabCut per frame, smoothed over time, edges feathered by feather
    canvas px, after pulling it in by shrink px); background: how much of the rest stays (0 =
    black)."""
    if lm is None:
        lm = landmarks(frames)
    centre, angle, size = poses(lm, smooth)
    ratio = np.median(np.hypot(*(lm[:, 1] - lm[:, 0]).T) / size)   # eye distance per size unit
    eye_x, eye_y = eyes_mm
    mid_c, mid_r = mm_to_canvas(0.0, eye_y)
    zones = head_zones(eye_x, eye_y)
    lut = ((np.arange(256) / 255.0) ** GAMMA).astype(np.float32)
    imgs, masks = [], []
    for k, f in enumerate(frames):
        s = 2 * eye_x / CANVAS_MM / (size[k] * ratio)          # canvas px per source px
        # a source that shrinks a lot (video: about 3.5x) is reduced first, so the bilinear warp
        # does not alias
        red = max(1, round(1 / (2 * s)))
        src = cv2.resize(f, (f.shape[1] // red, f.shape[0] // red), interpolation=cv2.INTER_AREA) if red > 1 else f
        lin = lut[src]
        s *= red
        a = angle[k]
        A = s * np.array([[math.cos(a), math.sin(a)], [-math.sin(a), math.cos(a)]])
        t = np.array([mid_c, mid_r]) - A @ (centre[k] / red)
        m = np.hstack([A, t[:, None]]).astype(np.float32)
        img = cv2.warpAffine(lin, m, (CANVAS_W, CANVAS_H), flags=cv2.INTER_LINEAR,
                             borderMode=cv2.BORDER_CONSTANT, borderValue=0)
        # where the source frame ends (a head cut off by the frame's edge): no cut-out there, the
        # frame's edge rows would show as a line
        valid = cv2.warpAffine(np.ones(lin.shape[:2], np.float32), m, (CANVAS_W, CANVAS_H),
                               flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)
        valid = ndimage.binary_erosion(valid > 0.99, iterations=2)
        imgs.append(img)
        cut = segment(img, zones) if background < 1 else np.ones(zones.shape, np.float32)
        masks.append(cut * valid)
    # pull the edge in (the soft edge would otherwise show a fringe of the background: a halo in
    # front of a bright wall), average the cut-out over neighbouring frames against flickering
    # edges, then feather it
    if shrink:
        masks = [ndimage.binary_erosion(m > 0.5, iterations=shrink).astype(np.float32) for m in masks]
    masks = ndimage.gaussian_filter(np.array(masks), (0.8, feather, feather), mode="nearest")
    return [img * (background + (1 - background) * m)[..., None] * gain for img, m in zip(imgs, masks)]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("video")
    p.add_argument("--cuts", nargs=2, type=float, metavar=("START", "SECONDS"), help="list hard cuts")
    a = p.parse_args()
    if a.cuts:
        for t in find_cuts(a.video, *a.cuts):
            print(f"cut at {int(t // 60)}:{t % 60:05.2f} ({t:.2f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
