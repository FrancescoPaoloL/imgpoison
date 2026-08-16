"""
imgpoison
mask + calibration prototype (standalone, no imgpoison dependency)

Validates the risky math from the brief before it touches the real codebase:

  - structure-tensor texture map (energy, coherence), normalized against the
    99th percentile instead of the max, so a few outlier pixels don't set
    the scale for the whole map

  - energy-preserving gamma mask: E[M**2] == 1 for every gamma, so gamma
    only redistributes where the payload goes, and gain is the only knob
    for how much; an earlier version confounded the two (see gamma_mask
    docstring)

  - regression check: on a flat image the mask must be ~1.0 for every gamma
    in the grid, not just gamma=0

  - calibration search dynamics (binary search on gain), against real LPIPS
    when torch/lpips are importable, SSIM as an automatic offline fallback

Same file runs everywhere:
    - in a sandbox with no torch it silently drops to the SSIM fallback
      (logic-only check, numbers not meaningful)
    - in the Docker image built from the Dockerfile in this folder,
      torch+lpips are installed, so it uses real LPIPS automatically
"""

import os

import numpy as np
from PIL import Image
from skimage import data
from skimage.feature import structure_tensor, structure_tensor_eigenvalues
from skimage.metrics import structural_similarity as ssim
from skimage.metrics import peak_signal_noise_ratio as psnr
import matplotlib.pyplot as plt

try:
    import torch
    import lpips as lpips_pkg
    _LPIPS_NET = lpips_pkg.LPIPS(net="alex")
    _LPIPS_NET.eval()
    HAVE_LPIPS = True
except ImportError:
    HAVE_LPIPS = False
    print("[info] torch/lpips not importable —> using SSIM fallback proxy "
          "(logic-only check, do not trust the numbers)")

RNG_SEED = 0
GAMMA_GRID = [-1, -0.5, 0, 0.5, 1, 2]
EPS = 0.01  # additive floor before pow(), see gamma_mask docstring re: checkerboard degeneracy


# structure-tensor mask

def compute_texture_map(image_uint8, sigma=1.5, pctl=99):
    """
        energy = l1+l2, coherence = (l1-l2)/(l1+l2), texture = energy*(1-coherence).

        Normalized against the pctl-th percentile (default 99th), not the max:
        on checkerboard a few outlier pixels (corner crossings) were setting
        the scale for the whole map. Found in review (Opus).
    """
    img = image_uint8.astype(np.float64) / 255.0
    # mode='reflect': skimage's default (mode='constant', cval=0) pads with
    # zeros and fabricates a fake edge at every border (found by testing on
    # a flat image: max energy 1.29 with default mode, 0.0 with reflect).
    Axx, Axy, Ayy = structure_tensor(img, sigma=sigma, mode="reflect")
    l1, l2 = structure_tensor_eigenvalues([Axx, Axy, Ayy])  # l1 >= l2 elementwise

    energy = l1 + l2
    denom = np.where(energy > 1e-12, energy, 1e-12)
    coherence = (l1 - l2) / denom
    texture = energy * (1 - coherence)

    scale = max(np.percentile(texture, pctl), 1e-12)
    texture_norm = np.clip(texture / scale, 0.0, 1.0)
    return texture_norm, coherence


def gamma_mask(texture_norm, gamma, eps=EPS, clip_range=(0.1, 10.0)):
    """
        L2-preserving: E[M**2] == 1 for every gamma, so a given `gain` in
        naive_embed delivers the same total MSE budget regardless of gamma.
        gamma decides *where* that budget goes; gain is the only knob for
        *how much* there is.

        Three bugs found in review (Opus):

        1. Original version returned clip(texture, eps, 1)**gamma
           unnormalized. On checkerboard, mean(mask) dropped ~30x from
           gamma=0 to gamma=1 at fixed gain -- gamma and gain were
           confounded, exactly what calibration exists to prevent.

        2. The floor was a hard clip(texture, eps, 1), which collapsed every
           pixel below eps to the same value and lost their ordering. On
           large flat regions (checkerboard's flat squares, or sky/
           background in real photos) this flattened gamma=-1 and gamma=2
           into a plateau instead of a distribution. The affine floor
           (t+eps)/(1+eps) keeps below-floor pixels ordered instead of
           collapsing them.

        3. The important one: normalizing to E[M]=1 (L1) is not the same
           as keeping perturbation energy comparable across gamma.
           naive_embed's MSE depends on E[M**2] (L2), and E[M**2] >=
           E[M]**2 by Jensen's inequality, with equality only at gamma=0.
           A peaky mask (gamma=1 had RMS ~3.3 despite mean=1) delivers far
           more MSE than a flat one at the same gain, so part of the
           earlier gamma=0-vs-gamma=1 LPIPS gap was this artifact, not a
           genuine placement effect. Normalizing to E[M**2]=1 instead
           keeps PSNR roughly constant at fixed gain across gamma, so gain
           finally means the same thing everywhere.

        Confirmed on the real Azure run: at fixed gain, the PSNR gap
        between gamma=0 and gamma=1 dropped from ~4dB (pre-fix) to ~1.5dB
        (post-fix). The residual ~1.5dB is accepted: the clip step
        perturbs the distribution after the first RMS normalization,
        which the second normalization doesn't fully undo.
    """
    texture_floored = (texture_norm + eps) / (1 + eps)
    M = texture_floored ** gamma
    M = M / np.sqrt((M ** 2).mean())
    M = np.clip(M, clip_range[0], clip_range[1])
    M = M / np.sqrt((M ** 2).mean())
    return M


