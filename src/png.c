#include "../include/png_priv.h"
#include "../include/formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <arpa/inet.h>

/* the 8-byte magic number every PNG file starts with.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Structure.html */
static const uint8_t PNG_SIG[PNG_SIG_LEN] = PNG_SIG_BYTES;

/* read a 4-byte big-endian integer from a byte pointer.
 * PNG stores all multi-byte integers in big-endian order.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Structure.html#Chunk-layout */
static uint32_t u32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16)
         | ((uint32_t)p[2]<< 8) |  (uint32_t)p[3];
}

/* map PNG color type field (byte 9 of IHDR) to number of channels.
 * color type is a bitmask: bit0=palette, bit1=color, bit2=alpha.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Chunks.html#C.IHDR */
static uint8_t channels_for(uint8_t color_type) {
    switch (color_type) {
        case PNG_COLOR_GRAY:   return 1;
        case PNG_COLOR_RGB:    return 3;
        case PNG_COLOR_GRAY_A: return 2;
        case PNG_COLOR_RGBA:   return 4;
        default: fprintf(stderr, "unsupported color type %u\n", color_type); exit(1);
    }
}

/* Paeth predictor — PNG filter type 4.
 * predicts the next byte from left (a), above (b), upper-left (c).
 * ref: libpng.org/pub/png/spec/1.2/PNG-Filters.html */
static int paeth(int a, int b, int c) {
    int p=a+b-c, pa=abs(p-a), pb=abs(p-b), pc=abs(p-c);
    return (pa<=pb && pa<=pc) ? a : (pb<=pc) ? b : c;
}

/* collect all IDAT chunks into a single buffer for decompression.
 * PNG splits compressed pixel data across one or more IDAT chunks.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Chunks.html#C.IDAT */
static uint8_t *collect_idat(const uint8_t *file, size_t file_size, size_t *idat_size) {
    uint8_t *buf = malloc(file_size);
    *idat_size   = 0;
    size_t off   = 8;

    while (off + 8 <= file_size) {
        uint32_t len  = u32be(file + off);
        uint8_t *type = (uint8_t *)(file + off + 4);
        off += 8;
        if (memcmp(type, "IDAT", 4) == 0) {
            memcpy(buf + *idat_size, file + off, len);
            *idat_size += len;
        }
        if (memcmp(type, "IEND", 4) == 0) break;
        off += len + 4;
    }
    return buf;
}

/* decompress IDAT payload with zlib.
 * PNG uses deflate compression (RFC 1950) via zlib.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Compression.html */
static uint8_t *inflate_idat(const uint8_t *in, size_t in_size, size_t out_size) {
    uint8_t *out = malloc(out_size);
    z_stream z   = {0};
    z.next_in    = (Bytef *)in;
    z.avail_in   = (uInt)in_size;
    z.next_out   = out;
    z.avail_out  = (uInt)out_size;
    if (inflateInit(&z) != Z_OK) { fprintf(stderr, "inflateInit failed\n"); exit(1); }
    if (inflate(&z, Z_FINISH) != Z_STREAM_END) { fprintf(stderr, "inflate failed\n"); exit(1); }
    inflateEnd(&z);
    return out;
}

/* undo PNG row filters and return raw pixel buffer.
 * each row starts with a filter-type byte (0-4).
 * ref: libpng.org/pub/png/spec/1.2/PNG-Filters.html */
static uint8_t *reconstruct(const uint8_t *raw, uint32_t w, uint32_t h, uint8_t ch) {
    size_t   stride = (size_t)w * ch;
    uint8_t *px     = malloc(stride * h);

    for (uint32_t y = 0; y < h; y++) {
        uint8_t        f     = raw[y * (stride + 1)];
        const uint8_t *src   = raw + y * (stride + 1) + 1;
        uint8_t       *dst   = px  + y * stride;
        uint8_t       *prior = (y > 0) ? px + (y-1) * stride : NULL;

        for (size_t x = 0; x < stride; x++) {
            uint8_t l  = (x >= ch)          ? dst[x-ch]   : 0;
            uint8_t u  = prior              ? prior[x]    : 0;
            uint8_t ul = (prior && x >= ch) ? prior[x-ch] : 0;
            switch (f) {
                case 0: dst[x] = src[x];                             break; /* None  */
                case 1: dst[x] = src[x] + l;                         break; /* Sub   */
                case 2: dst[x] = src[x] + u;                         break; /* Up    */
                case 3: dst[x] = src[x] + (uint8_t)((l + u) / 2);    break; /* Avg   */
                case 4: dst[x] = src[x] + (uint8_t)paeth(l, u, ul);  break; /* Paeth */
                default: fprintf(stderr, "unknown filter %u\n", f); exit(1);
            }
        }
    }
    return px;
}

