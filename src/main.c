#include "../include/image.h"
#include "../include/ss.h"
#include "../include/lsb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s --extract [--detect] <image.png|jpg>\n"
        "  %s --extract --method ss --seed <n> <image.png|jpg>\n"
        "  %s --embed --method lsb --payload <text> <in.png|jpg> <out.png|jpg>\n"
        "  %s --embed --method ss  --payload <text> --seed <n> [--strength <n>] <in.png|jpg> <out.png|jpg>\n",
        prog, prog, prog, prog);
    exit(1);
}

static void cmd_extract(int argc, char *argv[]) {
    int         detect = 0;
    const char *method = "lsb";
    const char *path   = NULL;
    uint32_t    seed   = 42;

    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--detect") == 0) detect = 1;
        if (strcmp(argv[i], "--method") == 0) method = argv[++i];
        if (strcmp(argv[i], "--seed")   == 0) seed   = (uint32_t)atoi(argv[++i]);
    }
    path = argv[argc - 1];
    if (!path) usage("imgpoison");

    Image img = image_load(path);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    if (strcmp(method, "ss") == 0) {
        size_t   payload_len;
        uint8_t *payload = ss_extract(img.pixels, img.size,
                                      img.width, img.channels,
                                      seed, &payload_len);
        printf("Length   : %zu bytes\n", payload_len);
        printf("Payload  : %s\n", payload);
        free(payload);
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
    const char *method   = "lsb";
    const char *payload  = NULL;
    const char *input    = NULL;
    const char *output   = NULL;
    uint32_t    seed     = 42;
    uint32_t    strength = 10;  /* default — robust at q95 */

    for (int i = 0; i < argc - 2; i++) {
        if (strcmp(argv[i], "--method")   == 0) method   = argv[++i];
        if (strcmp(argv[i], "--payload")  == 0) payload  = argv[++i];
        if (strcmp(argv[i], "--seed")     == 0) seed     = (uint32_t)atoi(argv[++i]);
        if (strcmp(argv[i], "--strength") == 0) strength = (uint32_t)atoi(argv[++i]);
    }
    input  = argv[argc - 2];
    output = argv[argc - 1];

    if (!payload || !input || !output) usage("imgpoison");

    Image img = image_load(input);
    printf("Image    : %ux%u  channels=%u\n", img.width, img.height, img.channels);

    if (strcmp(method, "ss") == 0) {
        ss_embed(img.pixels, img.size, img.width, img.channels,
                 (const uint8_t *)payload, strlen(payload), seed, strength);
    } else {
        lsb_embed(img.pixels, img.size,
                  (const uint8_t *)payload, strlen(payload));
    }

    image_save(&img, output);
    printf("Saved    : %s\n", output);
    image_free(&img);
}

int main(int argc, char *argv[]) {
    if (argc < 2) usage(argv[0]);

    if (strcmp(argv[1], "--extract") == 0)
        cmd_extract(argc - 2, argv + 2);
    else if (strcmp(argv[1], "--embed") == 0)
        cmd_embed(argc - 2, argv + 2);
    else
        usage(argv[0]);

    return 0;
}

