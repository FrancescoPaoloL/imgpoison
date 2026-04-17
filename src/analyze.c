#include "../include/analyze.h"
#include "../include/formats.h"
#include <complex.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* LCG and chip helpers — mirrored from embed_ss.c so this module is
   self-contained and does not depend on ss internal linkage.
   Same constants and same bit extraction logic as embed_ss.c.
   ref: Press, Teukolsky, Vetterling, Flannery — Numerical Recipes in C,
        2nd ed., 1992, ch. 7 (LCG constants) */
typedef struct { uint32_t state; } LCG_a;
static void lcg_a_seed(LCG_a *r, uint32_t seed) { r->state = seed; }

static int lcg_a_bit(LCG_a *r) {
    r->state = r->state * LCG_MULTIPLIER + LCG_INCREMENT;
    return (r->state >> LCG_TOP_BIT) & 1;
}

static void make_chip_a(LCG_a *r, float *chip) {
    for (int i = 0; i < CHIP_SIZE; i++)
        chip[i] = lcg_a_bit(r) ? 1.0f : -1.0f;
}

/* pixel luminance — average of all channels (R, G, B).
 * BT.601 would use weighted sum (0.299*R + 0.587*G + 0.114*B)
 * to match human eye sensitivity, but simple average is close
 * enough for SS purposes — the signal margin is much larger
 * than the approximation error.
 * ref: en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma */
static float luma_a(const uint8_t *px, uint32_t ch) {
    float s = 0.0f;
    for (uint32_t c = 0; c < ch; c++) s += px[c];
    return s / (float)ch;
}

/* Fisher-Yates shuffle — same seed produces the same pixel order as embed.
   without the seed the permutation is unknown and analysis fails.
   ref: Knuth — The Art of Computer Programming, vol. 2, sec. 3.4.2 */
static void shuffle_a(size_t *idx, size_t n, uint32_t seed) {
    LCG_a rng;
    lcg_a_seed(&rng, seed);
    for (size_t i = n - 1; i > 0; i--) {
        rng.state = rng.state * LCG_MULTIPLIER + LCG_INCREMENT;
        size_t j  = rng.state % (i + 1);
        size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }
}

/* like extract_bit() in embed_ss.c but returns the raw float correlation
   instead of the hard 0/1 decision.
   correlation > 0 means bit-1 was embedded, < 0 means bit-0.
   larger magnitude means stronger signal.
   ref: Marvel, Boncelet, Retter — ARL-TR-1698, 1998, sec 4.2
        apps.dtic.mil/sti/citations/ADA349102
*/
static float bit_correlation(const uint8_t *pixels, const size_t *perm,
                              size_t pair_offset, uint32_t ch, LCG_a *rng) {
    float chip[CHIP_SIZE];
    make_chip_a(rng, chip);

    float corr = 0.0f;
    for (int i = 0; i < CHIP_SIZE; i++) {
        /* diff = block A minus block B.
          embed_bit() shifts A and B by ±strength in opposite directions.
          - bit-1: A shifted up, B shifted down → diff positive.
          - bit-0: A shifted down, B shifted up → diff negative.
          ref: Marvel, Boncelet, Retter — ARL-TR-1698, 1998, sec 3.2 */
        float diff = luma_a(pixels + perm[pair_offset + i]             * ch, ch)
                   - luma_a(pixels + perm[pair_offset + CHIP_SIZE + i] * ch, ch);
        corr += diff * chip[i];
    }
    return corr;
}

/* ss_analyze: measure SS signal strength at each header bit.
   Same seed as embed is required — no seed, no valid result.
   ref: Marvel, Boncelet, Retter — ARL-TR-1698, 1998, sec 4.1
        apps.dtic.mil/sti/citations/ADA349102 */
void ss_analyze(const uint8_t *pixels, size_t px_size,
                uint32_t width, uint32_t channels,
                uint32_t seed) {
    (void)width;

    size_t n_pixels  = px_size / channels;
    size_t px_needed = HEADER_BITS * 2 * CHIP_SIZE;

    if (px_needed > n_pixels) {
        fprintf(stderr, "analyze: image too small to hold SS header\n");
        return;
    }

    // rebuild the same permutation used at embed time
    size_t *perm = malloc(n_pixels * sizeof(size_t));
    if (!perm) { fprintf(stderr, "analyze: out of memory\n"); return; }
    for (size_t i = 0; i < n_pixels; i++) perm[i] = i;
    shuffle_a(perm, n_pixels, seed);

    LCG_a rng;
    lcg_a_seed(&rng, seed);

    // collect raw correlations for all 32 header bits
    float corr[HEADER_BITS];
    float sum     = 0.0f;
    float sum2    = 0.0f;
    float abs_sum = 0.0f;

    printf("SS fingerprint  (seed=%u, chip=%d)\n", seed, CHIP_SIZE);
    printf("%-6s  %-10s  %s\n", "bit", "corr", "signal");
    printf("------  ----------  ------\n");

    for (int i = 0; i < HEADER_BITS; i++) {
        corr[i] = bit_correlation(pixels, perm,
                                  (size_t)i * 2 * CHIP_SIZE,
                                  channels, &rng);
        sum     += corr[i];
        sum2    += corr[i] * corr[i];
        abs_sum += fabsf(corr[i]);
        printf("  %3d   %+10.3f  %s\n", i, corr[i],
               corr[i] > 0.0f ? ">>>" : "<<<");
    }

    free(perm);

    // descriptive stats over the 32 header correlations
    float mean      = sum  / HEADER_BITS;
    float mean2     = sum2 / HEADER_BITS;
    float stddev    = sqrtf(mean2 - mean * mean);
    float abs_mean  = abs_sum / HEADER_BITS;

    // TODO: formula is approximate! JPEG noise inflates the result. will be corrected in a future version.
    float strength_est = (abs_mean * 2.0f) / (float)CHIP_SIZE;

    printf("------  ----------\n");
    printf("mean corr   : %+.3f\n", mean);
    printf("stddev      :  %.3f\n", stddev);
    printf("mean |corr| :  %.3f\n", abs_mean);
    printf("strength est:  %.2f  (embed used --strength N)\n", strength_est);

    /* if nothing is hidden: A and B pixel blocks differ only by random noise,
     * which cancels out when subtracted → correlation near zero.
     * if something is hidden: A and B were shifted by embed_bit(),
     * the shift survives in the diff → correlation large and consistent.
     * threshold: abs_mean > CHIP_SIZE / 2. */
    int embedded = abs_mean > (float)CHIP_SIZE * 0.5f;
    printf("verdict     : %s\n",
           embedded
           ? "SUSPICIOUS — SS signal detected at this seed"
           : "clean — no SS signal at this seed");
}

