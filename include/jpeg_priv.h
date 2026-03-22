#ifndef JPEG_PRIV_H
#define JPEG_PRIV_H

#include "image.h"

Image jpeg_load(const char *path);
void  jpeg_save(const Image *img, const char *path);

#endif

