#include "../include/formats.h"
#include "../include/lsb.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* read the 32-bit length header, then extract payload bits MSB first.
 * mirror of lsb_embed — must use same bit order. */
uint8_t *lsb_extract(const uint8_t *px, size_t px_size, size_t *out_len) {

    /* reconstruct length from first 32 LSBs */
    uint32_t len = 0;
    for (int i = 0; i < LSB_HEADER_BITS; i++)
        len = (len << 1) | (px[i] & 1);

    if (len == 0 || len > MAX_PAYLOAD_BYTES) {
        fprintf(stderr, "no valid LSB payload found (got %u)\n", len); exit(1);
    }
    if (LSB_HEADER_BITS + (size_t)len * 8 > px_size) {
        fprintf(stderr, "payload exceeds image capacity\n"); exit(1);
    }

    /* reassemble bytes from individual LSBs */
    uint8_t *payload = calloc(len + 1, 1);  /* +1 for null terminator */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (byte << 1) | (px[LSB_HEADER_BITS + i*8 + b] & 1);
        payload[i] = byte;
    }

    *out_len = len;
    return payload;
}

/* steganalysis: detect LSB embedding via chi-square test and LSB ratio.
 *
 * LSB embedding makes adjacent pixel values (2k, 2k+1) equally likely —
 * their frequencies converge. chi-square measures this flattening.
 * LSB ratio near 0.5 means half the pixels have LSB=1, consistent with
 * random embedded data.
 *
 * ref: Westfeld, Pfitzmann - "Attacks on Steganographic Systems", 1999.
 *      link.springer.com/chapter/10.1007/10719724_15 */
void lsb_detect(const uint8_t *px, size_t px_size) {

    /* count frequency of each pixel value 0-255 */
    size_t freq[256] = {0};
    for (size_t i = 0; i < px_size; i++)
        freq[px[i]]++;

    /* chi-square test on pairs (0,1), (2,3), ..., (254,255).
     * if embedded: freq[2k] ≈ freq[2k+1] → chi-square stays low.
     * if clean: natural image statistics → chi-square is high. */
    double chi        = 0.0;
    size_t pairs_used = 0;
    for (int v = 0; v < 256; v += 2) {
        double expected = (freq[v] + freq[v+1]) / 2.0;
        if (expected < 1.0) continue;
        double de  = freq[v]   - expected;
        double do_ = freq[v+1] - expected;
        chi += (de * de + do_ * do_) / expected;
        pairs_used++;
    }

    /* LSB ratio — random payload drives this toward 0.5 */
    size_t lsb_ones = 0;
    for (size_t i = 0; i < px_size; i++)
        lsb_ones += px[i] & 1;
    double lsb_ratio = (double)lsb_ones / (double)px_size;

    printf("Chi-square : %.2f  (pairs=%zu)\n", chi, pairs_used);
    printf("LSB ratio  : %.4f  (0.5000 = fully embedded)\n", lsb_ratio);
    printf("Verdict    : %s\n",
        (chi > 20000.0 || fabs(lsb_ratio - 0.5) < 0.01)
        ? "SUSPICIOUS — possible LSB payload"
        : "clean — no LSB embedding detected");
}