def regression_test_gamma_invariance_on_flat(flat_image_uint8):
    """
        On a flat image there's no structure to redistribute, so the mask
        must be ~1.0 for every gamma, not just gamma=0. The old test only
        checked gamma=0 and missed gamma=1 silently returning 0.0010
        uniform on the same image -- this assertion catches that
        immediately.
    """
    texture_norm, _ = compute_texture_map(flat_image_uint8)
    all_ok = True
    for g in GAMMA_GRID:
        m = gamma_mask(texture_norm, g)
        ok = bool(np.allclose(m, 1.0, atol=1e-6))
        all_ok = all_ok and ok
        print(f"[regression] flat image, gamma={g:>5} -> mask≈1: "
              f"{'PASS' if ok else 'FAIL'} (min={m.min():.6f}, max={m.max():.6f})")
    return all_ok


# Step 1: calibration search (real LPIPS, SSIM fallback)

def naive_embed(image_uint8, mask, gain, seed=RNG_SEED):
    """
        Stand-in payload: continuous per-pixel pattern in [-1, 1] scaled by
        mask*gain. Not imgpoison's real embedding -- only here to exercise
        the calibration search against something that degrades quality as
        gain grows.

        Was originally a discrete +-1 choice, which made naive_embed a step
        function of gain: at gamma=0 every pixel's perturbation is exactly
        +-gain, so the rounded uint8 output stayed identical across a whole
        range of gain then jumped by thousands of pixels at once at each
        integer boundary. Once bisection narrowed into one of those flat
        plateaus it couldn't converge -- not a search bug, just too coarse
        an output space for a tight target (0.003). The continuous pattern
        makes different pixels cross their rounding threshold at different
        gains, so nearby gain values produce gradually different images
        instead of one synchronized jump.
    """
    rng = np.random.default_rng(seed)
    pattern = rng.uniform(-1.0, 1.0, size=image_uint8.shape)
    img = image_uint8.astype(np.float64)
    perturbed = img + gain * mask * pattern
    return np.clip(perturbed, 0, 255).astype(np.uint8)


def _to_lpips_tensor(img_uint8):
    """Grayscale HxW uint8 -> (1,3,H,W) float tensor in [-1, 1], the shape
    lpips.LPIPS expects (trained on RGB)."""
    x = img_uint8.astype(np.float32) / 127.5 - 1.0
    x = np.stack([x, x, x], axis=0)
    return torch.from_numpy(x).unsqueeze(0)


def proxy_quality(orig_uint8, embedded_uint8):
    """
        Returns (score, higher_is_better). Real LPIPS distance when
        available (lower = more similar), SSIM fallback otherwise (higher
        = more similar). calibrate_to_quality reads the flag instead of
        assuming a direction.
    """
    if HAVE_LPIPS:
        with torch.no_grad():
            d = _LPIPS_NET(_to_lpips_tensor(orig_uint8), _to_lpips_tensor(embedded_uint8)).item()
        return d, False
    return ssim(orig_uint8, embedded_uint8, data_range=255), True


def psnr_quality(orig_uint8, embedded_uint8):
    """PSNR in dB, higher is better. Unlike LPIPS/SSIM this is a global,
    placement-blind measure of total squared error -- it doesn't care
    where the error is, only how much. Used as a second metric to check
    whether an LPIPS gap between gamma values reflects genuine perceptual
    masking or just LPIPS's own sensitivity to error concentration.

    Guards the identical-images case explicitly: gain=0 means true MSE=0,
    true PSNR=+inf. skimage returns inf directly, which can silently
    poison a later mean/average. Returns a large finite sentinel instead."""
    if np.array_equal(orig_uint8, embedded_uint8):
        return 100.0, True  # far above any realistic image PSNR; unambiguous "basically identical"
    return psnr(orig_uint8, embedded_uint8, data_range=255), True


