"""Faces from a photo folder for the private "family" scene collection.

    python3 tools/family.py scan PHOTO_DIR      # find faces (minutes), then group
    python3 tools/family.py group               # group and rank again from the cache (seconds)
    python3 tools/family.py pick P03 P07b ...   # choose people (letter: alternative, default a)
    python3 tools/family.py pick --first P07b P03 P07b ...   # P07b comes first after the intro
    python3 tools/family.py show P07            # all faces of one person (letters a, b, c, ...)
    python3 tools/family.py similar P07         # faces elsewhere that look like P07 (pick as P07~1, ...)
    python3 tools/family.py pick P03 extra.jpg  # a photo (path, in private/family/ or PHOTO_DIR): its largest face

scan: detects every face (YuNet) and computes a face fingerprint (SFace) -> faces.json.
group: groups the faces by person and ranks each person's faces: looking straight at the
camera, not cut off by the photo's edge, large, sharp, eyes open. Contact sheets
(private/family/people_*.png) show one row per person seen in several photos: the best faces
(a, b, c, d) as they are cut out for the mask; once_*.png shows the people seen only once.
Everything stays in private/ (ignored by git).

pick: stores the selection in private/family/selection.json for tools/scenes/family.py.

Models (OpenCV zoo, downloaded once to ~/.cache/matrixmask/): YuNet face detection (MIT),
SFace face recognition (Apache 2.0).
"""

import argparse
import json
import sys
import urllib.request
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.cluster import hierarchy

import facetrack

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "private" / "family"
SFACE_URL = ("https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/face_recognition_sface/"
             "face_recognition_sface_2021dec.onnx")
SFACE = Path.home() / ".cache" / "matrixmask" / "face_recognition_sface_2021dec.onnx"
DETECT_MAX = 2400        # px, longest side for the detector (group photos have small faces)
MIN_IOD = 40             # px, smaller faces are too blurry for the mask
MIN_SCORE = 0.85         # detector confidence; below are mostly patterns, prints, hands
SAME_PERSON = 0.42       # cosine similarity above which two faces are the same person (average link)
ALTERNATIVES = 4
LETTERS = "abcdefghijklmnopqrstuvwxyz"
PER_SHEET = 8            # people per contact sheet (rows)
ONCE_COLS, ONCE_ROWS = 6, 5   # grid of the people seen once
EYES = (26.0, 22.0)      # mm, the face's eyes on the mask (as for Rick)


def _sface() -> str:
    if not SFACE.exists():
        print(f"downloading the face recognition model to {SFACE}", file=sys.stderr)
        SFACE.parent.mkdir(parents=True, exist_ok=True)
        tmp = SFACE.with_suffix(".part")
        urllib.request.urlretrieve(SFACE_URL, tmp)
        tmp.rename(SFACE)
    return str(SFACE)


def load_rgb(path) -> np.ndarray:
    """Photo as RGB uint8, turned upright by its EXIF orientation."""
    from PIL import ImageOps
    with Image.open(path) as im:
        return np.asarray(ImageOps.exif_transpose(im).convert("RGB"))


def quality(rgb, lm, iod):
    """Scores of one face: yaw (0 = straight), sharpness, eyes open (contrast in the eye
    regions), all from the landmarks and the pixels around them."""
    eyes = (lm[0] + lm[1]) / 2
    d = (lm[1] - lm[0]) / iod
    # nose position along the eye line, relative to the eye midpoint: 0 when looking straight
    yaw = abs(float(np.dot(lm[2] - eyes, d)) / iod)
    x0, y0 = np.maximum(0, (eyes - 1.2 * iod).astype(int))
    x1, y1 = (eyes + 1.2 * iod).astype(int)
    grey = cv2.cvtColor(np.ascontiguousarray(rgb[y0:y1, x0:x1]), cv2.COLOR_RGB2GRAY)
    grey = cv2.resize(grey, (120, 120), interpolation=cv2.INTER_AREA).astype(np.float32)
    sharp = float(cv2.Laplacian(grey, cv2.CV_32F).var())
    opened = []
    for p in lm[:2]:
        c = ((p - (x0, y0)) / (2.4 * iod) * 120).astype(int)
        patch = grey[max(0, c[1] - 6):c[1] + 6, max(0, c[0] - 11):c[0] + 11]
        opened.append(float(patch.std()) if patch.size else 0.0)
    return yaw, sharp, min(opened)


