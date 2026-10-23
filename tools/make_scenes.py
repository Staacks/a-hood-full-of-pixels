"""Generates the scene videos (mask) and strip animations (staff).

    python3 tools/make_scenes.py                    # all scenes
    python3 tools/make_scenes.py eyes fireskull     # only these
    python3 tools/make_scenes.py eyes --sheet       # contact sheet PNG instead of a preview video
    python3 tools/make_scenes.py eyes --seconds 2   # short test render (loops only)

Output:
    media/sd_display/<name>.vid, <name>_intro.vid       -> root of the display's SD card
    media/sd_controller/<name>.led, <name>_intro.led    -> root of the controller's SD card
    media/preview/<name>.mp4                            mask and staff: intro, then the loop twice

Copy to freshly formatted FAT32 cards (contiguous files). Each scene is a module in
tools/scenes/ with
    SECONDS, render(n, fps, rng)                        mask loop: linear 128x128x3 frames
    INTRO_SECONDS, render_intro(n, fps, rng, loop)      mask intro (optional)
    STAFF_SECONDS, render_staff(n, fps, rng)            staff loop: linear (BODY, 2, 3) (optional)
    STAFF_INTRO_SECONDS, render_staff_intro(n, fps, rng, loop)    staff intro (optional)
Loops must join seamlessly (last frame -> first frame). Intros get the finished loop frames;
their last INTRO_BLEND_S are crossfaded here into the loop's last frames, so the intro runs
into the loop's first frame without a jump. Without a staff loop the controller falls back to
the scene's built-in effect (and a staff intro blends to black).
"""

import argparse
import importlib
import sys
import time
from pathlib import Path

import numpy as np

import maskvid
import staffled

SCENES = ["eyes", "fireskull", "matrix", "lightning", "rainbow", "youtube", "maul", "creeper", "rick", "pumpkin",
          "family", "me"]
ROOT = Path(__file__).resolve().parent.parent
INTRO_BLEND_S = 0.4


def blend_into(intro: list, loop: list, fps: int) -> list:
    """Crossfades the intro's last frames into the loop's last frames (then loop frame 0 follows)."""
    b = min(len(intro), int(INTRO_BLEND_S * fps))
    out = list(intro)
    for j in range(b):
        a = (j + 1) / b
        target = loop[len(loop) - b + j] if loop else np.zeros_like(intro[0])
        out[len(intro) - b + j] = intro[len(intro) - b + j] * (1 - a) + target * a
    return out


def render(fn, seconds: float, fps: int, seed: int, *loop) -> list:
    n = int(round(seconds * fps))
    return [np.asarray(f, dtype=np.float32) for f in fn(n, fps, np.random.default_rng(seed), *loop)]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("scenes", nargs="*", default=SCENES)
    p.add_argument("--fps", type=int, default=25)
    p.add_argument("--seconds", type=float, help="override the loop lengths, skip intros (tests)")
    p.add_argument("--sheet", action="store_true", help="PNG contact sheet instead of an .mp4 preview")
    p.add_argument("--out", default=str(ROOT / "media"))
    p.add_argument("--seed", type=int, default=1)
    a = p.parse_args()

    out = Path(a.out)
    fps = a.fps
    for name in a.scenes:
        t0 = time.time()
        try:
            mod = importlib.import_module(f"scenes.{name}")
        except ModuleNotFoundError as e:
            if e.name != f"scenes.{name}":
                raise
            print(f"{name}: skipped (tools/scenes/{name}.py not included)")
            continue
        if not getattr(mod, "AVAILABLE", True):   # e.g. family without a selection of faces
            print(f"{name}: skipped (no source material)")
            continue
        intros = a.seconds is None

        loop = render(mod.render, a.seconds or mod.SECONDS, fps, a.seed)
        intro = []
        if intros and hasattr(mod, "render_intro"):
            intro = blend_into(render(mod.render_intro, mod.INTRO_SECONDS, fps, a.seed + 1, loop), loop, fps)
        staff = []
        if hasattr(mod, "render_staff"):
            staff = render(mod.render_staff, a.seconds or mod.STAFF_SECONDS, fps, a.seed + 2)
        staff_intro = []
        if intros and hasattr(mod, "render_staff_intro"):
            si = render(mod.render_staff_intro, mod.STAFF_INTRO_SECONDS, fps, a.seed + 3, staff)
            staff_intro = blend_into(si, staff, fps)
        staff = [staffled.full_frame(f) for f in staff]
        staff_intro = [staffled.full_frame(f) for f in staff_intro]

        report = [name + ":"]
        for suffix, frames in (("", loop), ("_intro", intro)):
            if frames:
                vid = maskvid.VidWriter(out / "sd_display" / f"{name}{suffix}.vid", fps)
                for f in frames:
                    vid.add(f)
                vid.close()
                loads = [maskvid.linear_load(f) for f in frames]
                report.append(f"mask{suffix} {len(frames) / fps:.1f} s load avg {np.mean(loads) * 100:.0f} % "
                              f"max {np.max(loads) * 100:.0f} %")
        for suffix, frames in (("", staff), ("_intro", staff_intro)):
            if frames:
                led = staffled.LedWriter(out / "sd_controller" / f"{name}{suffix}.led", fps)
                for f in frames:
                    led.add(f)
                led.close()
                ma = [staffled.strip_ma(f) for f in frames]
                report.append(f"staff{suffix} {len(frames) / fps:.1f} s {np.mean(ma):.0f} mA avg "
                              f"{np.max(ma):.0f} max at full brightness")

        # preview: intro, then the loop twice; mask and staff side by side
        total = max(len(intro) + 2 * len(loop), len(staff_intro) + 2 * len(staff))
        size = (maskvid.PREVIEW_W + staffled.PREVIEW_W, maskvid.PREVIEW_H)
        prev = maskvid.PreviewWriter(out / "preview" / f"{name}.{'png' if a.sheet else 'mp4'}", fps,
                                     sheet_every=max(1, total // 12), size=size)
        dark = staffled.full_frame(np.zeros((staffled.BODY, 2, 3)))
        for k in range(total):
            m = intro[k] if k < len(intro) else loop[(k - len(intro)) % len(loop)]
            if k < len(staff_intro):
                s = staff_intro[k]
            else:
                s = staff[(k - len(staff_intro)) % len(staff)] if staff else dark
            prev.add_image(np.hstack([maskvid.preview_image(maskvid.encoded_view(m)),
                                      staffled.preview_image(s, maskvid.PREVIEW_H)]))
        prev.close()
        print(", ".join(report) + f" ({time.time() - t0:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
