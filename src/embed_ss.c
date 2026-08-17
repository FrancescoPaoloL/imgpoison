#include "../include/formats.h"
#include "../include/ss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


/*  Spread Spectrum steganography.
 *
 * Each payload bit is split across two pixel blocks A and B.
 * A random +1/-1 sequence (chip) is generated from the seed.
 * A is nudged in the chip direction, B in the opposite.
 *
 * To extract: diff = A - B, then correlate with the same chip.
 * Positive -> bit 1, negative -> bit 0.
 *
 * JPEG noise is uncorrelated with the chip so it cancels out.
 * LSB dies after JPEG. SS survives because the signal is spread
 * across hundreds of pixels, not packed in a single bit.
 */


/* lcg: minimal pseudorandom number generator
 *
 * Linear Congruential Generator - seeded per bit so each bit
 * gets a independent chip. No external dependency.
 *
 * see: en.wikipedia.org/wiki/Linear_congruential_generator
 *          "Parameters in common use" --> Numerical Recipes
 *
 *
 * Recurrence relation (Numerical Recipes in C):
 *
 *   I_(j+1) = a * I_j + c  (mod m)
 *
 * system rand() are almost always LCGs, under the hood.
 *
 * When you add or multiply a uint32_t and the result exceeds 2^32,
 * it just wraps around - the overflow is discarded automatically.
 * That's the mod m for free, no extra code needed.
 *
 * mod 2^32 is free: uint32_t overflows wrap around automatically.
 */
typedef struct { uint32_t state; } LCG;
static void lcg_seed(LCG *r, uint32_t seed) { r->state = seed; }
static int lcg_bit(LCG *r) {
    r->state = r->state * LCG_MULTIPLIER + LCG_INCREMENT;
    return (r->state >> LCG_TOP_BIT) & 1;
}




/* generate chip[CHIP_SIZE] of +1/-1 values */
static void make_chip(LCG *r, float *chip)
{
    for (int i = 0; i < CHIP_SIZE; i++)
        chip[i] = lcg_bit(r) ? 1.0f : -1.0f;
}

/* luminance approximation weights (ITU-R BT.601)
 * we use simple average instead - close enough for SS embedding.
 * see: en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma */
static float pixel_luma(const uint8_t *px, uint32_t ch) {
    float sum = 0.0f;
    for (uint32_t c = 0; c < ch; c++) sum += px[c];
    return sum / ch;
}

/* returns 1 if delta had to be clipped to fit [0,255], 0 otherwise.
 * strength*mask can push delta well past what a pixel near 0 or 255
 * can absorb - clipping breaks the +/- symmetry embed_bit relies on
 * and costs signal, worse for the higher mask values that gamma>0
 * concentrates on. counted in ss_embed so it shows up as a number
 * instead of staying invisible. */
static int add_signal(uint8_t *px, uint32_t ch, float delta) {
    int saturated = 0;
    for (uint32_t c = 0; c < ch; c++) {
        int v = (int)px[c] + (int)delta;
        if (v < PIXEL_MIN || v > PIXEL_MAX) saturated = 1;
        px[c] = (uint8_t)(v < PIXEL_MIN ? PIXEL_MIN : v > PIXEL_MAX ? PIXEL_MAX : v);
    }
    return saturated;
}


/* capacity check.
 * each embedded bit needs 2 blocks of CHIP_SIZE pixels (block A and B).
 * the header section = (MAGIC_BITS + HEADER_BITS) bits, each repeated
 * HEADER_REPEAT times. the payload body = payload_len*8 bits, once each.
 * see embed_bit() for why 2 blocks per bit. */
static size_t header_bits_total(void) {
    return (size_t)(MAGIC_BITS + HEADER_BITS) * HEADER_REPEAT;
}
static size_t pixels_needed(size_t payload_len) {
    return (header_bits_total() + payload_len * 8 * PAYLOAD_REPEAT) * 2 * CHIP_SIZE;
}


/* total number of pixels available in the image */
static size_t total_pixels(size_t px_size, uint32_t channels)
{
    return px_size / channels;
}


/* fisher-yates shuffle on block indices using LCG.
 * distributes payload blocks evenly across the image
 * instead of packing them linearly at the start.
 * without the seed you cannot reconstruct the order.
 * see: en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle */
