"""Converts the sound clips for the controller's SD card.

    python3 tools/make_sounds.py                     # all scenes (SOUNDS below) -> media/sd_controller/
    python3 tools/make_sounds.py in.mp3 out.wav      # one file

Output: WAV, PCM 16-bit, 44.1 kHz, stereo (the A2DP stream's format, no conversion in the
firmware). Leading silence is cut, so the sound starts at the stomp, and every clip is
normalised to the same peak level. Copy media/sd_controller/*.wav to the root of the
controller's SD card; the file names match the scenes (controller/main.c).
"""

import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "sound original"
OUT = ROOT / "media" / "sd_controller"

# scene file name -> source clip in "sound original/" (freesound.org, all CC0) or the name of a
# generator below (synthesised, no source needed). A missing source is skipped: the scene then
# fires without sound. rick: "sound original/rick.wav" if you bring your own clip, otherwise the
# song from the music video, starting where the mask's intro starts (lip-synced, see
# tools/scenes/rick.py); the song is copyrighted, so neither is included.
SOUNDS = {
    "eyes": "392697__poligonstudio__bass-growl.wav",
    "fireskull": "524305__bertsz__wet-spell-shoot.wav",
    "matrix": "812748__audiopapkin__sound-design-elements-impact-sfx-ps-082.wav",
    "lightning": "812709__audiopapkin__sound-design-elements-impact-sfx-ps-043.wav",
    "rainbow": "chime",
    "youtube": "ding",
    "maul": "saber",
    "creeper": "hiss",
    "rick": "rick.wav",
    "pumpkin": "whoosh",
    "family": "chime",
    "me": "signature.wav",   # my own jingle
}

RATE = 44100
PEAK_DB = -1.0       # normalised peak
SILENCE_DB = -50.0   # leading samples below this are cut
FADE_OUT_S = 0.02    # against a click if the clip ends abruptly


def decode(path: Path, start: float = 0.0, seconds: float = 0.0) -> np.ndarray:
    """Any audio (or video) file -> float32 stereo at RATE (frames x 2), optionally a part."""
    cut = (["-ss", str(start)] if start else []) + ["-i", str(path)] + (["-t", str(seconds)] if seconds else [])
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", *cut, "-vn", "-f", "f32le", "-acodec", "pcm_f32le",
                          "-ac", "2", "-ar", str(RATE), "-"], check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)


def chime() -> np.ndarray:
    """Rising bell arpeggio (C major, two octaves), notes alternating left and right."""
    notes = [1046.5, 1318.5, 1568.0, 2093.0, 2637.0, 3136.0]
    t = np.arange(int(1.6 * RATE)) / RATE
    out = np.zeros((len(t), 2))
    for i, f in enumerate(notes):
        start = int(i * 0.07 * RATE)
        tt = t[:len(t) - start]
        env = np.exp(-tt / 0.45) * np.minimum(1, tt / 0.002)
        tone = (np.sin(2 * np.pi * f * tt) + 0.35 * np.sin(2 * np.pi * 2.76 * f * tt) * np.exp(-tt / 0.15)
                + 0.2 * np.sin(2 * np.pi * 5.4 * f * tt) * np.exp(-tt / 0.06)) * env
        pan = 0.3 if i % 2 else -0.3
        out[start:, 0] += tone * (1 - pan)
        out[start:, 1] += tone * (1 + pan)
    return out


