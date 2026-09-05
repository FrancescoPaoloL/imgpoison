"""Paired gamma=0 vs gamma=1 comparison, driving imgpoison's real C binary
(bin/imgpoison), not the old Python naive_embed stub.

For each image: calibrate --strength (the tool's real, public knob) per
gamma to hit a target PSNR, then measure LPIPS between original and
embedded. Saves every per-image result plus the seed, not just the
aggregate -- losing the per-image data once already made this impossible
to compute after the fact.

Three statistics, all on the SAME paired per-image data:
  1. Wilcoxon signed-rank on the paired (gamma0, gamma1) LPIPS values --
     tests whether the advantage is real, not just a mean-of-two-samples
     comparison.
  2. Median and IQR of the ratio gamma0/gamma1 -- robust to a single
     odd image the way a mean isn't.
  3. Bootstrap: resample the images (not the pixels) 1000 times, look at
     where the median ratio falls -- an interval, not a single number.

Usage:
    python3 run_c_paired_suite.py <image_dir> [--psnr-target 42.3] [--seed 42]
"""
import argparse
import json
import os
import subprocess

import numpy as np
from PIL import Image
from scipy.stats import wilcoxon
from skimage.metrics import structural_similarity as ssim_fn

TOOL = "./bin/imgpoison"
PAYLOAD = "poison"  # short on purpose -- must match fetch_images.py's MIN_PIXELS filter

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


def compute_psnr(orig_path, embedded_path):
    orig = np.array(Image.open(orig_path).convert("RGB"), dtype=np.float64)
    emb = np.array(Image.open(embedded_path).convert("RGB"), dtype=np.float64)
    mse = np.mean((orig - emb) ** 2)
    if mse == 0:
        return 100.0
    return 10 * np.log10(255.0 ** 2 / mse)


def compute_quality(orig_path, embedded_path):
    """Returns (score, higher_is_better). Real LPIPS when available (lower
    is better), SSIM fallback otherwise (higher is better)."""
    if HAVE_LPIPS:
        orig = np.array(Image.open(orig_path).convert("RGB"), dtype=np.float32) / 127.5 - 1.0
        emb = np.array(Image.open(embedded_path).convert("RGB"), dtype=np.float32) / 127.5 - 1.0
        t1 = torch.from_numpy(orig).permute(2, 0, 1).unsqueeze(0)
        t2 = torch.from_numpy(emb).permute(2, 0, 1).unsqueeze(0)
        with torch.no_grad():
            d = _LPIPS_NET(t1, t2).item()
        return d, False
    orig = np.array(Image.open(orig_path).convert("L"))
    emb = np.array(Image.open(embedded_path).convert("L"))
    return ssim_fn(orig, emb, data_range=255), True


def embed_with_strength(image_path, out_path, seed, strength, gamma):
    """Returns (ok, stderr_text). stderr_text is empty on success -- kept
    even on success in case the tool ever warns without failing."""
    cmd = [TOOL, "--embed", "--method", "ss", "--seed", str(seed),
           "--strength", str(int(round(strength))), "--payload", PAYLOAD]
    if gamma is not None:
        cmd += ["--auto-mask", str(gamma)]
    cmd += [image_path, out_path]
    result = subprocess.run(cmd, capture_output=True)
    stderr = result.stderr.decode("utf-8", errors="replace").strip()
    return result.returncode == 0, stderr


