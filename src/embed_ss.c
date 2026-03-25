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
 * Linear Congruential Generator — seeded per bit so each bit
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
 * it just wraps around — the overflow is discarded automatically.
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
 * we use simple average instead — close enough for SS embedding.
 * see: en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma */
static float pixel_luma(const uint8_t *px, uint32_t ch) {
    float sum = 0.0f;
    for (uint32_t c = 0; c < ch; c++) sum += px[c];
    return sum / ch;
}

static void add_signal(uint8_t *px, uint32_t ch, float delta) {
    for (uint32_t c = 0; c < ch; c++) {
        int v = (int)px[c] + (int)delta;
        px[c] = (uint8_t)(v < PIXEL_MIN ? PIXEL_MIN : v > PIXEL_MAX ? PIXEL_MAX : v);
    }

}


/* capacity check.
 * each payload bit needs 2 blocks of CHIP_SIZE pixels (block A and B).
 * total pixels = (header bits + payload bits) * 2 * CHIP_SIZE.
 * see embed_bit() for why 2 blocks per bit. */
static size_t pixels_needed(size_t payload_len) {
    return (HEADER_BITS + payload_len * 8) * 2 * CHIP_SIZE;
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
        size_t j  = rng.state % (i + 1);
        size_t tmp = idx[i];
        idx[i]     = idx[j];
        idx[j]     = tmp;
    }
}


/* embed one bit: nudge block A and B in opposite directions.
 * bit=1 -> signal=+1, bit=0 -> signal=-1.
 * using two blocks instead of one cancels out background brightness. */
static void embed_bit(uint8_t *pixels, const size_t *perm, size_t pair_offset,
                      uint32_t ch, int bit, LCG *rng) {
    float  signal  = bit ? 1.0f : -1.0f;
    float  chip[CHIP_SIZE];

    make_chip(rng, chip);

    for (int i = 0; i < CHIP_SIZE; i++) {
        add_signal(pixels + perm[pair_offset + i] * ch, ch,  signal * chip[i] * STRENGTH);
        add_signal(pixels + perm[pair_offset + CHIP_SIZE + i] * ch, ch, -signal * chip[i] * STRENGTH);
    }
}



/* extract one bit: subtract B from A, correlate with the chip.
 * positive correlation -> bit 1, negative -> bit 0.
 * JPEG noise cancels out because it is uncorrelated with the chip. */

static int extract_bit(const uint8_t *pixels, const size_t *perm, size_t pair_offset,
                       uint32_t ch, LCG *rng) {
    float  chip[CHIP_SIZE];

    make_chip(rng, chip);

    float correlation = 0.0f;
    for (int i = 0; i < CHIP_SIZE; i++) {
        float diff = pixel_luma(pixels + perm[pair_offset + i] * ch, ch)
                   - pixel_luma(pixels + perm[pair_offset + CHIP_SIZE + i] * ch, ch);
        correlation += diff * chip[i];
    }

    return correlation > 0.0f ? 1 : 0;
}



void ss_embed(uint8_t *pixels, size_t px_size,
              uint32_t width, uint32_t channels,
              const uint8_t *payload, size_t payload_len,
              uint32_t seed) {
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
    size_t n_bits   = HEADER_BITS + payload_len * 8;
    size_t px_needed = n_bits * 2 * CHIP_SIZE;

    if (px_needed > n_pixels) {
        fprintf(stderr, "payload too large for image\n"); exit(1);
    }

    /* perm[i] = scattered pixel index for position i */
    size_t *perm = malloc(n_pixels * sizeof(size_t));
    for (size_t i = 0; i < n_pixels; i++)
        perm[i] = i;

    /* shuffle so chip samples land on random pixels, not contiguous rows */
    shuffle_indices(perm, n_pixels, seed);

    LCG rng;
    lcg_seed(&rng, seed);

    /* embed 32-bit length header first so extract knows how many bits to read */
    for (int i = 0; i < HEADER_BITS; i++)
        embed_bit(pixels, perm, (size_t)i * 2 * CHIP_SIZE, channels,
                  (payload_len >> (31 - i)) & 1, &rng);

    /* embed payload MSB first, one bit at a time */
    for (size_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            embed_bit(pixels, perm,
                      (size_t)(HEADER_BITS + i*8 + b) * 2 * CHIP_SIZE,
                      channels, (payload[i] >> (7 - b)) & 1, &rng);

    free(perm);

    printf("SS embed : %zu bytes, seed=%u, strength=%d, chip=%d\n",
           payload_len, seed, STRENGTH, CHIP_SIZE);
    printf("SNR est. : %.0f:1 vs JPEG noise\n",
           (float)(2 * STRENGTH * CHIP_SIZE) / (3.0f * sqrtf(CHIP_SIZE)));
}


uint8_t *ss_extract(const uint8_t *pixels, size_t px_size,
                    uint32_t width, uint32_t channels,
                    uint32_t seed, size_t *out_len) {
    (void)width;

    if (pixels_needed(1) > px_size / channels) {
        fprintf(stderr, "image too small\n"); exit(1);
    }

    /* rebuild the same shuffled pixel permutation used during embed.
     * same seed = same shuffle = same pixel positions.
     * without the seed the permutation is unknown — extraction fails.
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

    /* read 32-bit length header — must use same seed so LCG state matches embed */
    uint32_t payload_len = 0;
    for (int i = 0; i < HEADER_BITS; i++)
        payload_len = (payload_len << 1) |
                      extract_bit(pixels, perm, (size_t)i * 2 * CHIP_SIZE,
                                  channels, &rng);

    if (payload_len == 0 || payload_len > MAX_PAYLOAD) {
        fprintf(stderr, "no valid SS payload found (got %u)\n", payload_len);
        free(perm);
        exit(1);
    }

    /* extract payload MSB first, one bit at a time */
    uint8_t *payload = calloc(payload_len + 1, 1);  /* +1 for null terminator */
    for (uint32_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            payload[i] = (payload[i] << 1) |
                         extract_bit(pixels, perm,
                                     (size_t)(HEADER_BITS + i*8 + b) * 2 * CHIP_SIZE,
                                     channels, &rng);

    free(perm);
    *out_len = payload_len;
    return payload;
}