def _t(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def _noise(n, rng, lowpass_hz=None, highpass_hz=None):
    """White noise, optionally band-limited (FFT brick wall)."""
    x = rng.normal(0, 1, n)
    if lowpass_hz or highpass_hz:
        f = np.fft.rfftfreq(n, 1 / RATE)
        spec = np.fft.rfft(x)
        if lowpass_hz:
            spec[f > lowpass_hz] = 0
        if highpass_hz:
            spec[f < highpass_hz] = 0
        x = np.fft.irfft(spec, n)
    return x / np.abs(x).max()


def _stereo(mono, width=0.0):
    return np.stack([mono * (1 - width), mono * (1 + width)], axis=1)


def ding() -> np.ndarray:
    """A bubbly pop, then a cheerful two-note bell (notification)."""
    t = _t(1.3)
    pop_f = 400 + 900 * np.exp(-t / 0.03)
    pop = np.sin(2 * np.pi * np.cumsum(pop_f) / RATE) * np.exp(-t / 0.04)
    out = pop * 0.8
    for start, f in ((0.08, 1318.5), (0.22, 1975.5)):
        tt = np.clip(t - start, 0, None)
        env = np.exp(-tt / 0.35) * np.minimum(1, tt / 0.003) * (t >= start)
        out = out + (np.sin(2 * np.pi * f * tt) + 0.3 * np.sin(2 * np.pi * 2.0 * f * tt) * np.exp(-tt / 0.1)) * env
    return _stereo(out)


def saber() -> np.ndarray:
    """Lightsaber ignition: a crackle and a rising hum, then the steady hum fading out."""
    rng = np.random.default_rng(3)
    t = _t(2.2)
    f0 = 92 + 40 * np.exp(-t / 0.12) - 30 * np.exp(-t / 0.04)
    phase = 2 * np.pi * np.cumsum(f0 * (1 + 0.004 * np.sin(2 * np.pi * 5.3 * t))) / RATE
    hum = sum(np.sin(k * phase) / k ** 0.8 for k in range(1, 12))
    hum = np.tanh(hum * 1.5)
    env = np.minimum(1, t / 0.06) * np.where(t < 1.4, 1, np.exp(-(t - 1.4) / 0.3))
    crackle = _noise(len(t), rng, lowpass_hz=6000, highpass_hz=800) * np.exp(-t / 0.08)
    swell = 1 + 0.5 * np.exp(-((t - 0.15) / 0.1) ** 2)
    out = hum * env * swell * 0.7 + crackle * 0.6
    return _stereo(out, 0.1 * np.sin(2 * np.pi * 0.7 * t))


def hiss() -> np.ndarray:
    """Creeper fuse: a hiss swelling for 1.5 s, cut off."""
    rng = np.random.default_rng(4)
    t = _t(1.6)
    x = _noise(len(t), rng, lowpass_hz=9000, highpass_hz=1500)
    env = np.minimum(1, t / 0.05) * (0.45 + 0.55 * t / 1.6) * np.where(t < 1.5, 1, np.exp(-(t - 1.5) / 0.02))
    flutter = 1 + 0.25 * np.sin(2 * np.pi * 9 * t)
    return np.stack([x * env * flutter, np.roll(x, 37) * env * flutter], axis=1)


def whoosh() -> np.ndarray:
    """Pumpkin: a flame whoosh (noise rising in pitch) and a deep boom when the candle flares."""
    rng = np.random.default_rng(5)
    t = _t(2.4)
    n = len(t)
    out = np.zeros(n)
    # whoosh: noise through a sweeping band (overlapping blocks)
    block = 2048
    noise = rng.normal(0, 1, n + block)
    for i in range(0, n, block // 2):
        seg = noise[i:i + block] * np.hanning(block)
        spec = np.fft.rfft(seg)
        f = np.fft.rfftfreq(block, 1 / RATE)
        centre = 300 + 1700 * min(1.0, i / RATE / 0.6)
        spec *= np.exp(-((np.log(f + 1) - np.log(centre)) / 0.5) ** 2)
        part = np.fft.irfft(spec, block)
        out[i:i + block] += part[:max(0, min(block, n - i))]
    out /= np.abs(out).max()
    out *= np.minimum(1, t / 0.15) * np.exp(-np.clip(t - 0.6, 0, None) / 0.35)
    tb = np.clip(t - 0.6, 0, None)
    boom = np.sin(2 * np.pi * (45 * tb + 40 * 0.08 * (1 - np.exp(-tb / 0.08)))) * np.exp(-tb / 0.5) * (t >= 0.6)
    return _stereo(out * 0.7 + boom * 0.9)


GENERATORS = {"chime": chime, "ding": ding, "saber": saber, "hiss": hiss, "whoosh": whoosh}


def rick_video() -> np.ndarray | None:
    """The song from the rick scene's intro shot (lip-synced) to its end (about 3 min 14 s;
    firing another scene stops it)."""
    from scenes import rick
    if not rick.VIDEO.exists():
        return None
    return decode(rick.VIDEO, rick.SONG_START).astype(np.float64)


# scene -> function giving the sound when its source in "sound original/" is missing
FALLBACKS = {"rick": rick_video}


def convert(src: Path, dst: Path, audio: np.ndarray | None = None) -> None:
    if audio is not None:
        a = audio
    else:
        a = GENERATORS[src.name]() if src.name in GENERATORS else decode(src).astype(np.float64)
    level = np.abs(a).max(axis=1)
    loud = np.nonzero(level > 10 ** (SILENCE_DB / 20))[0]
    if not len(loud):
        raise SystemExit(f"{src}: silent")
    cut = loud[0]
    a = a[cut:loud[-1] + 1]
    a *= 10 ** (PEAK_DB / 20) / np.abs(a).max()
    fade = min(len(a), int(FADE_OUT_S * RATE))
    a[len(a) - fade:] *= np.linspace(1, 0, fade)[:, None]
    pcm = np.round(a * 32767).astype("<i2")
    dst.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    print(f"{dst.name}: {len(pcm) / RATE:.2f} s, {cut / RATE * 1000:.0f} ms leading silence cut ({src.name})")


def main(argv: list[str]) -> int:
    if len(argv) == 2:
        convert(Path(argv[0]), Path(argv[1]))
        return 0
    if argv:
        print(__doc__)
        return 1
    for name, src in SOUNDS.items():
        if src not in GENERATORS and not (SOURCE / src).exists():
            fallback = FALLBACKS[name]() if name in FALLBACKS else None
            if fallback is None:
                print(f"{name}.wav: skipped, {SOURCE / src} missing (the scene fires without sound)")
                continue
            convert(Path(f"{name} (fallback)"), OUT / f"{name}.wav", fallback)
            continue
        convert(SOURCE / src, OUT / f"{name}.wav")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
