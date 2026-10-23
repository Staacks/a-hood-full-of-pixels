"""Converts any video (e.g. a Blender render) into a .vid file for the mask.

The source is treated as the mask seen from the front: it is fitted onto the frontal canvas
(212 x 253 mm visible, docs/mask_geometry.md) and every panel pixel samples it at its frontal
position, so the slit, the bowing and the compressed outer columns are handled. Render sources
in that aspect ratio (about 5:6, e.g. 600x720) with the eyes at the slit:
the wearer's eyes are at 35 % and 65 % of the width, the slit at half the height.

    python3 tools/convert_video.py in.mp4 media/sd_display/myscene.vid --fps 25 --preview myscene.mp4
    python3 tools/convert_video.py in.mp4 out.vid --start 2 --seconds 6 --fit contain

Power: the display limits the current at runtime (dims frames above the budget), so no
offline limiter is needed. The tool prints the linear load for orientation.
"""

import argparse
import subprocess
import sys

import numpy as np

import maskvid


def frames(path, fps, fit, start, seconds):
    w, h = maskvid.CANVAS_W, maskvid.CANVAS_H
    if fit == "cover":
        vf = f"scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h}"
    else:
        vf = f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2"
    cmd = ["ffmpeg", "-loglevel", "error"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if seconds:
        cmd += ["-t", str(seconds)]
    cmd += ["-vf", f"fps={fps},{vf}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = w * h * 3
    while True:
        data = proc.stdout.read(size)
        if len(data) < size:
            break
        img = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 3) / 255.0
        yield img ** maskvid.GAMMA  # video is gamma-encoded; the canvas is linear light
    proc.wait()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input")
    p.add_argument("output", help=".vid file")
    p.add_argument("--fps", type=int, default=25)
    p.add_argument("--fit", choices=["cover", "contain"], default="cover",
                   help="cover: fill the mask, crop the source; contain: whole source, black borders")
    p.add_argument("--start", type=float, help="start time in the source (s)")
    p.add_argument("--seconds", type=float, help="length (s)")
    p.add_argument("--gain", type=float, default=1.0, help="brightness factor (linear)")
    p.add_argument("--preview", help="frontal preview: .mp4 or .png (contact sheet)")
    a = p.parse_args()

    vid = maskvid.VidWriter(a.output, a.fps)
    prev = maskvid.PreviewWriter(a.preview, a.fps) if a.preview else None
    loads = []
    for canvas in frames(a.input, a.fps, a.fit, a.start, a.seconds):
        f = maskvid.sample_canvas(canvas * a.gain)
        vid.add(f)
        loads.append(maskvid.linear_load(f))
        if prev:
            prev.add(maskvid.encoded_view(f))
    vid.close()
    if prev:
        prev.close()
    if not loads:
        print("no frames read", file=sys.stderr)
        return 1
    print(f"{a.output}: {vid.count} frames at {a.fps} fps, linear load avg {np.mean(loads) * 100:.1f} % "
          f"max {np.max(loads) * 100:.1f} %")
    return 0


if __name__ == "__main__":
    sys.exit(main())
