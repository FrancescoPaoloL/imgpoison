#ifndef TEXTURE_H
#define TEXTURE_H

#include <stdint.h>
#include <stddef.h>

/* convert an interleaved pixel buffer to a single-channel float luma
 * buffer, one entry per pixel (width*height, not width*height*channels).
 * simple average across channels, same approximation as pixel_luma in
 * embed_ss.c. caller frees the returned buffer. returns NULL on
 * allocation failure. */
float *texture_luma(const uint8_t *pixels, uint32_t width, uint32_t height,
                    uint32_t channels);

/* compute a normalized texture map from a luma buffer, in [0,1], one
 * entry per pixel. caller frees. returns NULL on allocation failure. */
float *texture_compute(const float *gray, uint32_t width, uint32_t height);

/* redistribute a texture map into an embedding mask for a given gamma,
 * ready for ss_embed's mask parameter. gamma=0 is uniform (no change),
 * gamma>0 concentrates on high-texture regions, gamma<0 concentrates on
 * flat regions. texture_norm is read only, not freed. caller frees the
 * returned buffer. returns NULL on allocation failure. */
float *texture_gamma_mask(const float *texture_norm, uint32_t width,
                          uint32_t height, float gamma);

#endif

