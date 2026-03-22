#include "../include/image.h"
#include "../include/png_priv.h"
#include "../include/jpeg_priv.h"
#include <strings.h>
#include <string.h>
#include <stdlib.h>

/* infer format from file extension — .jpg/.jpeg -> JPEG, else PNG.
 * we trust the extension rather than reading magic bytes at runtime. */
static ImageFormat detect_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot) {
        if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
            return IMG_FMT_JPEG;
    }
    return IMG_FMT_PNG;
}

Image image_load(const char *path)
{
    if (detect_format(path) == IMG_FMT_JPEG)
        return jpeg_load(path);
    return load_png(path);
}

void image_save(const Image *img, const char *path)
{
    if (detect_format(path) == IMG_FMT_JPEG)
        jpeg_save(img, path);
    else
        save_png(img, path);
}

void image_free(Image *img)
{
    free(img->pixels);
    img->pixels = NULL;
}

