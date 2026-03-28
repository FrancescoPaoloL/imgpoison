#include "../include/formats.h"
#include "../include/image.h"
#include "../include/png_priv.h"
#include "../include/jpeg_priv.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

/* detect format by reading the first bytes of the file.
 * more reliable than trusting the extension.
 */
static ImageFormat detect_format(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); exit(1); }

    uint8_t sig[2];
    if (fread(sig, 1, 2, fp) != 2) {
        fprintf(stderr, "cannot read file signature\n"); exit(1);
    }
    fclose(fp);

    if (sig[0] == JPEG_SIG_B0 && sig[1] == JPEG_SIG_B1)
        return IMG_FMT_JPEG;
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
    /* for saving we check the extension — the file does not exist yet */
    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0))
        jpeg_save(img, path);
    else
        save_png(img, path);
}

void image_free(Image *img)
{
    free(img->pixels);
    img->pixels = NULL;
}

