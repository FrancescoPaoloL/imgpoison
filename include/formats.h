#ifndef FORMATS_H
#define FORMATS_H

/* PNG signature — see libpng.org/pub/png/spec/1.2/PNG-Structure.html */
#define PNG_SIG_LEN         8
#define PNG_SIG_BYTES       { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A }

/* LCG constants from Numerical Recipes in C, chapter 7.
 * see: en.wikipedia.org/wiki/Linear_congruential_generator
 *      "Parameters in common use" -> Numerical Recipes */
#define LCG_MULTIPLIER  1664525u
#define LCG_INCREMENT   1013904223u
#define LCG_TOP_BIT     31

/* luminance approximation weights (ITU-R BT.601)
 * we use simple average instead — close enough for SS embedding.
 * see: en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma */
#define PIXEL_MAX  255
#define PIXEL_MIN  0

/* PNG IHDR offsets */
#define PNG_IHDR_OFFSET     16   /* bytes from file start to IHDR data */

/* PNG color types -> channels */
#define PNG_COLOR_GRAY      0
#define PNG_COLOR_RGB       2
#define PNG_COLOR_GRAY_A    4
#define PNG_COLOR_RGBA      6

/* PNG bit depth (we only support 8) */
#define PNG_BIT_DEPTH       8

/* JPEG save quality */
#define JPEG_SAVE_QUALITY   95

#endif