static void shuffle_indices(size_t *idx, size_t n, uint32_t seed)
{
    LCG rng;
    lcg_seed(&rng, seed);

    for (size_t i = n - 1; i > 0; i--) {
        rng.state = rng.state * LCG_MULTIPLIER + LCG_INCREMENT;
        /* high bits, not modulo: rng.state % (i+1) picks up the LCG's
         * low-order bits, which have far shorter periods than the high
         * ones (bit 0 has period 2). make_chip already avoids this by
         * reading bit 31 - this is the same fix applied here, using
         * the high 32 bits of a 64-bit product instead of a modulo.
         * see: en.wikipedia.org/wiki/Linear_congruential_generator
         *      "Advantages and disadvantages" */
        size_t j = (size_t)(((uint64_t)rng.state * (i + 1)) >> 32);
        size_t tmp = idx[i];
        idx[i]     = idx[j];
        idx[j]     = tmp;
    }
}


/* embed one bit: nudge block A and B in opposite directions.
 * bit=1 -> signal=+1, bit=0 -> signal=-1.
 * using two blocks instead of one cancels out background brightness.
 * mask, if not NULL, scales strength per pixel: block A and B usually
 * land on different pixels, so each chip sample looks up its own
 * mask value instead of sharing one for the whole bit. */
static void embed_bit(uint8_t *pixels, const size_t *perm, size_t pair_offset,
                      uint32_t ch, int bit, LCG *rng, int strength,
                      const float *mask, size_t *saturated) {
    float  signal  = bit ? 1.0f : -1.0f;
    float  chip[CHIP_SIZE];

    make_chip(rng, chip);

    for (int i = 0; i < CHIP_SIZE; i++) {
        size_t idx_a = perm[pair_offset + i];
        size_t idx_b = perm[pair_offset + CHIP_SIZE + i];
        float  m_a   = mask ? mask[idx_a] : 1.0f;
        float  m_b   = mask ? mask[idx_b] : 1.0f;

        *saturated += add_signal(pixels + idx_a * ch, ch,
                                 signal * chip[i] * strength * m_a);
        *saturated += add_signal(pixels + idx_b * ch, ch,
                                -signal * chip[i] * strength * m_b);
    }
}



/* extract one bit: subtract B from A, correlate with the chip.
 * positive correlation -> bit 1, negative -> bit 0.
 * JPEG noise cancels out because it is uncorrelated with the chip.
 *
 * weighted by mask when given: embed_bit scales the signal at each
 * pixel by mask[i], so an unweighted correlator is not the matched
 * filter for a non-uniform mask. with equal weights, signal ~ N*E[M]
 * (N=CHIP_SIZE) but noise ~ sigma*sqrt(N), independent of the mask -
 * the correlator's noise term never sees mask at all when unweighted.
 * so SNR ~ E[M]*sqrt(N)/sigma, and since the mask is RMS-normalized
 * (E[M^2]=1), Jensen's inequality gives E[M] <= 1 with equality only
 * at gamma=0. every other gamma would lose SNR by construction, not
 * because masking is worse. weighting the correlation by (m_a + m_b)
 * matches the filter to the actual per-sample signal amplitude and
 * removes that bias. */
static int extract_bit(const uint8_t *pixels, const size_t *perm, size_t pair_offset,
                       uint32_t ch, LCG *rng, const float *mask) {
    float  chip[CHIP_SIZE];

    make_chip(rng, chip);

    float correlation = 0.0f;
    for (int i = 0; i < CHIP_SIZE; i++) {
        size_t idx_a = perm[pair_offset + i];
        size_t idx_b = perm[pair_offset + CHIP_SIZE + i];
        float  diff  = pixel_luma(pixels + idx_a * ch, ch)
                     - pixel_luma(pixels + idx_b * ch, ch);
        float  m     = mask ? (mask[idx_a] + mask[idx_b]) : 2.0f;
        correlation += diff * chip[i] * m;
    }

    return correlation > 0.0f ? 1 : 0;
}


/* embed one logical bit redundantly across HEADER_REPEAT physical slots.
 * *slot is the running pair index; advanced by HEADER_REPEAT.
 * each repeat consumes a fresh chip from the LCG, exactly mirrored on extract. */
static void embed_bit_rep(uint8_t *pixels, const size_t *perm, size_t *slot,
                          uint32_t ch, int bit, LCG *rng, int strength,
                          const float *mask, size_t *saturated, int repeat) {
    for (int r = 0; r < repeat; r++)
        embed_bit(pixels, perm, (*slot)++ * 2 * CHIP_SIZE, ch, bit, rng, strength, mask, saturated);
}

