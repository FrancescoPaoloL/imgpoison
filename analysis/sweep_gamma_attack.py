"""Task 4 -- gamma x attack-strength sweep using --bitacc, at the fragile
operating point (--chip-size 64 --payload-repeat 1) for real resolution --
see Task 3. Wilson score interval per point, not a normal approximation:
correct at small n and near 0/1 accuracy, where a normal interval can go
outside [0,1] or badly under-cover.

Two corrections from the first version of this sweep:

1. Sigma grid concentrated in [0.5, 1.2], not [0.3, 2.0]. Below ~0.6 the
   scheme is close to untouched (accuracy > 0.9, uninformative); above
   ~1.3 both attacks have already reduced it to chance. Points sitting at
   floor (bit accuracy indistinguishable from 0.5 by its own Wilson CI)
   are marked (floor) instead of compared -- a 0.53 vs 0.52 at the floor
   is comparing two dead watermarks, not measuring anything.

2. Compared by the ATTACKER'S OWN perceptual cost (LPIPS between stego
   and attacked image), not by sigma. Uniform blur and mask-targeted blur
   don't cost the same LPIPS at the same sigma or even at the same PSNR
   -- comparing them at matched sigma is the same flaw this project
   argues against in WaterVIB (attacks compared at 16dB vs 30dB PSNR
   without matching budget). Sigma is only used to generate points; the
   x-axis for comparison is each point's own measured LPIPS(stego,
   attacked).

Two attack families:

  1. Gaussian blur.

  2. Mask-recompute attack. An independent Python reimplementation of the
     same texture-tensor algorithm C uses (structure tensor -> gamma
     mask) -- Kerckhoffs's principle: the defender's placement strategy
     is public, an attacker doesn't need the tool's source, just the
     algorithm. Computes the mask from the RECEIVED image, concentrates
     BLUR (not noise -- this scheme is immune to additive noise, already
     measured) where that mask is highest, at a fixed PSNR budget -- but
     what actually gets reported is each point's own LPIPS cost, not the
     PSNR used to generate it.

Additive UNIFORM noise is skipped on purpose: already measured immune
(sigma=150, attack PSNR=8.5dB, payload intact) in earlier robustness work.

Usage:
    python3 sweep_gamma_attack.py <image> [--payload TEXT]
"""
import argparse
import subprocess
import re

import numpy as np
from PIL import Image, ImageFilter
from skimage.feature import structure_tensor, structure_tensor_eigenvalues

TOOL = "./bin/imgpoison"
SEED = 42
STRENGTH = 10
CHIP_SIZE = 64
PAYLOAD_REPEAT = 1
GAMMAS = [-1.0, -0.5, 0.0, 0.5, 1.0, 2.0]
# concentrated where accuracy actually moves (0.60-0.95), see module
# docstring -- was [0.3 .. 2.0], now [0.5 .. 1.2] with finer spacing.
BLUR_SIGMAS = [0.5, 0.58, 0.66, 0.74, 0.82, 0.90, 0.98, 1.06, 1.13, 1.20]
EPS = 0.01

try:
    import torch
    import lpips as lpips_pkg
    _LPIPS_NET = lpips_pkg.LPIPS(net="alex")
    _LPIPS_NET.eval()
    HAVE_LPIPS = True
except ImportError:
    HAVE_LPIPS = False
    print("[info] torch/lpips not importable -- using SSIM-based distance "
          "fallback for attacker cost (logic-only check, not a real result)")


