#include "../include/image.h"
#include "../include/ss.h"
#include "../include/lsb.h"
#include "../include/analyze.h"
#include "../include/texture.h"
#include "../include/formats.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s --extract [--detect] <image.png|jpg>\n"
        "  %s --extract --method ss --seed <n> [--mask <file>] [--auto-mask <gamma>] [--chip-size <n>] [--payload-repeat <n>] <image.png|jpg>\n"
        "  %s --embed --method lsb --payload <text> <in.png|jpg> <out.png|jpg>\n"
        "  %s --embed --method ss  --payload <text> --seed <n> [--strength <n>] [--mask <file>] [--auto-mask <gamma>] [--chip-size <n>] [--payload-repeat <n>] <in.png|jpg> <out.png|jpg>\n"
        "  %s --analyze [--method lsb|ss] [--seed <n>] <image.png|jpg>\n"
        "  %s --bitacc --payload <text> --seed <n> [--mask <file>] [--auto-mask <gamma>] [--chip-size <n>] [--payload-repeat <n>] <image.png|jpg>\n"
        "\n"
        "--chip-size and --payload-repeat default to CHIP_SIZE/PAYLOAD_REPEAT\n"
        "(formats.h) when not given -- the robust operating point. A smaller\n"
        "chip-size and --payload-repeat 1 trade that robustness for enough\n"
        "resolution to see a signal degrade under attack instead of just\n"
        "whether it survived. Must match between --embed and the matching\n"
        "--extract/--bitacc, same as --seed.\n",
        prog, prog, prog, prog, prog, prog);
    exit(1);
}

/* validates chip_size >= 1 and payload_repeat odd and >= 1 -- payload_repeat
 * even would let a majority vote tie, chip_size 0 would divide-by-zero
 * downstream (SNR estimate, capacity check). exits with a clear message
 * instead of the tool misbehaving three calls later. */
static void validate_ss_params(uint32_t chip_size, uint32_t payload_repeat) {
    if (chip_size < 1) {
        fprintf(stderr, "--chip-size must be at least 1\n");
        exit(1);
    }
    if (payload_repeat < 1 || payload_repeat % 2 == 0) {
        fprintf(stderr, "--payload-repeat must be odd and at least 1 (got %u) "
                        "- an even value could tie the majority vote\n", payload_repeat);
        exit(1);
    }
}

/* load a per-pixel mask from a flat binary file of floats, one entry
 * per pixel in raster order (width*height entries, not width*height*
 * channels - mask applies once per pixel across all its channels,
 * see embed_bit() in embed_ss.c). returns NULL on any error, with
 * a reason on stderr. */
static float *mask_load(const char *path, size_t n_pixels) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open mask file: %s\n", path);
        return NULL;
    }

    float *mask = malloc(n_pixels * sizeof(float));
    size_t read = fread(mask, sizeof(float), n_pixels, f);
    fclose(f);

    if (read != n_pixels) {
        fprintf(stderr, "mask file size mismatch: expected %zu floats, got %zu\n",
                n_pixels, read);
        free(mask);
        return NULL;
    }
    return mask;
}

