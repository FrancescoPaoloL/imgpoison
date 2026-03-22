#ifndef PNG_PRIV_H
#define PNG_PRIV_H

#include "image.h"

Image load_png(const char *path);
void  save_png(const Image *img, const char *path);

#endif

