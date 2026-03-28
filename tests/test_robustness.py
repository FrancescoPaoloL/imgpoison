#!/usr/bin/env python3
# test_robustness.py — test SS payload survival through image transformations
# usage: python tests/test_robustness.py
#
# runs the tool through a series of transformations and checks
# that the payload is recovered correctly after each one.

import subprocess
import sys
from PIL import Image

# paths
TOOL        = "./bin/imgpoison"
ORIGINAL    = "img/original.png"
PAYLOAD     = "This is a poisoned prompt"
SEED        = "42"
STRENGTH    = "10"
STEGO_BASE  = "/tmp/stego_robustness"

# please note: expected limits with STRENGTH=10, CHIP_SIZE=256

def embed(output):
    """embed payload into original image and save to output."""
    subprocess.run([
        TOOL, "--embed", "--method", "ss",
        "--seed", SEED, "--strength", STRENGTH,
        "--payload", PAYLOAD,
        ORIGINAL, output
    ], check=True, capture_output=True)

def extract(path):
    """extract payload from image, return the payload string."""
    result = subprocess.run([
        TOOL, "--extract", "--method", "ss",
        "--seed", SEED, path
    ], check=True, capture_output=True, text=True)
    # parse "Payload  : <text>" from stdout
    for line in result.stdout.splitlines():
        if line.startswith("Payload"):
            return line.split(":", 1)[1].strip()
    return None

def check(label, path):
    """extract and verify payload, print result."""
    try:
        got = extract(path)
        ok  = got == PAYLOAD
        status = "PASS" if ok else "FAIL"
        print(f"  {status}  {label}")
        if not ok:
            print(f"       expected: {PAYLOAD}")
            print(f"       got:      {got}")
        return ok
    except subprocess.CalledProcessError as e:
        print(f"  FAIL  {label}")
        print(f"       {e.stderr.strip()}")
        return False

def main():
    print("imgpoison robustness tests")
    print("==========================")

    # start from a fresh stego JPEG
    base = STEGO_BASE + ".jpg"
    embed(base)

    results = []

    # test 1 — baseline: no transformation
    results.append(check("baseline (no transformation)", base))

    # test 2 — recompress at quality 90
    path = STEGO_BASE + "_q90.jpg"
    Image.open(base).save(path, quality=90)
    results.append(check("recompress q90", path))

    # test 3 — recompress at quality 85
    path = STEGO_BASE + "_q85.jpg"
    Image.open(base).save(path, quality=85)
    results.append(check("recompress q85", path))

    # test 4 — recompress at quality 75
    path = STEGO_BASE + "_q75.jpg"
    Image.open(base).save(path, quality=75)
    results.append(check("recompress q75", path))

    # test 5 — rotate 1 degree (introduces interpolation noise)
    path = STEGO_BASE + "_rot1.jpg"
    Image.open(base).rotate(1).save(path, quality=95)
    results.append(check("rotate 1 degree + save q95", path))

    print()
    passed = sum(results)
    total  = len(results)
    print(f"{passed}/{total} tests passed")
    print()
    print("note: q85 and below are known limits at STRENGTH=10.")
    print("      increase --strength to improve robustness at the cost of visibility.")
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()

