#include "../include/jpeg_priv.h"
#include "../include/formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

/* decode a JPEG file to a flat RGB pixel buffer.
 * libjpeg handles all DCT/quantization/huffman internally.
 * we force JCS_RGB output so the rest of the pipeline is uniform.
 * ref: libjpeg-turbo.org/Documentation */
Image jpeg_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); exit(1); }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr         jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    uint32_t w  = cinfo.output_width;
    uint32_t h  = cinfo.output_height;
    uint8_t  ch = (uint8_t)cinfo.output_components;

    size_t   stride = (size_t)w * ch;
    uint8_t *pixels = malloc(stride * h);

    /* libjpeg decompresses one scanline at a time */
    while (cinfo.output_scanline < h) {
        uint8_t *row = pixels + cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    Image img = { pixels, stride * h, w, h, ch, IMG_FMT_JPEG };
    return img;
}

/* encode pixel buffer to JPEG at JPEG_SAVE_QUALITY.
 * save as JPEG at q95 — high enough for SS payload to survive compression.
 * ref: libjpeg-turbo.org/Documentation */
void jpeg_save(const Image *img, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); exit(1); }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = img->width;
    cinfo.image_height     = img->height;
    cinfo.input_components = img->channels;
    cinfo.in_color_space   = (img->channels == 3) ? JCS_RGB : JCS_GRAYSCALE;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_SAVE_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    size_t stride = (size_t)img->width * img->channels;
    while (cinfo.next_scanline < img->height) {
        uint8_t *row = img->pixels + cinfo.next_scanline * stride;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
}

