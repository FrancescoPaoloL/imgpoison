"""
    Fetch 100 real COCO val2017 photos into a local images/ folder.

    Run this LOCALLY, once, before `docker build` -- not inside the Dockerfile.
    Puts the images where `docker build`'s context can just COPY them in
    directly: no network dependency during the build itself, no re-download on
    every build, and you can look at what's in images/ before it goes anywhere.

    Random sample, not the first N alphabetically -- alphabetical-first is not
    a random sample of COCO (val2017 filenames are just zero-padded image IDs,
    so "first N" is whatever IDs happen to sort lowest, not a representative
    draw). Seeded (SEED=0) so the sample is still reproducible without needing
    to remember which 100 files were picked.

    Full val2017.zip is ~1GB (no simpler way to fetch a subset of COCO without
    it -- individual images are only reachable by exact filename, and getting
    those filenames means the annotations file, itself ~241MB, or the zip's own
    listing, which is what this does). The zip is deleted after extracting the
    100 files -- it never touches the images/ folder itself.

    Usage:
        python3 fetch_images.py
    Then in the Dockerfile: COPY images/ ./images/
"""
import os
import random
import shutil
import urllib.request
import zipfile

VAL2017_URL = "http://images.cocodataset.org/zips/val2017.zip"
ZIP_PATH = "/tmp/val2017.zip"
OUT_DIR = "images"  # relative to wherever you run this -- put it next to the Dockerfile
N_IMAGES = 100
SEED = 0


def main():
    print(f"Downloading {VAL2017_URL} (~1GB, this can take a few minutes)...")
    urllib.request.urlretrieve(VAL2017_URL, ZIP_PATH)

    os.makedirs(OUT_DIR, exist_ok=True)
    with zipfile.ZipFile(ZIP_PATH) as z:
        all_names = [n for n in z.namelist() if n.endswith(".jpg")]
        names = random.Random(SEED).sample(all_names, N_IMAGES)
        for name in names:
            dest = os.path.join(OUT_DIR, os.path.basename(name))
            with z.open(name) as src, open(dest, "wb") as dst:
                shutil.copyfileobj(src, dst)

    os.remove(ZIP_PATH)
    print(f"Extracted {len(names)} images to {OUT_DIR}/, removed the zip.")
    print(f"Take a look at what's in {OUT_DIR}/ before running docker build.")


if __name__ == "__main__":
    main()