/* extract one logical bit by majority vote over `repeat` physical slots.
 * repeat must be odd so the vote can never tie. */
static int extract_bit_rep(const uint8_t *pixels, const size_t *perm, size_t *slot,
                           uint32_t ch, LCG *rng, const float *mask, int repeat) {
    int ones = 0;
    for (int r = 0; r < repeat; r++)
        ones += extract_bit(pixels, perm, (*slot)++ * 2 * CHIP_SIZE, ch, rng, mask);
    return (ones * 2 > repeat) ? 1 : 0;
}


void ss_embed(uint8_t *pixels, size_t px_size,
              uint32_t width, uint32_t channels,
              const uint8_t *payload, size_t payload_len,
              uint32_t seed, uint32_t strength,
              const float *mask){
    (void)width;

    if (payload_len == 0 || payload_len > MAX_PAYLOAD) {
        fprintf(stderr, "invalid payload length: %zu\n", payload_len); exit(1);
    }
    if (pixels_needed(payload_len) > px_size / channels) {
        fprintf(stderr, "payload too large for image\n"); exit(1);
    }

    /* build shuffled pixel permutation table.
     * instead of embedding into contiguous horizontal stripes (which causes
     * visible noise in flat regions like sky), we scatter individual pixel
     * positions randomly across the entire image using a seeded shuffle.
     * each chip pair draws samples from all over the 2D surface.
     * same seed = same permutation = reproducible extraction.
     * ref: Marvel, Boncelet, Retter - "Methodology of Spread-Spectrum
     *      Image Steganography", ARL-TR-1698, 1998, sec 4.1
     *      apps.dtic.mil/sti/citations/ADA349102 */
    size_t n_pixels = total_pixels(px_size, channels);
    size_t px_needed = pixels_needed(payload_len);

    if (px_needed > n_pixels) {
        fprintf(stderr,
            "payload too large for image: need %zu pixels, have %zu "
            "(CHIP_SIZE=%d, try a smaller payload or larger image)\n",
            px_needed, n_pixels, CHIP_SIZE);
        exit(1);
    }

    /* perm[i] = scattered pixel index for position i */
    size_t *perm = malloc(n_pixels * sizeof(size_t));
    for (size_t i = 0; i < n_pixels; i++)
        perm[i] = i;

    /* shuffle so chip samples land on random pixels, not contiguous rows */
    shuffle_indices(perm, n_pixels, seed);

    LCG rng;
    lcg_seed(&rng, seed);

    int str = (int)strength;

    /* running slot index; embed and extract advance it identically */
    size_t slot = 0;
    size_t saturated = 0;

    /* 1) magic marker (redundant) so extract can reject noise */
    for (int i = 0; i < MAGIC_BITS; i++)
        embed_bit_rep(pixels, perm, &slot, channels,
                      (SS_MAGIC >> (MAGIC_BITS - 1 - i)) & 1, &rng, str, mask, &saturated, HEADER_REPEAT);

    /* 2) 32-bit length header (redundant) */
    for (int i = 0; i < HEADER_BITS; i++)
        embed_bit_rep(pixels, perm, &slot, channels,
                      (payload_len >> (31 - i)) & 1, &rng, str, mask, &saturated, HEADER_REPEAT);

    /* 3) payload body, MSB first, majority-vote repeated the same way the
     * header already was. a single un-repeated bit had nothing protecting
     * it from something as small as the mask drift between embed and
     * extract time (see extract_bit's comment) - found on a real image
     * where one payload bit flipped with zero transformations applied. */
    for (size_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            embed_bit_rep(pixels, perm, &slot, channels,
                          (payload[i] >> (7 - b)) & 1, &rng, str, mask, &saturated, PAYLOAD_REPEAT);

    free(perm);

    printf("SS embed : %zu bytes, seed=%u, strength=%d, chip=%d\n",
           payload_len, seed, str, CHIP_SIZE);
    printf("SNR est. : %.0f:1 vs JPEG noise\n",
           (float)(2 * strength * CHIP_SIZE) / (3.0f * sqrtf(CHIP_SIZE)));

    size_t total_samples = 2 * CHIP_SIZE * (header_bits_total() + payload_len * 8);
    printf("Saturated: %zu / %zu samples (%.1f%%)\n",
           saturated, total_samples, 100.0 * (double)saturated / (double)total_samples);
}


uint8_t *ss_extract(const uint8_t *pixels, size_t px_size,
                    uint32_t width, uint32_t channels,
                    uint32_t seed, size_t *out_len, const float *mask) {
    (void)width;

    if (pixels_needed(1) > px_size / channels) {
        fprintf(stderr, "image too small\n"); exit(1);
    }

    /* rebuild the same shuffled pixel permutation used during embed.
     * same seed = same shuffle = same pixel positions.
     * without the seed the permutation is unknown - extraction fails.
     * ref: Marvel, Boncelet, Retter - "Methodology of Spread-Spectrum
     *      Image Steganography", ARL-TR-1698, 1998, sec 4.1
     *      apps.dtic.mil/sti/citations/ADA349102 */
    size_t n_pixels = total_pixels(px_size, channels);

    size_t *perm = malloc(n_pixels * sizeof(size_t));
    for (size_t i = 0; i < n_pixels; i++)
        perm[i] = i;

    shuffle_indices(perm, n_pixels, seed);

    LCG rng;
    lcg_seed(&rng, seed);

    size_t slot = 0;

    /* 1) read magic and verify. mismatch = noise (wrong seed, no payload,
     *    or past the robustness limit), not a usable length. */
    uint32_t magic = 0;
    for (int i = 0; i < MAGIC_BITS; i++)
        magic = (magic << 1) | extract_bit_rep(pixels, perm, &slot, channels, &rng, mask, HEADER_REPEAT);

    if (magic != SS_MAGIC) {
        fprintf(stderr,
            "no valid SS payload found: magic mismatch "
            "(got 0x%04X, want 0x%04X) - wrong seed or image past "
            "robustness limit\n", magic, SS_MAGIC);
        free(perm);
        exit(1);
    }

    /* 2) read redundant 32-bit length header. */
    uint32_t payload_len = 0;
    for (int i = 0; i < HEADER_BITS; i++)
        payload_len = (payload_len << 1) |
                      extract_bit_rep(pixels, perm, &slot, channels, &rng, mask, HEADER_REPEAT);

    if (payload_len == 0 || payload_len > MAX_PAYLOAD) {
        fprintf(stderr, "no valid SS payload found (bad length %u)\n", payload_len);
        free(perm);
        exit(1);
    }

    /* 3) extract payload body, MSB first, majority vote over PAYLOAD_REPEAT
     * slots per bit, same scheme as the header. */
    uint8_t *payload = calloc(payload_len + 1, 1);  /* +1 for null terminator */
    for (uint32_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            payload[i] = (payload[i] << 1) |
                         extract_bit_rep(pixels, perm, &slot, channels, &rng, mask, PAYLOAD_REPEAT);

    free(perm);
    *out_len = payload_len;
    return payload;
}


float ss_bit_accuracy(const uint8_t *pixels, size_t px_size,
                      uint32_t width, uint32_t channels,
                      const uint8_t *known_payload, size_t payload_len,
                      uint32_t seed, const float *mask) {
    (void)width;

    size_t n_pixels = total_pixels(px_size, channels);
    size_t *perm = malloc(n_pixels * sizeof(size_t));
    for (size_t i = 0; i < n_pixels; i++)
        perm[i] = i;
    shuffle_indices(perm, n_pixels, seed);

    LCG rng;
    lcg_seed(&rng, seed);

    size_t slot = 0;

    /* skip magic + header without reading them back - just advance the
     * chip stream and slot counter the same amount ss_embed did, so the
     * payload section lines up. header_bits_total() already counts the
     * HEADER_REPEAT multiplication. */
    for (size_t i = 0; i < header_bits_total(); i++) {
        float chip[CHIP_SIZE];
        make_chip(&rng, chip);
        slot++;
    }

    size_t correct = 0, total = 0;
    for (size_t i = 0; i < payload_len; i++) {
        for (int b = 0; b < 8; b++) {
            int expected = (known_payload[i] >> (7 - b)) & 1;
            for (int r = 0; r < PAYLOAD_REPEAT; r++) {
                int got = extract_bit(pixels, perm, slot++ * 2 * CHIP_SIZE, channels, &rng, mask);
                if (got == expected) correct++;
                total++;
            }
        }
    }

    free(perm);
    return total > 0 ? (float)correct / (float)total : 0.0f;
}

