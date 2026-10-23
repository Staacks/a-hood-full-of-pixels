#!/usr/bin/env python3
"""Record and plot LIS3DH data from the controller's tuning mode.

  plot_accel.py record /dev/ttyACM0 stomps.csv   # Ctrl+C to stop
  plot_accel.py plot stomps.csv [--axis y]

The firmware prints "A,<ms>,<x_mg>,<y_mg>,<z_mg>" per sample and
"S,<ms>,<click_src>,<tilt_deg>,<verdict>" per detected click while streaming is on
(serial command 't'). Recording toggles streaming on and off automatically.

Record deliberate stomps, walking with the staff and waving it in separate files, then
choose the click threshold between the walking peaks and the stomp peaks.

Needs: pyserial (record), matplotlib (plot).
"""

import argparse
import math
import sys
import time


def record(port: str, out: str) -> None:
    import serial

    with serial.Serial(port, 115200, timeout=0.2) as ser, open(out, "w") as f:
        ser.write(b"t")
        print(f"recording to {out}, Ctrl+C to stop")
        n_samples = n_clicks = 0
        try:
            while True:
                line = ser.readline().decode(errors="replace").strip()
                if line.startswith("A,"):
                    n_samples += 1
                elif line.startswith("S,"):
                    n_clicks += 1
                    print(f"click: {line}")
                else:
                    continue
                f.write(line + "\n")
        except KeyboardInterrupt:
            pass
        finally:
            ser.write(b"t")
            time.sleep(0.2)
    print(f"{n_samples} samples ({n_samples / 400:.1f} s), {n_clicks} clicks")


def load(path: str):
    samples, clicks = [], []
    with open(path) as f:
        for line in f:
            p = line.strip().split(",")
            if p[0] == "A" and len(p) == 5:
                samples.append(tuple(int(v) for v in p[1:]))
            elif p[0] == "S" and len(p) == 5:
                clicks.append((int(p[1]), p[2], p[3], p[4]))
    return samples, clicks


def plot(path: str, axis: str) -> None:
    import matplotlib.pyplot as plt

    samples, clicks = load(path)
    if not samples:
        sys.exit("no samples in file")
    t0 = samples[0][0]
    t = [(s[0] - t0) / 1000 for s in samples]
    xyz = {"x": [s[1] / 1000 for s in samples], "y": [s[2] / 1000 for s in samples],
           "z": [s[3] / 1000 for s in samples]}
    mag = [math.sqrt(a * a + b * b + c * c) for a, b, c in zip(xyz["x"], xyz["y"], xyz["z"])]

    # Peaks of |a| - 1 g, for choosing the threshold.
    peaks = []
    for i in range(1, len(mag) - 1):
        if mag[i] > 2.0 and mag[i] >= mag[i - 1] and mag[i] >= mag[i + 1]:
            if not peaks or t[i] - peaks[-1][0] > 0.3:
                peaks.append((t[i], mag[i]))
            elif mag[i] > peaks[-1][1]:
                peaks[-1] = (t[i], mag[i])
    print(f"{len(samples)} samples, {len(clicks)} clicks, {len(peaks)} peaks > 2 g:")
    for tp, m in peaks:
        print(f"  t={tp:7.2f} s  |a|={m:5.1f} g")

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(12, 7))
    for name, vals in xyz.items():
        ax1.plot(t, vals, label=name, linewidth=2.0 if name == axis else 0.8)
    ax1.set_ylabel("acceleration [g]")
    ax1.legend(loc="upper right")
    ax2.plot(t, mag, color="k", linewidth=0.8)
    ax2.set_ylabel("|a| [g]")
    ax2.set_xlabel("time [s]")
    colours = {"ok": "g", "refractory": "orange", "tilted": "r"}
    for ms, src, tilt, verdict in clicks:
        for ax in (ax1, ax2):
            ax.axvline((ms - t0) / 1000, color=colours.get(verdict, "b"), linestyle="--", linewidth=1)
        ax2.annotate(f"{verdict}\n{tilt}°", ((ms - t0) / 1000, max(mag)), fontsize=7)
    ax1.set_title(path)
    plt.tight_layout()
    plt.show()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("port")
    r.add_argument("out")
    p = sub.add_parser("plot")
    p.add_argument("file")
    p.add_argument("--axis", default="y", choices="xyz", help="staff axis (drawn bold)")
    args = ap.parse_args()
    if args.cmd == "record":
        record(args.port, args.out)
    else:
        plot(args.file, args.axis)


if __name__ == "__main__":
    main()
