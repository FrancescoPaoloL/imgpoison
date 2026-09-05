#include "../include/texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*  Structure tensor texture mask.
 *
 * Ported from a python prototype that found three bugs the hard way,
 * all carried over here so they do not have to be found twice.
 *
 * 1. Reflect boundary, not zero padding. A gradient or blur that treats
 *    pixels outside the image as zero fabricates a fake edge at every
 *    border. Both sobel_gradients and gaussian_blur mirror the index
 *    back inside the image instead.
 *
 * 2. Percentile normalization, not max. A handful of outlier pixels
 *    (a corner, a hot pixel) should not set the scale for the whole
 *    map. texture_compute normalizes against the 99th percentile.
 *
 * 3. RMS-preserving gamma mask, not mean-preserving. MSE depends on
 *    E[M^2], not E[M]. A peaky mask with mean 1 still delivers more
 *    MSE at the same gain than a flat one, so normalizing to mean 1
 *    left the embedding gain not comparable across gamma values.
 *    texture_gamma_mask normalizes to E[M^2] == 1 instead.
 *
 * see also: the additive floor (t+eps)/(1+eps) before the exponent,
 * not a hard clip - a hard clip collapses every low-texture pixel to
 * the same value, losing the ordering among them.
 */

#define TEXTURE_SIGMA     1.5f
#define TEXTURE_PCTL      0.99
#define TEXTURE_EPS       0.01f
#define TEXTURE_CLIP_MIN  0.1f
#define TEXTURE_CLIP_MAX  10.0f


float *texture_luma(const uint8_t *pixels, uint32_t width, uint32_t height,
                    uint32_t channels) {
    size_t n = (size_t)width * height;
    float *gray = malloc(n * sizeof(float));
    if (!gray) return NULL;

    for (size_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; c++)
            sum += pixels[i * channels + c];
        gray[i] = sum / (float)channels;
    }
    return gray;
}


/* mirror an out of range coordinate back inside [0, limit-1].
 * see the reflect boundary note at the top of this file. */
static long reflect(long v, long limit) {
    if (v < 0) v = -v;
    if (v >= limit) v = 2 * limit - v - 2;
    return v;
}


/* sobel gradients, reflect boundary. */
static void sobel_gradients(const float *gray, uint32_t w, uint32_t h,
                            float *gx, float *gy) {
    static const int kx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
    static const int ky[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float sx = 0.0f, sy = 0.0f;
            for (int dy = -1; dy <= 1; dy++) {
                long sy_idx = reflect((long)y + dy, (long)h);
                for (int dx = -1; dx <= 1; dx++) {
                    long sx_idx = reflect((long)x + dx, (long)w);
                    float v = gray[(size_t)sy_idx * w + sx_idx];
                    sx += v * kx[dy + 1][dx + 1];
                    sy += v * ky[dy + 1][dx + 1];
                }
            }
            gx[(size_t)y * w + x] = sx;
            gy[(size_t)y * w + x] = sy;
        }
    }
}


/* separable gaussian blur, reflect boundary. sigma is the window the
 * structure tensor gets smoothed over, same role as skimage's
 * structure_tensor(sigma=...) in the python prototype. */
static void gaussian_blur(float *buf, uint32_t w, uint32_t h, float sigma) {
    int radius = (int)ceilf(sigma * 3.0f);
    if (radius < 1) radius = 1;
    int ksize = 2 * radius + 1;

    float *kernel = malloc((size_t)ksize * sizeof(float));
    float sum = 0.0f;
    for (int i = 0; i < ksize; i++) {
        int d = i - radius;
        kernel[i] = expf(-(float)(d * d) / (2.0f * sigma * sigma));
        sum += kernel[i];
    }
    for (int i = 0; i < ksize; i++) kernel[i] /= sum;

    float *tmp = malloc((size_t)w * h * sizeof(float));

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int k = 0; k < ksize; k++) {
                long sx = reflect((long)x + (k - radius), (long)w);
                acc += buf[(size_t)y * w + sx] * kernel[k];
            }
            tmp[(size_t)y * w + x] = acc;
        }
    }

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int k = 0; k < ksize; k++) {
                long sy = reflect((long)y + (k - radius), (long)h);
                acc += tmp[(size_t)sy * w + x] * kernel[k];
            }
            buf[(size_t)y * w + x] = acc;
        }
    }

    free(tmp);
    free(kernel);
}


static int float_cmp(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}


