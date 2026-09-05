#ifndef SS_H
#define SS_H

#include <stdint.h>
#include <stddef.h>

void ss_embed(uint8_t *pixels, size_t px_size,
              uint32_t width, uint32_t channels,
              const uint8_t *payload, size_t payload_len,
              uint32_t seed, uint32_t strength,
              const float *mask, uint32_t chip_size, uint32_t payload_repeat);

uint8_t *ss_extract(const uint8_t *pixels, size_t px_size,
                    uint32_t width, uint32_t channels,
                    uint32_t seed, size_t *out_len, const float *mask,
                    uint32_t chip_size, uint32_t payload_repeat);

/* diagnostic only: raw per-bit accuracy against a known payload, no
 * magic gate, no majority vote. the ordinary decode path snaps to
 * all-or-nothing (HEADER_REPEAT/PAYLOAD_REPEAT redundancy plus the
 * magic gate mean an attack either fully fails or barely dents it),
 * which makes a robustness sweep across gamma or attack strength a
 * flat line then a cliff, not a curve. this measures the signal
 * itself degrading, one repeat slot at a time.
 * caller must already know payload_len and mask (same requirement as
 * ss_extract when mask isn't NULL). returns fraction correct in
 * [0,1]. */
float ss_bit_accuracy(const uint8_t *pixels, size_t px_size,
                      uint32_t width, uint32_t channels,
                      const uint8_t *known_payload, size_t payload_len,
                      uint32_t seed, const float *mask,
                      uint32_t chip_size, uint32_t payload_repeat);

#endif