def calibrate_to_quality(image_uint8, mask, target_quality, metric_fn=proxy_quality,
                          gain_bounds=(0.0, 80.0), rel_tol=0.02, max_iter=40,
                          seed=RNG_SEED, label=""):
    """Binary search on gain so metric_fn(orig, embed(gain)) ~= target_quality.
    Direction (higher_is_better) comes from metric_fn itself, not assumed.
    metric_fn defaults to proxy_quality (LPIPS/SSIM) but also takes
    psnr_quality, for the equal-PSNR reverse test in main().

    Tolerance is relative to target (rel_tol * target), not absolute: an
    absolute tol=0.005 was fine at target=0.05 (10% relative), but became
    167% relative once target dropped to 0.003 -- the search "converged"
    on the first bisection point it landed near. Found reviewing a run
    against target=0.003 (Opus): both gamma=0 and gamma=1 reported
    "converged" at gain=5.0000 after 3 iterations, exactly where bisection
    on [0,40] lands after 3 steps, while the LPIPS actually reached was
    over 2x the target.

    Raises instead of returning a best-effort answer if max_iter is
    exhausted: a number that looks calibrated but isn't is worse than no
    answer. A raised exception here means the target isn't reachable in
    gain_bounds -- widen the bounds or fix the target, don't paper over it.

    Default upper bound doubled (40 -> 80) after switching naive_embed's
    pattern from discrete +-1 to continuous uniform(-1,1): the continuous
    pattern has half the expected magnitude, so the same gain now delivers
    roughly half the old perturbation."""
    lo, hi = gain_bounds
    q_lo, higher_is_better = metric_fn(image_uint8, naive_embed(image_uint8, mask, lo, seed))
    q_hi, _ = metric_fn(image_uint8, naive_embed(image_uint8, mask, hi, seed))

    bracket_ok = (q_lo >= target_quality >= q_hi) if higher_is_better else (q_lo <= target_quality <= q_hi)
    if not bracket_ok:
        print(f"  [warn] {label} target {target_quality:.4f} not bracketed by "
              f"[gain={lo}: q={q_lo:.4f}, gain={hi}: q={q_hi:.4f}] "
              f"(higher_is_better={higher_is_better}) — widen gain_bounds")
        return None

    tol = rel_tol * abs(target_quality)
    q_mid, mid = None, None
    for i in range(max_iter):
        mid = (lo + hi) / 2
        q_mid, _ = metric_fn(image_uint8, naive_embed(image_uint8, mask, mid, seed))
        if abs(q_mid - target_quality) <= tol:
            return mid, i + 1, q_mid
        move_lo = (q_mid > target_quality) if higher_is_better else (q_mid < target_quality)
        if move_lo:
            lo = mid
        else:
            hi = mid

    raise RuntimeError(
        f"calibration failed to converge: {label} target={target_quality}, "
        f"reached={q_mid}, gain={mid}, tol={tol:.5f} after {max_iter} iters"
    )


def aggregate_psnr(image_pairs, data_range=255):
    """Combine PSNR across multiple (orig, embedded) pairs correctly:
    average the MSE in linear domain across images, then convert to dB
    once at the end. Averaging PSNR values already in dB is a geometric
    mean of the underlying MSEs, which understates the true aggregate MSE
    whenever distortion varies a lot across images. Not yet exercised on
    a real multi-image set (no photos available in this sandbox) --
    self-tested below on the three synthetic images to confirm the gap
    is real before this runs on data that matters."""
    mses = []
    for orig, embedded in image_pairs:
        if np.array_equal(orig, embedded):
            mses.append(0.0)
        else:
            diff = orig.astype(np.float64) - embedded.astype(np.float64)
            mses.append(float(np.mean(diff ** 2)))
    mean_mse = float(np.mean(mses))
    if mean_mse == 0.0:
        return 100.0
    return float(10 * np.log10((data_range ** 2) / mean_mse))


