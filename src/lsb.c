#include "../include/formats.h"
#include "../include/lsb.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>


uint8_t *lsb_extract(const uint8_t *px, size_t px_size, size_t *out_len) {
    uint32_t len = 0;
    for (int i = 0; i < LSB_HEADER_BITS; i++)
        len = (len << 1) | (px[i] & 1);

    if (len == 0 || len > MAX_PAYLOAD_BYTES) {
        fprintf(stderr, "no valid LSB payload found (got %u)\n", len); exit(1);
    }
    if (LSB_HEADER_BITS + (size_t)len * 8 > px_size) {
        fprintf(stderr, "payload exceeds image capacity\n"); exit(1);
    }

    uint8_t *payload = calloc(len + 1, 1);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (byte << 1) | (px[LSB_HEADER_BITS + i*8 + b] & 1);
        payload[i] = byte;
    }

    *out_len = len;
    return payload;
}

void lsb_detect(const uint8_t *px, size_t px_size) {
    size_t freq[256] = {0};
    for (size_t i = 0; i < px_size; i++)
        freq[px[i]]++;

    double chi        = 0.0;
    size_t pairs_used = 0;
    for (int v = 0; v < 256; v += 2) {
        double expected = (freq[v] + freq[v+1]) / 2.0;
        if (expected < 1.0) continue;
        double de = freq[v]   - expected;
        double do_ = freq[v+1] - expected;
        chi += (de * de + do_ * do_) / expected;
        pairs_used++;
    }

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

