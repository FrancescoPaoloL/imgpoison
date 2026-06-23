#!/usr/bin/env python3
# test_robustness.py - test SS payload survival through image transformations
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
STEGO_BASE  = "img/stego_robustness"

# please note: expected limits with STRENGTH=10, CHIP_SIZE=512

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

def run_transforms(base):
    """apply the 5 standard transforms to base, return list of pass/fail."""
    results = []

    # test 1 - baseline: no transformation
    results.append(check("baseline (no transformation)", base))

    # test 2 - recompress at quality 90
    path = STEGO_BASE + "_q90.jpg"
    Image.open(base).save(path, quality=90)
    results.append(check("recompress q90", path))

    # test 3 - recompress at quality 85
    path = STEGO_BASE + "_q85.jpg"
    Image.open(base).save(path, quality=85)
    results.append(check("recompress q85", path))

    # test 4 - recompress at quality 75
    path = STEGO_BASE + "_q75.jpg"
    Image.open(base).save(path, quality=75)
    results.append(check("recompress q75", path))

    # test 5 - rotate 1 degree (geometric desync, not just quantization noise)
    path = STEGO_BASE + "_rot1.jpg"
    Image.open(base).rotate(1).save(path, quality=95)
    results.append(check("rotate 1 degree + save q95", path))

    return results

def main():
    print("imgpoison robustness tests")
    print("==========================")

    # two paths, because "does the payload survive" has two honest answers.
    #
    # path A - PIPELINE: embed straight to JPEG (as the README documents the
    #   tool's real output), then recompress. every case is a DOUBLE
    #   compression (q95 from the embed + the test's q). this is what a real
    #   user actually subjects the image to.
    #
    # path B - ALGORITHM: embed to lossless PNG, then a SINGLE recompression.
    #   isolates the spread-spectrum scheme's own robustness from the extra
    #   compression the JPEG output format adds.

    print("\n[A] pipeline (embed->JPEG q95, the README flow):")
    base_jpg = STEGO_BASE + ".jpg"
    embed(base_jpg)
    res_a = run_transforms(base_jpg)

    print("\n[B] algorithm (embed->PNG lossless, single recompress):")
    base_png = STEGO_BASE + ".png"
    embed(base_png)
    res_b = run_transforms(base_png)

    print()
    pa, pb = sum(res_a), sum(res_b)
    print(f"[A] pipeline : {pa}/{len(res_a)} passed")
    print(f"[B] algorithm: {pb}/{len(res_b)} passed")
    print()
    print("note: rotate is geometric desync (the seeded pixel permutation")
    print("      points to pixels that moved under interpolation), not a")
    print("      quantization problem - out of scope, fails with magic mismatch.")
    sys.exit(0 if (pa + pb) == (len(res_a) + len(res_b)) else 1)

if __name__ == "__main__":
    main()

