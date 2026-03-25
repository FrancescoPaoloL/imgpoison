#ifndef SS_H
#define SS_H

#include <stdint.h>
#include <stddef.h>

void ss_embed(uint8_t *pixels, size_t px_size,
              uint32_t width, uint32_t channels,
              const uint8_t *payload, size_t payload_len,
              uint32_t seed);

uint8_t *ss_extract(const uint8_t *pixels, size_t px_size,
                    uint32_t width, uint32_t channels,
                    uint32_t seed, size_t *out_len);

#endif

