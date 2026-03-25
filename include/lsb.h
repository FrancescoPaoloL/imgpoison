#ifndef LSB_H
#define LSB_H

#include <stdint.h>
#include <stddef.h>

// extraction
uint8_t *lsb_extract(const uint8_t *pixels, size_t size, size_t *out_len);
void     lsb_detect(const uint8_t *pixels, size_t size);

// embedding
void     lsb_embed(uint8_t *pixels, size_t size,
                   const uint8_t *payload, size_t payload_len);

#endif