def calibrate_strength(image_path, seed, gamma, target_psnr,
                        strength_bounds=(1, 60)):
    """Integer binary search on --strength -- imgpoison's real, ONLY-
    integer public parameter -- for the value closest to target_psnr.

    Not continuous bisection with a tolerance check: once the search
    narrows to two adjacent integers, there is nothing finer to test.
    Picking whichever of the two is closer to target IS the answer, not
    a failure to converge further.

    An earlier version kept bisecting a continuous 'mid' value past that
    point. Every mid within the same rounding bucket (e.g. anything in
    [1.5, 2.5) rounds to strength=2 for the actual embed call) produced
    the IDENTICAL embedded image and PSNR -- so the search either landed
    on the true answer by coincidence or burned through max_iter unable
    to ever satisfy a continuous tolerance that a discrete knob cannot
    supply. Confirmed on a real run: every single image converged to the
    exact same float (1.921875, displayed 1.922) after exactly 12
    iterations -- not because every image needed the same strength, but
    because every image's search took the identical dead path through
    the same rounding bucket.

    Returns (best_s, best_p, lo, p_lo, hi, p_hi, error). lo/hi are the
    final adjacent-integer bracket (hi=lo+1) with p_lo >= target >= p_hi
    -- returned, not discarded, because interpolating between them (see
    main()) gives a true per-image iso-PSNR comparison for gamma=1
    against gamma=0's own achieved PSNR, without assuming any particular
    LPIPS-vs-energy scaling law. No tolerance failure mode: the only
    errors are an embed failure or the target being unreachable at all
    within strength_bounds. The residual gap between best_p and
    target_psnr is real and should be reported per image, not hidden --
    but it isn't a bug to raise on, it's the tool's actual resolution at
    this operating point."""
    lo, hi = strength_bounds
    tmp = "/tmp/_calib.png"

    def psnr_at(s_int):
        ok, err = embed_with_strength(image_path, tmp, seed, s_int, gamma)
        if not ok:
            return None, err
        return compute_psnr(image_path, tmp), None

    p_lo, err_lo = psnr_at(lo)
    if p_lo is None:
        return None, None, None, None, None, None, err_lo or f"embed failed at strength={lo}"
    p_hi, err_hi = psnr_at(hi)
    if p_hi is None:
        return None, None, None, None, None, None, err_hi or f"embed failed at strength={hi}"

    if not (p_lo >= target_psnr >= p_hi):
        return None, None, None, None, None, None, (
            f"target {target_psnr} not bracketed: "
            f"strength={lo} -> {p_lo:.2f}dB, strength={hi} -> {p_hi:.2f}dB")

    best_s, best_p = ((lo, p_lo) if abs(p_lo - target_psnr) < abs(p_hi - target_psnr)
                       else (hi, p_hi))

    while hi - lo > 1:
        mid = (lo + hi) // 2
        p_mid, err_mid = psnr_at(mid)
        if p_mid is None:
            return None, None, None, None, None, None, err_mid or f"embed failed at strength={mid}"
        if abs(p_mid - target_psnr) < abs(best_p - target_psnr):
            best_s, best_p = mid, p_mid
        if p_mid > target_psnr:
            lo, p_lo = mid, p_mid
        else:
            hi, p_hi = mid, p_mid

    return best_s, best_p, lo, p_lo, hi, p_hi, None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image_dir")
    parser.add_argument("--psnr-target", type=float, default=42.3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--out", default="c_paired_results.json")
    args = parser.parse_args()

    names = sorted(p for p in os.listdir(args.image_dir)
                   if p.lower().endswith((".png", ".jpg", ".jpeg")))

    records = []
    for name in names:
        path = os.path.join(args.image_dir, name)
        row = {"name": name, "seed": args.seed}

        s0, p0, _, _, _, _, err0 = calibrate_strength(path, args.seed, None, args.psnr_target)
        s1, p1, b1_lo, b1_p_lo, b1_hi, b1_p_hi, err1 = calibrate_strength(
            path, args.seed, 1.0, args.psnr_target)

        if err0 or err1:
            if err0:
                print(f"  [warn] {name} gamma=0: {err0}")
            if err1:
                print(f"  [warn] {name} gamma=1: {err1}")
            row["gamma0_lpips"] = None
            row["gamma1_lpips"] = None
            row["gamma1_lpips_interp"] = None
            records.append(row)
            continue

        e0, e1 = "/tmp/_g0.png", "/tmp/_g1.png"
        embed_with_strength(path, e0, args.seed, s0, None)
        embed_with_strength(path, e1, args.seed, s1, 1.0)

        q0, higher_is_better = compute_quality(path, e0)
        q1, _ = compute_quality(path, e1)

        # true per-image iso-PSNR point for gamma=1: log-interpolate LPIPS
        # between the two bracket strengths (b1_lo, b1_hi) at gamma=0's
        # OWN achieved PSNR (p0), not gamma=1's own target-nearest point.
        # avoids assuming any particular LPIPS-vs-energy scaling law --
        # only interpolates locally between two real measurements. costs
        # one extra LPIPS call (the bracket's other endpoint) reusing
        # strengths the calibration search already had to test, not a new
        # embed sweep.
        e1_lo, e1_hi = "/tmp/_g1_lo.png", "/tmp/_g1_hi.png"
        embed_with_strength(path, e1_lo, args.seed, b1_lo, 1.0)
        embed_with_strength(path, e1_hi, args.seed, b1_hi, 1.0)
        q1_lo, _ = compute_quality(path, e1_lo)
        q1_hi, _ = compute_quality(path, e1_hi)

        if b1_p_lo == b1_p_hi or not (b1_p_hi <= p0 <= b1_p_lo):
            # p0 not actually inside gamma=1's bracket -- can't interpolate
            # safely, fall back to the nearer bracket endpoint's LPIPS.
            q1_interp = q1_lo if abs(b1_p_lo - p0) < abs(b1_p_hi - p0) else q1_hi
        else:
            frac = (b1_p_lo - p0) / (b1_p_lo - b1_p_hi)
            q1_interp = float(np.exp(np.log(q1_lo) + frac * (np.log(q1_hi) - np.log(q1_lo))))

        row.update({
            "gamma0_strength": s0, "gamma0_psnr": p0, "gamma0_lpips": q0,
            "gamma1_strength": s1, "gamma1_psnr": p1, "gamma1_lpips": q1,
            "gamma1_bracket_lo_strength": b1_lo, "gamma1_bracket_lo_psnr": b1_p_lo,
            "gamma1_bracket_lo_lpips": q1_lo,
            "gamma1_bracket_hi_strength": b1_hi, "gamma1_bracket_hi_psnr": b1_p_hi,
            "gamma1_bracket_hi_lpips": q1_hi,
            "gamma1_lpips_interp": q1_interp,
        })
        records.append(row)
        print(f"  {name}: g0={q0:.4f} (str={s0}, psnr={p0:.2f})  "
              f"g1={q1:.4f} (str={s1}, psnr={p1:.2f})  "
              f"g1_interp={q1_interp:.4f} (@psnr={p0:.2f}, from str={b1_lo}/{b1_hi})")

    with open(args.out, "w") as f:
        json.dump(records, f, indent=2)
    print(f"\nSaved {len(records)} per-image records to {args.out}")

    complete = [r for r in records
                if r.get("gamma0_lpips") is not None and r.get("gamma1_lpips") is not None]
    print(f"n={len(complete)}/{len(records)} complete pairs")
    if len(complete) < 2:
        print("[error] not enough complete pairs for statistics")
        return

    g0 = np.array([r["gamma0_lpips"] for r in complete])
    g1 = np.array([r["gamma1_lpips"] for r in complete])
    g1_interp = np.array([r["gamma1_lpips_interp"] for r in complete])

    stat, p_value = wilcoxon(g0, g1)
    stat_i, p_value_i = wilcoxon(g0, g1_interp)

    ratio = g0 / g1
    median_ratio = float(np.median(ratio))
    q25, q75 = np.percentile(ratio, [25, 75])

    ratio_i = g0 / g1_interp
    median_ratio_i = float(np.median(ratio_i))
    q25_i, q75_i = np.percentile(ratio_i, [25, 75])

    rng = np.random.default_rng(args.seed)
    n = len(complete)
    boot_medians = [np.median(g0[idx] / g1[idx])
                    for idx in (rng.integers(0, n, n) for _ in range(1000))]
    ci_lo, ci_hi = np.percentile(boot_medians, [2.5, 97.5])

    boot_medians_i = [np.median(g0[idx] / g1_interp[idx])
                      for idx in (rng.integers(0, n, n) for _ in range(1000))]
    ci_lo_i, ci_hi_i = np.percentile(boot_medians_i, [2.5, 97.5])

    psnr0 = np.array([r["gamma0_psnr"] for r in complete])
    psnr1 = np.array([r["gamma1_psnr"] for r in complete])
    psnr_gap = psnr1 - psnr0
    gap_corr = float(np.corrcoef(psnr_gap, np.log(ratio))[0, 1])

    print(f"\n=== paired statistics, RAW (at each gamma's own nearest strength, n={n}) ===")
    print(f"PSNR gap (g1-g0)       : mean={psnr_gap.mean():+.3f}dB  "
          f"min={psnr_gap.min():+.3f}  max={psnr_gap.max():+.3f}  "
          f"-- real, --strength is integer-only, this is not exactly iso-PSNR")
    print(f"    correlation(gap, log(ratio)): {gap_corr:.3f}  "
          f"-- across-image only, cannot see a near-constant offset, see interpolated below")
    print(f"1. Wilcoxon signed-rank : stat={stat:.1f}  p={p_value:.2e}")
    print(f"2. median ratio         : {median_ratio:.3f}  IQR [{q25:.3f}, {q75:.3f}]")
    print(f"3. bootstrap 95% CI     : [{ci_lo:.3f}, {ci_hi:.3f}]  (on the median ratio, 1000 resamples)")

    print(f"\n=== paired statistics, INTERPOLATED (gamma=1 log-LPIPS interpolated "
          f"to gamma=0's own PSNR, per image, n={n}) ===")
    print(f"this is the clean iso-PSNR number: no scaling-law assumption, no "
          f"across-image regression, just two real measurements per image")
    print(f"1. Wilcoxon signed-rank : stat={stat_i:.1f}  p={p_value_i:.2e}")
    print(f"2. median ratio         : {median_ratio_i:.3f}  IQR [{q25_i:.3f}, {q75_i:.3f}]")
    print(f"3. bootstrap 95% CI     : [{ci_lo_i:.3f}, {ci_hi_i:.3f}]  (on the median ratio, 1000 resamples)")


if __name__ == "__main__":
    main()

