#ifndef ANALYZE_H
#define ANALYZE_H

#include <stdint.h>
#include <stddef.h>


/* ss_analyze: measure SS signal strength at each header bit.
 * Same seed as embed is required — no seed, no valid result.
 *
 * ref: Marvel, Boncelet, Retter - "Methodology of Spread-Spectrum
 *      Image Steganography", ARL-TR-1698, 1998, sec 4.1
 *      apps.dtic.mil/sti/citations/ADA349102
 */
void ss_analyze(const uint8_t *pixels, size_t px_size,
                uint32_t width, uint32_t channels,
                uint32_t seed);

#endif