/* read a PNG file and return an Image with unpacked pixel data.
 * layout: signature | IHDR | ...IDAT... | IEND
 * each chunk: 4B length | 4B type | data | 4B CRC
 * ref: libpng.org/pub/png/spec/1.2/PNG-Structure.html */
Image load_png(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); exit(1); }
    fseek(fp, 0, SEEK_END);
    size_t file_size = (size_t)ftell(fp); rewind(fp);
    uint8_t *file = malloc(file_size);
    if (fread(file, 1, file_size, fp) != file_size) {
        fprintf(stderr, "fread failed\n"); exit(1);
    }
    fclose(fp);

    if (memcmp(file, PNG_SIG, PNG_SIG_LEN) != 0) {
        fprintf(stderr, "not a PNG\n"); exit(1);
    }

    /* IHDR starts at byte 16: 8B sig + 4B length + 4B type */
    const uint8_t *ihdr = file + PNG_IHDR_OFFSET;
    uint32_t w  = u32be(ihdr);
    uint32_t h  = u32be(ihdr + 4);
    uint8_t  ch = channels_for(ihdr[9]);

    size_t   idat_size;
    uint8_t *idat     = collect_idat(file, file_size, &idat_size);
    size_t   expected = ((size_t)w * ch + 1) * h;  /* +1 per row for filter byte */
    uint8_t *raw      = inflate_idat(idat, idat_size, expected);
    uint8_t *pixels   = reconstruct(raw, w, h, ch);

    free(file); free(idat); free(raw);

    Image img = { pixels, (size_t)w * h * ch, w, h, ch, IMG_FMT_PNG };
    return img;
}

/* write pixel data back to a PNG file.
 * filter type 0 (None) on every row — simplest output.
 * ref: libpng.org/pub/png/spec/1.2/PNG-Filters.html */
void save_png(const Image *img, const char *path)
{
    size_t   stride   = (size_t)img->width * img->channels;
    size_t   raw_size = (stride + 1) * img->height;
    uint8_t *raw      = malloc(raw_size);

    for (uint32_t y = 0; y < img->height; y++) {
        raw[y * (stride + 1)] = 0;
        memcpy(raw + y * (stride + 1) + 1,
               img->pixels + y * stride, stride);
    }

    uLongf   comp_size = compressBound((uLong)raw_size);
    uint8_t *comp      = malloc(comp_size);
    compress2(comp, &comp_size, raw, raw_size, Z_BEST_COMPRESSION);
    free(raw);

    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); exit(1); }

    static const uint8_t sig[PNG_SIG_LEN] = PNG_SIG_BYTES;
    fwrite(sig, 1, PNG_SIG_LEN, fp);

    /* chunk: 4B length | 4B type | data | 4B CRC (covers type+data)
     * ref: libpng.org/pub/png/spec/1.2/PNG-Structure.html#CRC-algorithm */
    #define WRITE_CHUNK(type, data, len) do { \
        uint32_t l = htonl((uint32_t)(len)); \
        fwrite(&l, 4, 1, fp); \
        fwrite(type, 1, 4, fp); \
        fwrite(data, 1, len, fp); \
        uint32_t crc = htonl(crc32(crc32(0, (const uint8_t*)type, 4), data, (uInt)(len))); \
        fwrite(&crc, 4, 1, fp); \
    } while(0)

    /* IHDR: width(4) height(4) bitdepth(1) colortype(1) compress(1) filter(1) interlace(1)
     * ref: libpng.org/pub/png/spec/1.2/PNG-Chunks.html#C.IHDR */
    uint8_t ihdr[13];
    uint32_t w = htonl(img->width), h = htonl(img->height);
    memcpy(ihdr,     &w, 4);
    memcpy(ihdr + 4, &h, 4);
    ihdr[8]  = PNG_BIT_DEPTH;
    ihdr[9]  = (img->channels == 3) ? PNG_COLOR_RGB   :
               (img->channels == 4) ? PNG_COLOR_RGBA  :
               (img->channels == 2) ? PNG_COLOR_GRAY_A : PNG_COLOR_GRAY;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    WRITE_CHUNK("IHDR", ihdr, 13);
    WRITE_CHUNK("IDAT", comp, comp_size);
    WRITE_CHUNK("IEND", (uint8_t *)"", 0);

    #undef WRITE_CHUNK

    free(comp);
    fclose(fp);
}

