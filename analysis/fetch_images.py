"""Fetch 100 real COCO val2017 photos into a local images/ folder.

Run this LOCALLY, once, before `docker build` -- not inside the Dockerfile.
Puts the images where `docker build`'s context can just COPY them in
directly: no network dependency during the build itself, no re-download on
every build, and you can look at what's in images/ before it goes anywhere.

Zip: reused if already on disk, not deleted afterward. If val2017.zip is
already present, asks before redownloading (it's ~1GB, no need to pull it
again just to resample) -- everything else about the sample (which 100,
what seed) still gets redone either way.

Photos: images/ is always cleared of existing photos before writing the
new selection, regardless of whether the zip was redownloaded or reused.
Mixing an old sample with a new one silently is worse than a clean slate
every time.

Filters for a minimum pixel count before sampling, not after: found the
hard way that COCO val2017's AVERAGE image (640x480 = 307,200 pixels) is
already below the ~393,000 pixels imgpoison needs for even a 6-byte
payload at PAYLOAD_REPEAT=3, CHIP_SIZE=512 -- an earlier attempt used a
7-byte payload and a 600,000-pixel filter, both guessed too high, and
every single one of the 5000 candidates got skipped. Every embed on an
undersized image fails outright ("payload too large for image")
regardless of --strength -- that's a capacity wall, not a PSNR-resolution
question.

Random sample (seeded), not the first N alphabetically -- val2017
filenames are just zero-padded image IDs, "first N" is whatever IDs sort
lowest, not a representative draw.

Usage:
    python3 fetch_images.py
Then in the Dockerfile: COPY images/ ./images/
"""
import io
import os
import random
import urllib.request
import zipfile

from PIL import Image

VAL2017_URL = "http://images.cocodataset.org/zips/val2017.zip"
ZIP_PATH = "val2017.zip"  # kept next to this script, reused across runs
OUT_DIR = "images"  # relative to wherever you run this -- put it next to the Dockerfile
N_IMAGES = 100
SEED = 0
MIN_PIXELS = 400_000  # margin above the ~393,000 the shorter payload below needs


def ensure_zip():
    if os.path.exists(ZIP_PATH):
        answer = input(f"{ZIP_PATH} already exists. Redownload it? [y/N] ").strip().lower()
        if answer != "y":
            print(f"Reusing existing {ZIP_PATH}")
            return
    print(f"Downloading {VAL2017_URL} (~1GB, this can take a few minutes)...")
    urllib.request.urlretrieve(VAL2017_URL, ZIP_PATH)


def clear_existing_photos():
    if not os.path.isdir(OUT_DIR):
        return
    existing = [f for f in os.listdir(OUT_DIR) if f.lower().endswith((".jpg", ".jpeg", ".png"))]
    if existing:
        print(f"Removing {len(existing)} existing photo(s) from {OUT_DIR}/")
        for f in existing:
            os.remove(os.path.join(OUT_DIR, f))


def main():
    ensure_zip()
    clear_existing_photos()
    os.makedirs(OUT_DIR, exist_ok=True)

    rng = random.Random(SEED)
    with zipfile.ZipFile(ZIP_PATH) as z:
        all_names = [n for n in z.namelist() if n.endswith(".jpg")]
        rng.shuffle(all_names)

        saved = 0
        checked = 0
        for name in all_names:
            if saved >= N_IMAGES:
                break
            checked += 1
            with z.open(name) as src:
                raw = src.read()
            try:
                w, h = Image.open(io.BytesIO(raw)).size
            except Exception:
                continue
            if w * h < MIN_PIXELS:
                continue
            dest = os.path.join(OUT_DIR, os.path.basename(name))
            with open(dest, "wb") as dst:
                dst.write(raw)
            saved += 1

    print(f"Extracted {saved} images to {OUT_DIR}/ (checked {checked}, "
          f"skipped {checked - saved} under {MIN_PIXELS} pixels).")
    print(f"{ZIP_PATH} kept on disk for reuse next time, not deleted.")
    print(f"Take a look at what's in {OUT_DIR}/ before running docker build.")


if __name__ == "__main__":
    main()

