#include "../include/formats.h"
#include "../include/lsb.h"
#include <stdio.h>
#include <stdlib.h>


void lsb_embed(uint8_t *pixels, size_t px_size,
               const uint8_t *payload, size_t payload_len) {
    if (payload_len == 0 || payload_len > MAX_PAYLOAD_BYTES) {
        fprintf(stderr, "invalid payload length: %zu\n", payload_len);
        exit(1);
    }

    size_t bits_needed = LSB_HEADER_BITS + payload_len * 8;
    if (bits_needed > px_size) {
        fprintf(stderr, "payload too large for image\n");
        exit(1);
    }

    // write 32-bit length header into first 32 LSBs
    for (int i = 0; i < LSB_HEADER_BITS; i++)
        pixels[i] = (pixels[i] & 0xFE) | ((payload_len >> (31 - i)) & 1);

    // write payload bits
    for (size_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            pixels[LSB_HEADER_BITS + i*8 + b] =
                (pixels[LSB_HEADER_BITS + i*8 + b] & 0xFE) |
                ((payload[i] >> (7 - b)) & 1);
}