static void cmd_extract(int argc, char *argv[]) {
    int         detect    = 0;
    const char *method    = "lsb";
    const char *path      = NULL;
    const char *mask_path = NULL;
    int         auto_mask = 0;
    float       auto_gamma = 1.0f;
    uint32_t    seed      = 42;
    uint32_t    chip_size = CHIP_SIZE;
    uint32_t    payload_repeat = PAYLOAD_REPEAT;

    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--detect")          == 0) detect         = 1;
        if (strcmp(argv[i], "--method")          == 0) method         = argv[++i];
        if (strcmp(argv[i], "--seed")            == 0) seed           = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--mask")            == 0) mask_path      = argv[++i];
        if (strcmp(argv[i], "--auto-mask")       == 0) { auto_mask = 1; auto_gamma = (float)atof(argv[++i]); }
        if (strcmp(argv[i], "--chip-size")       == 0) chip_size      = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--payload-repeat")  == 0) payload_repeat = (uint32_t)atoi(argv[++i]);
    }
    path = argv[argc - 1];
    if (!path) usage("imgpoison");
    if (mask_path && auto_mask) {
        fprintf(stderr, "--mask and --auto-mask are mutually exclusive\n");
        exit(1);
    }
    validate_ss_params(chip_size, payload_repeat);

    Image img = image_load(path);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    if (strcmp(method, "ss") == 0) {
        /* the mask is public, recomputed from the received image itself,
         * not something transmitted alongside the payload. embed_bit
         * scales the signal by mask, so extract_bit must weight the
         * correlation by the same mask or it is not the matched filter -
         * see extract_bit's comment in embed_ss.c. */
        float *mask = NULL;
        float *gray_buf = NULL, *tex_buf = NULL;
        if (mask_path) {
            mask = mask_load(mask_path, (size_t)img.width * img.height);
            if (!mask) exit(1);
        } else if (auto_mask) {
            gray_buf = texture_luma(img.pixels, img.width, img.height, img.channels);
            tex_buf  = texture_compute(gray_buf, img.width, img.height);
            mask     = texture_gamma_mask(tex_buf, img.width, img.height, auto_gamma);
        }

        size_t   payload_len;
        uint8_t *payload = ss_extract(img.pixels, img.size,
                                      img.width, img.channels,
                                      seed, &payload_len, mask,
                                      chip_size, payload_repeat);
        printf("Length   : %zu bytes\n", payload_len);
        printf("Payload  : %s\n", payload);
        free(payload);
        free(mask);
        free(gray_buf);
        free(tex_buf);
    } else {
        if (detect) {
            lsb_detect(img.pixels, img.size);
        } else {
            size_t   payload_len;
            uint8_t *payload = lsb_extract(img.pixels, img.size, &payload_len);
            printf("Length   : %zu bytes\n", payload_len);
            printf("Payload  : %s\n", payload);
            free(payload);
        }
    }
    image_free(&img);
}

static void cmd_embed(int argc, char *argv[]) {
    const char *method     = "lsb";
    const char *payload    = NULL;
    const char *input      = NULL;
    const char *output     = NULL;
    const char *mask_path  = NULL;
    int         auto_mask  = 0;
    float       auto_gamma = 1.0f;
    uint32_t    seed       = 42;
    uint32_t    strength   = 10;  // how hard the signal is pushed into the pixels
                                  // higher = more robust after JPEG, more visible.
                                  // 10 survives JPEG at quality 95 with no bit errors
    uint32_t    chip_size      = CHIP_SIZE;
    uint32_t    payload_repeat = PAYLOAD_REPEAT;

    for (int i = 0; i < argc - 2; i++) {
        if (strcmp(argv[i], "--method")          == 0) method         = argv[++i];
        if (strcmp(argv[i], "--payload")         == 0) payload        = argv[++i];
        if (strcmp(argv[i], "--seed")            == 0) seed           = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--strength")        == 0) strength       = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--mask")            == 0) mask_path      = argv[++i];
        if (strcmp(argv[i], "--auto-mask")       == 0) { auto_mask = 1; auto_gamma = (float)atof(argv[++i]); }
        if (strcmp(argv[i], "--chip-size")       == 0) chip_size      = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--payload-repeat")  == 0) payload_repeat = (uint32_t)atoi(argv[++i]);
    }
    input  = argv[argc - 2];
    output = argv[argc - 1];

    if (!payload || !input || !output) usage("imgpoison");
    if (mask_path && auto_mask) {
        fprintf(stderr, "--mask and --auto-mask are mutually exclusive\n");
        exit(1);
    }
    validate_ss_params(chip_size, payload_repeat);

    Image img = image_load(input);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    float *mask = NULL;
    if (mask_path) {
        mask = mask_load(mask_path, (size_t)img.width * img.height);
        if (!mask) exit(1);
    } else if (auto_mask) {
        /* structure tensor texture mask, computed straight from this
         * image. no python, no external file. see texture.c for the
         * math and the three bugs it carries fixes for. */
        float *gray = texture_luma(img.pixels, img.width, img.height, img.channels);
        float *tex  = texture_compute(gray, img.width, img.height);
        mask = texture_gamma_mask(tex, img.width, img.height, auto_gamma);
        free(gray);
        free(tex);
        printf("Mask     : auto, gamma=%.2f\n", auto_gamma);
    }

    if (strcmp(method, "ss") == 0) {
        ss_embed(img.pixels, img.size, img.width, img.channels,
                 (const uint8_t *)payload, strlen(payload), seed, strength, mask,
                 chip_size, payload_repeat);
    } else {
        lsb_embed(img.pixels, img.size,
                  (const uint8_t *)payload, strlen(payload));
    }

    free(mask);
    image_save(&img, output);
    printf("Saved    : %s\n", output);
    image_free(&img);
}

