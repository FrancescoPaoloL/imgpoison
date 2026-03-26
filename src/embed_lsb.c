#include "../include/formats.h"
#include "../include/lsb.h"
#include <stdio.h>
#include <stdlib.h>

/* LSB steganography — hides data in the least significant bit of each pixel.
 * each pixel byte loses 1 bit of color precision — imperceptible to the eye.
 * fragile: JPEG compression randomizes LSBs, destroying the payload.
 * use only with lossless formats (PNG).
 * ref: van Schyndel, Tirkel, Osborne - "A Digital Watermark", ICIP 1994. */
void lsb_embed(uint8_t *pixels, size_t px_size,
               const uint8_t *payload, size_t payload_len) {
    if (payload_len == 0 || payload_len > MAX_PAYLOAD_BYTES) {
        fprintf(stderr, "invalid payload length: %zu\n", payload_len);
        exit(1);
    }

    /* each bit needs one pixel — check we have enough */
    size_t bits_needed = LSB_HEADER_BITS + payload_len * 8;
    if (bits_needed > px_size) {
        fprintf(stderr, "payload too large for image\n");
        exit(1);
    }

    /* write 32-bit length header into first 32 pixels.
     * the extractor reads this first to know how many bytes to expect. */
    for (int i = 0; i < LSB_HEADER_BITS; i++)
        pixels[i] = (pixels[i] & 0xFE) | ((payload_len >> (31 - i)) & 1);

    /* write payload bits MSB first, one bit per pixel.
     * 0xFE = 11111110 — clears the LSB, then sets it to the payload bit. */
    for (size_t i = 0; i < payload_len; i++)
        for (int b = 0; b < 8; b++)
            pixels[LSB_HEADER_BITS + i*8 + b] =
                (pixels[LSB_HEADER_BITS + i*8 + b] & 0xFE) |
                ((payload[i] >> (7 - b)) & 1);
}