def _self_test_psnr_aggregation():
    """Mechanism check for aggregate_psnr, using the three synthetic
    images as stand-ins for a real multi-image set. Not a scientific
    result -- just confirms averaging-in-dB vs averaging-in-MSE actually
    diverge here, in the expected direction, before trusting this on
    real photos."""
    checkerboard = data.checkerboard()
    grass = data.grass()
    flat = np.full((200, 200), 128, dtype=np.uint8)
    texture_checker, _ = compute_texture_map(checkerboard)
    texture_grass, _ = compute_texture_map(grass)
    texture_flat, _ = compute_texture_map(flat)

    pairs = []
    per_image_psnr = []
    for img, texture_norm in [(checkerboard, texture_checker), (grass, texture_grass), (flat, texture_flat)]:
        mask = gamma_mask(texture_norm, 1.0)
        embedded = naive_embed(img, mask, gain=5.0)
        pairs.append((img, embedded))
        p, _ = psnr_quality(img, embedded)
        per_image_psnr.append(p)

    mean_of_db = float(np.mean(per_image_psnr))
    correct_aggregate = aggregate_psnr(pairs)
    print(f"[self-test] per-image PSNR: {[f'{p:.2f}' for p in per_image_psnr]}")
    print(f"[self-test] mean-of-dB (wrong): {mean_of_db:.2f} dB  vs  "
          f"mean-of-MSE-then-dB (correct): {correct_aggregate:.2f} dB  "
          f"(gap={mean_of_db - correct_aggregate:+.2f} dB)")


IMAGE_DIR = "images"  # baked in by fetch_images.py at Docker build time; absent in this sandbox


def load_real_photos(image_dir=IMAGE_DIR):
    """Load all .jpg/.jpeg/.png images from image_dir as grayscale uint8
    arrays. Returns [] if the directory doesn't exist or is empty -- lets
    the caller fall back gracefully (e.g. running in a sandbox without the
    baked-in dataset, or before the first real Docker build)."""
    if not os.path.isdir(image_dir):
        return []
    names = sorted(p for p in os.listdir(image_dir) if p.lower().endswith((".jpg", ".jpeg", ".png")))
    images = []
    for name in names:
        try:
            img = Image.open(os.path.join(image_dir, name)).convert("L")
            images.append(np.array(img, dtype=np.uint8))
        except Exception as e:
            print(f"  [warn] could not load {name}: {e}")
    return images


def run_real_photo_suite(image_dir=IMAGE_DIR, psnr_target=42.3, backend_name=""):
    """
        Review (Opus) flagged this as blocking: three synthetic images are
        good for finding bugs but aren't a sample. Runs the same
        PSNR-anchored comparison across real photos and reports mean +/-
        std, not a single number. Per-image calibration failures are
        counted and skipped, not raised -- some fraction of 50 varied
        photos failing to bracket is expected and should be reported, not
        treated as an abort condition.

        No-ops if image_dir is empty, which is true in this sandbox (no
        network to fetch anything) -- only runs where fetch_images.py
        baked real photos in at Docker build time. Not yet exercised
        against real data; the mechanism is tested in main() against the
        synthetic images, which is a different claim.
    """
    images = load_real_photos(image_dir)
    if not images:
        print(f"\n[info] no real photos found in '{image_dir}/'; skipping the real-photo suite "
              f"(expected here; only the Docker image has them, baked in at build time)")
        return

    print(f"\n=== real-photo suite: {len(images)} images, iso-PSNR={psnr_target}dB, backend={backend_name} ===")
    results = {0.0: [], 1.0: []}
    n_failed = {0.0: 0, 1.0: 0}
    for img in images:
        texture_norm, _ = compute_texture_map(img)
        for g in (0.0, 1.0):
            mask = gamma_mask(texture_norm, g)
            try:
                result = calibrate_to_quality(img, mask, psnr_target, metric_fn=psnr_quality,
                                               label=f"gamma={g}")
            except RuntimeError:
                result = None
            if result is None:
                n_failed[g] += 1
                continue
            gain, _, _ = result
            embedded = naive_embed(img, mask, gain)
            q, _ = proxy_quality(img, embedded)
            results[g].append(q)

    for g in (0.0, 1.0):
        vals = results[g]
        if vals:
            print(f"  gamma={g}: n={len(vals)}/{len(images)}  "
                  f"mean={np.mean(vals):.4f}  std={np.std(vals):.4f}  "
                  f"(failed to calibrate: {n_failed[g]})")
        else:
            print(f"  gamma={g}: all {len(images)} images failed to calibrate")


# Main: run on three offline skimage test images (edges / texture / flat)

