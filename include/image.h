#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    IMG_FMT_PNG,
    IMG_FMT_JPEG
} ImageFormat;

typedef struct {
    uint8_t     *pixels;
    size_t       size;
    uint32_t     width;
    uint32_t     height;
    uint8_t      channels;
    ImageFormat  format;
} Image;

Image image_load(const char *path);
void  image_save(const Image *img, const char *path);
void  image_free(Image *img);

#endif

