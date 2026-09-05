"""Task 1 -- held-out test of the log-linear interpolation used to produce
the iso-PSNR number (median 6.6x).

Doesn't need a fresh full run: c_paired_results.json already has LPIPS and
PSNR at strength=2 and strength=3 for gamma=1 (saved as the calibration
bracket, gamma1_bracket_lo/hi). This script adds strength=4, interpolates
log-LPIPS between strength=2 and strength=4 in log-PSNR space, evaluates
the prediction at strength=3's ACTUAL psnr, and compares against the
ALREADY-measured strength=3 LPIPS (gamma1_bracket_hi_lpips) -- a real
held-out point, not the one the original interpolation was fit to.

The original analysis interpolated within [2,3], a ~1dB-wide bracket. This
test interpolates within [2,4], ~3.5dB wide -- Opus's point: the tight
bracket proves the arithmetic, not that the underlying LPIPS-vs-PSNR curve
is actually log-linear over any real range. This is closer to a real test
of that.

Usage:
    python3 validate_interpolation.py <image_dir> c_paired_results.json [--n 20] [--seed 42]
"""
import argparse
import json
import subprocess

import numpy as np
from PIL import Image

TOOL = "./bin/imgpoison"
PAYLOAD = "poison"

try:
    import torch
    import lpips as lpips_pkg
    _LPIPS_NET = lpips_pkg.LPIPS(net="alex")
    _LPIPS_NET.eval()
    HAVE_LPIPS = True
except ImportError:
    HAVE_LPIPS = False
    print("[info] torch/lpips not importable -- using SSIM fallback "
          "(logic-only check, not a real result)")

from skimage.metrics import structural_similarity as ssim_fn


def compute_psnr(orig_path, embedded_path):
    orig = np.array(Image.open(orig_path).convert("RGB"), dtype=np.float64)
    emb = np.array(Image.open(embedded_path).convert("RGB"), dtype=np.float64)
    mse = np.mean((orig - emb) ** 2)
    if mse == 0:
        return 100.0
    return 10 * np.log10(255.0 ** 2 / mse)


def compute_quality(orig_path, embedded_path):
    if HAVE_LPIPS:
        orig = np.array(Image.open(orig_path).convert("RGB"), dtype=np.float32) / 127.5 - 1.0
        emb = np.array(Image.open(embedded_path).convert("RGB"), dtype=np.float32) / 127.5 - 1.0
        t1 = torch.from_numpy(orig).permute(2, 0, 1).unsqueeze(0)
        t2 = torch.from_numpy(emb).permute(2, 0, 1).unsqueeze(0)
        with torch.no_grad():
            d = _LPIPS_NET(t1, t2).item()
        return d
    orig = np.array(Image.open(orig_path).convert("L"))
    emb = np.array(Image.open(embedded_path).convert("L"))
    return ssim_fn(orig, emb, data_range=255)


def embed(image_path, out_path, seed, strength, gamma):
    cmd = [TOOL, "--embed", "--method", "ss", "--seed", str(seed),
           "--strength", str(strength), "--payload", PAYLOAD]
    if gamma is not None:
        cmd += ["--auto-mask", str(gamma)]
    cmd += [image_path, out_path]
    result = subprocess.run(cmd, capture_output=True)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image_dir")
    parser.add_argument("results_json")
    parser.add_argument("--n", type=int, default=20)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    with open(args.results_json) as f:
        records = json.load(f)

    # only images with a real, usable bracket (lo=2, hi=3 -- confirmed the
    # pattern from the main run, but check rather than assume)
    usable = [r for r in records
              if r.get("gamma1_bracket_lo_strength") is not None
              and r.get("gamma1_bracket_hi_strength") is not None
              and r["gamma1_bracket_hi_strength"] - r["gamma1_bracket_lo_strength"] == 1]

    subset = usable[:args.n]
    print(f"Testing on {len(subset)} images (of {len(usable)} with a usable bracket)\n")

    rel_errors = []
    for r in subset:
        name = r["name"]
        path = f"{args.image_dir}/{name}"

        s_lo = r["gamma1_bracket_lo_strength"]      # 2
        s_hi = r["gamma1_bracket_hi_strength"]       # 3, the held-out target
        psnr_lo = r["gamma1_bracket_lo_psnr"]
        psnr_hi_actual = r["gamma1_bracket_hi_psnr"]  # actual psnr at strength=3
        lpips_lo = r["gamma1_bracket_lo_lpips"]       # actual lpips at strength=2
        lpips_hi_actual = r["gamma1_bracket_hi_lpips"]  # actual lpips at strength=3 -- what we predict

        s_far = s_hi + 1  # strength=4, the new, wider bracket endpoint
        tmp = "/tmp/_val_far.png"
        if not embed(path, tmp, args.seed, s_far, 1.0):
            print(f"  [warn] {name}: embed at strength={s_far} failed, skipping")
            continue
        psnr_far = compute_psnr(path, tmp)
        lpips_far = compute_quality(path, tmp)

        # interpolate log-LPIPS between (psnr_lo, lpips_lo) and (psnr_far, lpips_far),
        # evaluated at the ACTUAL psnr of strength=3 -- predicting a held-out point,
        # not the point the bracket was fit to.
        frac = (psnr_lo - psnr_hi_actual) / (psnr_lo - psnr_far)
        lpips_pred = float(np.exp(np.log(lpips_lo) + frac * (np.log(lpips_far) - np.log(lpips_lo))))

        rel_err = abs(lpips_pred - lpips_hi_actual) / lpips_hi_actual
        rel_errors.append(rel_err)

        print(f"  {name}: bracket=[{s_lo},{s_far}]dB=[{psnr_lo:.2f},{psnr_far:.2f}]  "
              f"predicted(str={s_hi})={lpips_pred:.4f}  actual={lpips_hi_actual:.4f}  "
              f"rel_err={rel_err*100:.1f}%")

    if not rel_errors:
        print("\n[error] no usable predictions")
        return

    median_err = float(np.median(rel_errors)) * 100
    print(f"\n=== Task 1 result (n={len(rel_errors)}) ===")
    print(f"median relative error: {median_err:.2f}%")
    if median_err < 5:
        print("< 5%: confirms the log-linear shape over a real range, not just the tight bracket")
    elif median_err > 20:
        print("> 20%: the interpolation is extrapolating past where log-linear holds")
    else:
        print("between 5% and 20%: inconclusive by the stated criteria")


if __name__ == "__main__":
    main()