float *texture_compute(const float *gray, uint32_t width, uint32_t height) {
    size_t n = (size_t)width * height;

    float *gx = malloc(n * sizeof(float));
    float *gy = malloc(n * sizeof(float));
    if (!gx || !gy) { free(gx); free(gy); return NULL; }

    sobel_gradients(gray, width, height, gx, gy);

    float *axx = malloc(n * sizeof(float));
    float *axy = malloc(n * sizeof(float));
    float *ayy = malloc(n * sizeof(float));
    if (!axx || !axy || !ayy) {
        free(gx); free(gy); free(axx); free(axy); free(ayy);
        return NULL;
    }

    for (size_t i = 0; i < n; i++) {
        axx[i] = gx[i] * gx[i];
        axy[i] = gx[i] * gy[i];
        ayy[i] = gy[i] * gy[i];
    }
    free(gx);
    free(gy);

    gaussian_blur(axx, width, height, TEXTURE_SIGMA);
    gaussian_blur(axy, width, height, TEXTURE_SIGMA);
    gaussian_blur(ayy, width, height, TEXTURE_SIGMA);

    float *texture = malloc(n * sizeof(float));
    if (!texture) { free(axx); free(axy); free(ayy); return NULL; }

    for (size_t i = 0; i < n; i++) {
        /* eigenvalues of the 2x2 symmetric structure tensor
         * [[axx, axy], [axy, ayy]], closed form, l1 >= l2 always. */
        float trace = axx[i] + ayy[i];
        float diff  = axx[i] - ayy[i];
        float disc  = sqrtf(diff * diff + 4.0f * axy[i] * axy[i]);
        float l1 = (trace + disc) / 2.0f;
        float l2 = (trace - disc) / 2.0f;

        float energia = l1 + l2;
        float denom = energia > 1e-12f ? energia : 1e-12f;
        float coerenza = (l1 - l2) / denom;
        texture[i] = energia * (1.0f - coerenza);
    }

    free(axx);
    free(axy);
    free(ayy);

    /* normalize against the 99th percentile, not the max. see the
     * top of this file. */
    float *sorted = malloc(n * sizeof(float));
    if (!sorted) { free(texture); return NULL; }
    memcpy(sorted, texture, n * sizeof(float));
    qsort(sorted, n, sizeof(float), float_cmp);
    size_t idx99 = (size_t)(TEXTURE_PCTL * (double)(n - 1));
    float scale = sorted[idx99];
    free(sorted);
    if (scale < 1e-12f) scale = 1e-12f;

    for (size_t i = 0; i < n; i++) {
        float v = texture[i] / scale;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        texture[i] = v;
    }

    return texture;
}


float *texture_gamma_mask(const float *texture_norm, uint32_t width,
                          uint32_t height, float gamma) {
    size_t n = (size_t)width * height;
    float *mask = malloc(n * sizeof(float));
    if (!mask) return NULL;

    /* additive floor before the exponent, not a hard clip. see the
     * top of this file. */
    for (size_t i = 0; i < n; i++) {
        float floored = (texture_norm[i] + TEXTURE_EPS) / (1.0f + TEXTURE_EPS);
        mask[i] = powf(floored, gamma);
    }

    /* RMS-preserving: E[M^2] == 1, not E[M] == 1. see the top of this
     * file. accumulated in double, n can be large enough that a float
     * sum loses precision. */
    double sumsq = 0.0;
    for (size_t i = 0; i < n; i++) sumsq += (double)mask[i] * mask[i];
    float rms = sqrtf((float)(sumsq / (double)n));
    if (rms < 1e-12f) rms = 1e-12f;
    for (size_t i = 0; i < n; i++) mask[i] /= rms;

    /* bound the redistribution itself so a handful of near-zero-texture
     * pixels cannot eat the whole perceptual budget, then RMS-normalize
     * again since the clip shifts the mean away from 1. */
    for (size_t i = 0; i < n; i++) {
        if (mask[i] < TEXTURE_CLIP_MIN) mask[i] = TEXTURE_CLIP_MIN;
        if (mask[i] > TEXTURE_CLIP_MAX) mask[i] = TEXTURE_CLIP_MAX;
    }

    sumsq = 0.0;
    for (size_t i = 0; i < n; i++) sumsq += (double)mask[i] * mask[i];
    rms = sqrtf((float)(sumsq / (double)n));
    if (rms < 1e-12f) rms = 1e-12f;
    for (size_t i = 0; i < n; i++) mask[i] /= rms;

    /* diagnostic: RMS after this final re-normalization is 1.0 by
     * construction (we just divided every value by its own measured
     * RMS) - printing it isn't testing a hypothesis, it's confirming
     * the algebra. What actually varies with gamma is the MEAN, E[M]:
     * exactly 1 at gamma=0 (constant mask), strictly less than 1 for
     * any non-constant mask (Jensen's inequality, same argument as
     * extract_bit's SNR comment). If the PSNR gap between gamma=0 and
     * gamma=1 at matched --strength were caused by imperfect RMS
     * normalization, this print would show something other than
     * 1.000 for rms - if it shows 1.000 (expected), the gap's real
     * source is elsewhere, most likely the saturation asymmetry
     * already found and counted (see add_signal / the Saturated:
     * print in embed_ss.c), not the mask normalization itself.
     * recomputed fresh on the final mask values, not reusing the
     * pre-division sumsq above - that would silently print the wrong
     * number. */
    {
        double final_sum = 0.0, final_sumsq = 0.0;
        for (size_t i = 0; i < n; i++) {
            final_sum   += (double)mask[i];
            final_sumsq += (double)mask[i] * (double)mask[i];
        }
        fprintf(stderr, "mask diagnostic: mean=%.6f rms=%.6f (gamma=%.2f, n=%zu)\n",
                final_sum / (double)n, sqrt(final_sumsq / (double)n), gamma, n);
    }

    return mask;
}