def main():
    checkerboard = data.checkerboard()                # sharp edges, ~no texture
    grass = data.grass()                              # high-frequency texture
    flat = np.full((200, 200), 128, dtype=np.uint8)   # perfectly flat control

    images = {"checkerboard (edges)": checkerboard, "grass (texture)": grass, "flat (control)": flat}

    fig, axes = plt.subplots(len(images), len(GAMMA_GRID) + 1, figsize=(18, 9))

    for row, (name, img) in enumerate(images.items()):
        texture_norm, coherence = compute_texture_map(img)
        axes[row, 0].imshow(img, cmap="gray")
        axes[row, 0].set_title(name, fontsize=9)
        axes[row, 0].axis("off")

        print(f"\n=== {name} ===")
        p50, p90, p99 = np.percentile(texture_norm, [50, 90, 99])
        print(f"  texture map: mean={texture_norm.mean():.4f} "
              f"p50={p50:.4f} p90={p90:.4f} p99={p99:.4f}")

        for col, g in enumerate(GAMMA_GRID, start=1):
            m = gamma_mask(texture_norm, g)
            finite = np.isfinite(m).all()
            axes[row, col].imshow(m, cmap="magma", vmin=0, vmax=m.max() if finite else 1)
            axes[row, col].set_title(f"γ={g}", fontsize=9)
            axes[row, col].axis("off")
            mp50, mp90, mp99 = np.percentile(m, [50, 90, 99])
            print(f"  gamma={g:>5}: mask mean={m.mean():.4f} "
                  f"p50={mp50:.4f} p90={mp90:.4f} p99={mp99:.4f} finite={finite}")

    print()
    regression_test_gamma_invariance_on_flat(flat)
    print()
    _self_test_psnr_aggregation()

    plt.tight_layout()
    try:
        fig.savefig("mask_grid.png", dpi=130)
        print("\nSaved figure: mask_grid.png (ACI: only survives via a mounted volume — "
              "not wired up today, stdout below is the output that matters)")
    except OSError as e:
        # Plotting failure shouldn't take down the calibration run below.
        print(f"\n[warn] could not save mask_grid.png ({e}) — continuing to calibration")

    # PRIMARY test: calibrate to TrustMark's real, measured operating point
    # (PSNR=42.3dB), report gamma=0 vs gamma=1 at that point. This is the
    # anchor to trust since it's measured, not guessed -- the LPIPS target
    # below is still a placeholder. Masking theory predicts the masked
    # placement should score better (lower LPIPS / higher SSIM) at equal PSNR.
    backend = "LPIPS (net=alex)" if HAVE_LPIPS else "SSIM fallback"
    texture_norm, _ = compute_texture_map(checkerboard)
    print(f"\n=== PRIMARY: equal PSNR (TrustMark's measured operating point), compare {backend} ===")
    psnr_target = 42.3
    print(f"  [info] psnr_target={psnr_target} dB is TrustMark's measured operating point")
    for g in [0.0, 1.0]:
        mask = gamma_mask(texture_norm, g)
        result = calibrate_to_quality(checkerboard, mask, psnr_target, metric_fn=psnr_quality,
                                       label=f"gamma={g}")
        if result:
            gain, iters, p_final = result
            embedded = naive_embed(checkerboard, mask, gain)
            q_final, _ = proxy_quality(checkerboard, embedded)
            print(f"  gamma={g}: gain={gain:.4f} (PSNR={p_final:.2f} dB) -> {backend}={q_final:.4f}")
        else:
            print(f"  gamma={g}: calibration failed to bracket PSNR target={psnr_target}")

    # SECONDARY check: calibrate to an LPIPS target instead (placeholder,
    # not TrustMark's measured value -- demoted to a cross-check against
    # the PSNR-anchored result above, not a number to report on its own).
    print(f"\n=== secondary check: equal {backend} (placeholder target) ===")
    target = 0.003 if HAVE_LPIPS else 0.85
    print(f"  [todo] target={target} is a placeholder ({'LPIPS' if HAVE_LPIPS else 'SSIM'}), "
          f"not TrustMark's measured value")
    calibrated_gains = {}
    for g in [0.0, 1.0]:
        mask = gamma_mask(texture_norm, g)
        result = calibrate_to_quality(checkerboard, mask, target, label=f"gamma={g}")
        if result:
            gain, iters, q_final = result
            calibrated_gains[g] = gain
            print(f"  gamma={g}: gain={gain:.4f} (converged in {iters} iters, final={q_final:.4f})")
        else:
            print(f"  gamma={g}: calibration failed to bracket target={target}")

    # diagnostic: at equal secondary-check target, what's the PSNR gap?
    # Cross-checks the primary result from the other direction.
    if calibrated_gains:
        print(f"\n=== diagnostic: PSNR at the secondary-check-calibrated gains ===")
        for g, gain in calibrated_gains.items():
            mask = gamma_mask(texture_norm, g)
            embedded = naive_embed(checkerboard, mask, gain)
            p, _ = psnr_quality(checkerboard, embedded)
            print(f"  gamma={g}: gain={gain:.4f} -> PSNR={p:.2f} dB")

    run_real_photo_suite(backend_name=backend)


if __name__ == "__main__":
    main()