def compute_perceptual_cost(a_rgb, b_rgb):
    """Attacker's own perceptual cost: LPIPS(stego, attacked), lower is
    less damage done to reach a given bit-accuracy reduction. Real LPIPS
    when available; SSIM-based distance (1-SSIM) as a same-direction
    fallback otherwise."""
    if HAVE_LPIPS:
        t1 = torch.from_numpy(a_rgb.astype(np.float32) / 127.5 - 1.0).permute(2, 0, 1).unsqueeze(0)
        t2 = torch.from_numpy(b_rgb.astype(np.float32) / 127.5 - 1.0).permute(2, 0, 1).unsqueeze(0)
        with torch.no_grad():
            return _LPIPS_NET(t1, t2).item()
    from skimage.metrics import structural_similarity as ssim_fn
    a_gray = np.array(Image.fromarray(a_rgb).convert("L"))
    b_gray = np.array(Image.fromarray(b_rgb).convert("L"))
    return 1.0 - ssim_fn(a_gray, b_gray, data_range=255)


# --- independent python reimplementation of the C structure-tensor mask,
# same algorithm as src/texture.c: sobel gradients (via skimage's
# structure_tensor), eigenvalues, energia*(1-coerenza), 99th-percentile
# normalization, additive floor, RMS-preserving gamma redistribution. ---

def compute_texture_map(image_uint8, sigma=1.5, pctl=99):
    img = image_uint8.astype(np.float64) / 255.0
    Axx, Axy, Ayy = structure_tensor(img, sigma=sigma, mode="reflect")
    l1, l2 = structure_tensor_eigenvalues([Axx, Axy, Ayy])

    energia = l1 + l2
    denom = np.where(energia > 1e-12, energia, 1e-12)
    coerenza = (l1 - l2) / denom
    texture = energia * (1 - coerenza)

    scale = max(np.percentile(texture, pctl), 1e-12)
    return np.clip(texture / scale, 0.0, 1.0)


def gamma_mask(texture_norm, gamma, eps=EPS, clip_range=(0.1, 10.0)):
    floored = (texture_norm + eps) / (1 + eps)
    M = floored ** gamma
    M = M / np.sqrt((M ** 2).mean())
    M = np.clip(M, clip_range[0], clip_range[1])
    M = M / np.sqrt((M ** 2).mean())
    return M


def compute_psnr(orig, attacked):
    mse = np.mean((orig.astype(np.float64) - attacked.astype(np.float64)) ** 2)
    return 100.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def mask_weighted_attack(orig_rgb, target_psnr, base_sigma=8.0, seed=0):
    """Blur damage concentrated where the recomputed mask is highest,
    calibrated (binary search on the blend fraction) to hit target_psnr.
    PSNR here is only the generation knob -- the number that actually
    gets compared across families is this attack's own LPIPS cost,
    computed separately after this returns."""
    gray = np.array(Image.fromarray(orig_rgb).convert("L"))
    tex = compute_texture_map(gray)
    weight = gamma_mask(tex, 1.0)
    weight = weight / weight.max()

    blurred_full = np.array(Image.fromarray(orig_rgb).filter(ImageFilter.GaussianBlur(base_sigma)))

    def attacked_at(mix_scale):
        mix = np.clip(weight * mix_scale, 0.0, 1.0)[..., None]
        out = orig_rgb.astype(np.float64) * (1 - mix) + blurred_full.astype(np.float64) * mix
        return np.clip(out, 0, 255).astype(np.uint8)

    lo, hi = 0.0, 50.0
    for _ in range(40):
        mid = (lo + hi) / 2
        psnr = compute_psnr(orig_rgb, attacked_at(mid))
        if psnr > target_psnr:
            lo = mid
        else:
            hi = mid
    return attacked_at((lo + hi) / 2)


def wilson_ci(successes, n, z=1.96):
    if n == 0:
        return (0.0, 1.0)
    phat = successes / n
    denom = 1 + z * z / n
    center = (phat + z * z / (2 * n)) / denom
    margin = (z * np.sqrt(phat * (1 - phat) / n + z * z / (4 * n * n))) / denom
    return (max(0.0, center - margin), min(1.0, center + margin))


def is_floor(lo, hi):
    """CI touches or straddles 0.5 (chance) -- not distinguishable from a
    dead watermark, comparing two such points measures nothing."""
    return lo <= 0.5