static void cmd_bitacc(int argc, char *argv[]) {
    const char *payload   = NULL;
    const char *path      = NULL;
    const char *mask_path = NULL;
    int         auto_mask = 0;
    float       auto_gamma = 1.0f;
    uint32_t    seed      = 42;
    uint32_t    chip_size      = CHIP_SIZE;
    uint32_t    payload_repeat = PAYLOAD_REPEAT;

    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--payload")         == 0) payload        = argv[++i];
        if (strcmp(argv[i], "--seed")            == 0) seed           = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--mask")            == 0) mask_path      = argv[++i];
        if (strcmp(argv[i], "--auto-mask")       == 0) { auto_mask = 1; auto_gamma = (float)atof(argv[++i]); }
        if (strcmp(argv[i], "--chip-size")       == 0) chip_size      = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--payload-repeat")  == 0) payload_repeat = (uint32_t)atoi(argv[++i]);
    }
    path = argv[argc - 1];
    if (!payload || !path) usage("imgpoison");
    if (mask_path && auto_mask) {
        fprintf(stderr, "--mask and --auto-mask are mutually exclusive\n");
        exit(1);
    }
    validate_ss_params(chip_size, payload_repeat);

    Image img = image_load(path);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    float *mask = NULL;
    float *gray_buf = NULL, *tex_buf = NULL;
    if (mask_path) {
        mask = mask_load(mask_path, (size_t)img.width * img.height);
        if (!mask) exit(1);
    } else if (auto_mask) {
        gray_buf = texture_luma(img.pixels, img.width, img.height, img.channels);
        tex_buf  = texture_compute(gray_buf, img.width, img.height);
        mask     = texture_gamma_mask(tex_buf, img.width, img.height, auto_gamma);
    }

    float acc = ss_bit_accuracy(img.pixels, img.size, img.width, img.channels,
                                (const uint8_t *)payload, strlen(payload), seed, mask,
                                chip_size, payload_repeat);
    printf("Bit acc  : %.4f (%.1f%%)\n", acc, 100.0 * acc);

    free(mask);
    free(gray_buf);
    free(tex_buf);
    image_free(&img);
}

static void cmd_analyze(int argc, char *argv[]) {
    const char *method = "ss";
    const char *path   = NULL;
    uint32_t    seed   = 42;

    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--method") == 0) method = argv[++i];
        if (strcmp(argv[i], "--seed")   == 0) seed   = (uint32_t)atoi(argv[++i]);
    }
    path = argv[argc - 1];
    if (!path) usage("imgpoison");

    Image img = image_load(path);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    if (strcmp(method, "lsb") == 0) {
        lsb_detect(img.pixels, img.size);
    } else {
        ss_analyze(img.pixels, img.size, img.width, img.channels, seed);
    }
    image_free(&img);
}

int main(int argc, char *argv[]) {
    if (argc < 2) usage(argv[0]);

    if (strcmp(argv[1], "--extract") == 0)
        cmd_extract(argc - 2, argv + 2);
    else if (strcmp(argv[1], "--embed") == 0)
        cmd_embed(argc - 2, argv + 2);
    else if (strcmp(argv[1], "--analyze") == 0)
        cmd_analyze(argc - 2, argv + 2);
    else if (strcmp(argv[1], "--bitacc") == 0)
        cmd_bitacc(argc - 2, argv + 2);
    else
        usage(argv[0]);

    return 0;
}