def photo_size(path) -> tuple[int, int]:
    """Width, height of the upright photo (EXIF orientation), without decoding it."""
    with Image.open(path) as im:
        w, h = im.size
        return (h, w) if im.getexif().get(0x0112, 1) in (5, 6, 7, 8) else (w, h)


def clipped(face, size) -> float:
    """Part (0..1) of the head area (hair to neck) that lies outside the photo."""
    lm = np.array(face["lm"])
    cx, cy = lm[:2].mean(axis=0)
    iod = face["iod"]
    x0, x1, y0, y1 = cx - 1.4 * iod, cx + 1.4 * iod, cy - 2.0 * iod, cy + 2.4 * iod
    w, h = size
    inside = max(0.0, min(x1, w) - max(x0, 0)) * max(0.0, min(y1, h) - max(y0, 0))
    return 1 - inside / ((x1 - x0) * (y1 - y0))


def scan(photo_dir: Path):
    OUT.mkdir(parents=True, exist_ok=True)
    rec = cv2.FaceRecognizerSF.create(_sface(), "")
    det = cv2.FaceDetectorYN.create(facetrack._model(), "", (320, 320), 0.7)
    faces = []
    photos = sorted(p for p in photo_dir.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png"))
    small = 0
    for i, path in enumerate(photos):
        rgb = load_rgb(path)
        h, w = rgb.shape[:2]
        scale = min(1.0, DETECT_MAX / max(h, w))
        size = (int(w * scale), int(h * scale))
        det.setInputSize(size)
        bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        _, found = det.detect(cv2.resize(bgr, size, interpolation=cv2.INTER_AREA))
        for f in (found if found is not None else []):
            full = f.copy()
            full[:14] /= scale
            lm = full[4:14].reshape(5, 2)
            iod = float(np.hypot(*(lm[1] - lm[0])))
            if iod < MIN_IOD:
                small += 1
                continue
            feat = rec.feature(rec.alignCrop(bgr, full)).ravel()
            yaw, sharp, opened = quality(rgb, lm, iod)
            faces.append(dict(photo=path.name, lm=lm.tolist(), iod=iod, score=float(f[14]), yaw=yaw,
                              sharp=sharp, opened=opened, feat=(feat / np.linalg.norm(feat)).tolist()))
        print(f"\r{i + 1}/{len(photos)} photos, {len(faces)} faces ({small} too small)", end="", file=sys.stderr)
    print(file=sys.stderr)
    (OUT / "faces.json").write_text(json.dumps(dict(photo_dir=str(photo_dir), faces=faces)))
    group()


def group():
    data = json.loads((OUT / "faces.json").read_text())
    photo_dir = Path(data["photo_dir"])
    faces = [f for f in data["faces"] if f["score"] >= MIN_SCORE]
    sizes = {}
    for f in faces:
        if f["photo"] not in sizes:
            sizes[f["photo"]] = photo_size(photo_dir / f["photo"])
        f["clipped"] = clipped(f, sizes[f["photo"]])
    feats = np.array([f["feat"] for f in faces])
    link = hierarchy.linkage(feats, method="average", metric="cosine")
    labels = hierarchy.fcluster(link, 1 - SAME_PERSON, criterion="distance")
    # rank within a person: straight, big, sharp, eyes open (each normalised over all faces)
    def norm(key):
        v = np.array([f[key] for f in faces])
        return (v - np.median(v)) / (np.percentile(v, 90) - np.percentile(v, 10) + 1e-9)
    rank = -2.0 * norm("yaw") + 0.8 * norm("iod") + 0.7 * norm("sharp") + 1.0 * norm("opened") \
        - 8.0 * np.array([f["clipped"] for f in faces])
    for f, lab, r in zip(faces, labels, rank):
        f["person"] = int(lab)
        f["rank"] = float(r)
    people = {}
    for k, f in enumerate(faces):
        people.setdefault(f["person"], []).append(k)
    # people seen often first; among those seen once the best faces first (the rest is mostly junk)
    order = sorted(people, key=lambda p: (-len(people[p]), -max(faces[k]["rank"] for k in people[p])))
    result = []
    for n, p in enumerate(order):
        idx = sorted(people[p], key=lambda k: -faces[k]["rank"])
        result.append(dict(id=f"P{n + 1:02d}", count=len(idx), faces=idx))
    (OUT / "people.json").write_text(json.dumps(dict(photo_dir=str(photo_dir), faces=faces, people=result)))
    print(f"{len(faces)} faces, {len(result)} people, {sum(r['count'] == 1 for r in result)} of them in one "
          f"photo only")
    for old in OUT.glob("*.png"):
        old.unlink()
    sheets(photo_dir, faces, [r for r in result if r["count"] > 1])
    once_sheets(photo_dir, faces, [r for r in result if r["count"] == 1])


def portrait(photo_dir, face, cache={}):
    """Linear frontal canvas of one face (eyes at EYES, background cut out)."""
    path = photo_dir / face["photo"]
    if path not in cache:
        cache.clear()
        cache[path] = load_rgb(path)
    rgb = cache[path]
    lm = np.array(face["lm"])
    iod = face["iod"]
    # crop around the face first (speed), then the same mapping as for video
    c = lm[:2].mean(axis=0)
    x0, y0 = np.maximum(0, (c - 3.5 * iod).astype(int))
    x1, y1 = (c + 3.5 * iod).astype(int)
    crop = np.ascontiguousarray(rgb[y0:y1, x0:x1])
    return facetrack.stabilise([crop], EYES, smooth=0, lm=(lm - (x0, y0))[None])[0]


def thumb(canvas, level=1.0):
    return Image.fromarray((np.clip(canvas * level, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8))


def _font():
    return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22)


def _mask_thumb(photo_dir, face, size=(168, 196)):
    canvas = portrait(photo_dir, face)
    # brightness normalised for viewing like on the mask (the scene does the same)
    level = 0.35 / max(1e-3, np.percentile(canvas.sum(axis=2), 90) / 3)
    return thumb(canvas, level).resize(size)


def sheets(photo_dir, faces, people):
    font = _font()
    tw, th = 168, 196
    for s in range(0, len(people), PER_SHEET):
        rows = people[s:s + PER_SHEET]
        sheet = Image.new("RGB", (110 + ALTERNATIVES * (tw + 6), len(rows) * (th + 6)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for r, person in enumerate(rows):
            y = r * (th + 6)
            draw.text((8, y + 8), person["id"], fill=(255, 255, 255), font=font)
            draw.text((8, y + 40), f"{person['count']}x", fill=(200, 200, 200), font=font)
            for a, k in enumerate(person["faces"][:ALTERNATIVES]):
                x = 110 + a * (tw + 6)
                sheet.paste(_mask_thumb(photo_dir, faces[k], (tw, th)), (x, y))
                draw.text((x + 4, y + 2), "abcd"[a], fill=(255, 255, 0), font=font)
        path = OUT / f"people_{s // PER_SHEET + 1:02d}.png"
        sheet.save(path)
        print(path)


def once_sheets(photo_dir, faces, people):
    font = _font()
    tw, th = 168, 196
    per = ONCE_COLS * ONCE_ROWS
    for s in range(0, len(people), per):
        part = people[s:s + per]
        rows = (len(part) + ONCE_COLS - 1) // ONCE_COLS
        sheet = Image.new("RGB", (ONCE_COLS * (tw + 6), rows * (th + 6)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for i, person in enumerate(part):
            x, y = (i % ONCE_COLS) * (tw + 6), (i // ONCE_COLS) * (th + 6)
            sheet.paste(_mask_thumb(photo_dir, faces[person["faces"][0]], (tw, th)), (x, y))
            draw.text((x + 4, y + 2), person["id"], fill=(255, 255, 0), font=font)
        path = OUT / f"once_{s // per + 1:02d}.png"
        sheet.save(path)
        print(path)


def show(pid):
    """Contact sheet of all faces of one person, ranked (private/family/show_<id>.png)."""
    data = json.loads((OUT / "people.json").read_text())
    person = {p["id"]: p for p in data["people"]}[pid.upper()]
    idx = person["faces"][:len(LETTERS)]
    font = _font()
    tw, th, cols = 168, 196, 6
    rows = (len(idx) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + 6), rows * (th + 6)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, k in enumerate(idx):
        x, y = (i % cols) * (tw + 6), (i // cols) * (th + 6)
        sheet.paste(_mask_thumb(Path(data["photo_dir"]), data["faces"][k], (tw, th)), (x, y))
        draw.text((x + 4, y + 2), LETTERS[i], fill=(255, 255, 0), font=font)
    path = OUT / f"show_{pid.upper()}.png"
    sheet.save(path)
    print(f"{path}: {len(idx)} faces")


def similar(pid, count=12):
    """Faces not grouped with a person that look like them, from all detections (also the ones
    filtered out: very close faces get low detector scores) -> similar_<id>.png, pick as <id>~n."""
    data = json.loads((OUT / "people.json").read_text())
    raw = json.loads((OUT / "faces.json").read_text())["faces"]
    person = {p["id"]: p for p in data["people"]}[pid.upper()]
    own = [data["faces"][k] for k in person["faces"]]
    centre = np.mean([f["feat"] for f in own], axis=0)
    centre /= np.linalg.norm(centre)
    seen = {(f["photo"], str(f["lm"])) for f in own}
    cands = sorted((f for f in raw if (f["photo"], str(f["lm"])) not in seen),
                   key=lambda f: -float(np.dot(f["feat"], centre)))[:count]
    font = _font()
    tw, th, cols = 168, 196, 6
    sheet = Image.new("RGB", (cols * (tw + 6), ((len(cands) + cols - 1) // cols) * (th + 6)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, f in enumerate(cands):
        x, y = (i % cols) * (tw + 6), (i // cols) * (th + 6)
        sheet.paste(_mask_thumb(Path(data["photo_dir"]), f, (tw, th)), (x, y))
        draw.text((x + 4, y + 2), f"~{i + 1} {float(np.dot(f['feat'], centre)):.2f}", fill=(255, 255, 0), font=font)
    path = OUT / f"similar_{pid.upper()}.png"
    sheet.save(path)
    (OUT / f"similar_{pid.upper()}.json").write_text(json.dumps(cands))
    print(f"{path}: similarity 1 = same face; the same person is usually above ~0.5")


def face_in(path) -> dict:
    """The largest face in one photo (for a photo added later, without scanning again)."""
    rgb = load_rgb(path)
    h, w = rgb.shape[:2]
    scale = min(1.0, DETECT_MAX / max(h, w))
    size = (int(w * scale), int(h * scale))
    det = cv2.FaceDetectorYN.create(facetrack._model(), "", size, 0.5)
    _, found = det.detect(cv2.resize(cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR), size, interpolation=cv2.INTER_AREA))
    if found is None:
        raise SystemExit(f"{path}: no face found")
    best = found[np.argmax(found[:, 2] * found[:, 3])]
    lm = best[4:14].reshape(5, 2) / scale
    return dict(photo=path.name, lm=lm.tolist(), iod=float(np.hypot(*(lm[1] - lm[0]))))


def pick(choices, first=None):
    data = json.loads((OUT / "people.json").read_text())
    people = {p["id"]: p for p in data["people"]}
    sel = []
    for c in choices:
        pid, alt = c[:3].upper(), (c[3:] or "a").lower()
        if Path(c).suffix.lower() in (".jpg", ".jpeg", ".png"):
            # a photo: as given, in private/family/ or in the photo folder (stored with its full path)
            path = next((p for p in (Path(c), OUT / c, Path(data["photo_dir"]) / c) if p.exists()), None)
            if path is None:
                raise SystemExit(f"{c}: not found")
            f = face_in(path.resolve())
            f["photo"] = str(path.resolve())
        elif alt.startswith("~"):   # from the similar sheet
            f = json.loads((OUT / f"similar_{pid}.json").read_text())[int(alt[1:]) - 1]
        else:
            f = data["faces"][people[pid]["faces"][LETTERS.index(alt)]]
        sel.append(dict(id=c, photo=f["photo"], lm=f["lm"], iod=f["iod"]))
    if first:
        if first not in choices:
            raise SystemExit(f"--first {first} is not among the chosen faces")
        sel.sort(key=lambda f: f["id"] != first)   # stable: the rest keeps its order
    (OUT / "selection.json").write_text(json.dumps(dict(photo_dir=data["photo_dir"], faces=sel,
                                                        first=bool(first)), indent=1))
    print(f"{len(sel)} faces selected -> {OUT / 'selection.json'}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan").add_argument("photo_dir", type=Path)
    sub.add_parser("group")
    sub.add_parser("show").add_argument("person")
    sub.add_parser("similar").add_argument("person")
    pk = sub.add_parser("pick")
    pk.add_argument("choices", nargs="+")
    pk.add_argument("--first", help="this face comes first (the shuffled order is the same every time)")
    a = p.parse_args()
    if a.cmd == "scan":
        scan(a.photo_dir)
    elif a.cmd == "group":
        group()
    elif a.cmd == "show":
        show(a.person)
    elif a.cmd == "similar":
        similar(a.person)
    else:
        pick(a.choices, a.first)
    return 0


if __name__ == "__main__":
    sys.exit(main())