def embed(image_path, out_path, gamma):
    cmd = [TOOL, "--embed", "--method", "ss", "--seed", str(SEED),
           "--strength", str(STRENGTH), "--chip-size", str(CHIP_SIZE),
           "--payload-repeat", str(PAYLOAD_REPEAT), "--payload", PAYLOAD,
           "--auto-mask", str(gamma), image_path, out_path]
    subprocess.run(cmd, check=True, capture_output=True)


def bitacc(image_path, gamma):
    cmd = [TOOL, "--bitacc", "--payload", PAYLOAD, "--seed", str(SEED),
           "--chip-size", str(CHIP_SIZE), "--payload-repeat", str(PAYLOAD_REPEAT),
           "--auto-mask", str(gamma), image_path]
    result = subprocess.run(cmd, capture_output=True)
    stdout = result.stdout.decode("utf-8", errors="replace")
    m = re.search(r"Bit acc\s*:\s*([\d.]+)", stdout)
    if not m:
        return None
    return float(m.group(1))


def run_family(name, stego_path, stego_rgb, gamma, n_trials, make_attacked):
    """make_attacked(sigma) -> attacked_rgb. Returns list of
    (cost, acc, lo, hi, floor) sorted by cost ascending."""
    points = []
    for sigma in BLUR_SIGMAS:
        attacked_rgb = make_attacked(sigma)
        attacked_path = f"/tmp/_sweep_{name}_attacked.png"
        Image.fromarray(attacked_rgb).save(attacked_path)

        acc = bitacc(attacked_path, gamma)
        if acc is None:
            continue
        cost = compute_perceptual_cost(stego_rgb, attacked_rgb)
        successes = round(acc * n_trials)
        lo, hi = wilson_ci(successes, n_trials)
        points.append((cost, acc, lo, hi, is_floor(lo, hi)))
    points.sort(key=lambda p: p[0])
    return points


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--payload", default="poison test with much more room to spare")
    args = parser.parse_args()

    global PAYLOAD
    PAYLOAD = args.payload
    n_trials = len(PAYLOAD) * 8 * PAYLOAD_REPEAT

    orig_rgb = np.array(Image.open(args.image).convert("RGB"))

    print(f"n_trials per point (payload bits x repeat): {n_trials}")
    print("x-axis is each point's own LPIPS(stego, attacked) -- the attacker's "
          "perceptual cost -- not sigma. (floor) marks bit accuracy whose Wilson "
          "CI already touches 0.5 (chance): not a comparable measurement.\n")

    for gamma in GAMMAS:
        stego_path = f"/tmp/_sweep_g{gamma}.png"
        embed(args.image, stego_path, gamma)
        stego_rgb = np.array(Image.open(stego_path).convert("RGB"))

        print(f"=== gamma={gamma:.1f} ===")

        blur_points = run_family(
            "blur", stego_path, stego_rgb, gamma, n_trials,
            lambda sigma: np.array(Image.open(stego_path).filter(ImageFilter.GaussianBlur(sigma))))

        print("  family 1 (uniform blur):")
        for cost, acc, lo, hi, floor in blur_points:
            tag = " (floor)" if floor else ""
            print(f"    lpips={cost:.4f}  acc={acc:.3f} [{lo:.3f},{hi:.3f}]{tag}")

        def mask_attack_at(sigma):
            blurred = np.array(Image.open(stego_path).filter(ImageFilter.GaussianBlur(sigma)))
            target_psnr = compute_psnr(stego_rgb, blurred)
            return mask_weighted_attack(stego_rgb, target_psnr)

        mask_points = run_family("mask", stego_path, stego_rgb, gamma, n_trials, mask_attack_at)

        print("  family 2 (mask-recompute, blur concentrated by recomputed mask):")
        for cost, acc, lo, hi, floor in mask_points:
            tag = " (floor)" if floor else ""
            print(f"    lpips={cost:.4f}  acc={acc:.3f} [{lo:.3f},{hi:.3f}]{tag}")
        print()


if __name__ == "__main__":
    main()

