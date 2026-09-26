// fb2ppm.c - extract one frame of a sweep dump (a concatenation of raw
// framebuffers, as portalc_sweep writes) as a PPM image, pixels doubled
// horizontally for MODE 5's aspect.

#include <stdio.h>
#include <stdlib.h>

#include "mode5.h"

static uint8_t fb[PT_FB_BYTES];
static uint8_t rgba[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: portalc_fb2ppm sweep.bin out.ppm frame\n");
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    fseek(in, atol(argv[3]) * PT_FB_BYTES, SEEK_SET);
    if (fread(fb, 1, PT_FB_BYTES, in) != PT_FB_BYTES) {
        fprintf(stderr, "no frame %s in %s\n", argv[3], argv[1]);
        fclose(in);
        return 1;
    }
    fclose(in);

    pt_mode5_to_rgba(fb, rgba);
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", PT_SCREEN_WIDTH * 2, PT_SCREEN_SCANLINES);
    for (int y = 0; y < PT_SCREEN_SCANLINES; y++)
        for (int x = 0; x < PT_SCREEN_WIDTH; x++) {
            fwrite(&rgba[(y * PT_SCREEN_WIDTH + x) * 4], 1, 3, out);
            fwrite(&rgba[(y * PT_SCREEN_WIDTH + x) * 4], 1, 3, out);
        }
    fclose(out);
    return 0;
}
