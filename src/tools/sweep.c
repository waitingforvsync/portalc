// sweep.c - render every cell centroid at 32 angles, then a scripted
// pseudo-random camera walk, concatenating the raw framebuffers to a file.
// Two sweeps from different builds compare with cmp: byte-identical output
// proves a refactor changed nothing observable. --no-sprites leaves them
// out, for comparing what's under them.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "mode5.h"
#include "render.h"

static pt_map map;
static pt_render render;
static pt_vcache vc;

int main(int argc, char **argv)
{
    bool sprites = !(argc == 3 && strcmp(argv[1], "--no-sprites") == 0);
    if (argc != 2 && sprites) {
        fprintf(stderr, "usage: portalc_sweep [--no-sprites] out.bin\n");
        return 1;
    }
    const char *out = argv[argc - 1];

    pt_arith_init();
    pt_scene_init(&map);
    pt_vcache_init(&vc);
    pt_render_init(&render, &map, &vc);
    render.sprites = sprites;
    FILE *fp = fopen(out, "wb");
    if (!fp) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }

    for (uint8_t cell = 1; cell < map.num_cells; cell++) {
        const pt_cell *c = &map.cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            sx += map.verts[pt_map_side(&map, s)->v].x;
            sy += map.verts[pt_map_side(&map, s)->v].y;
        }
        for (uint32_t a = 0; a < 256; a += 8) {
            pt_render_frame(&render, (uint16_t)(sx / (pt_map_ring_end(c) - c->first)),
                            (uint16_t)(sy / (pt_map_ring_end(c) - c->first)), (uint8_t)a, cell);
            fwrite(render.fb, 1, PT_FB_BYTES, fp);
        }
    }

    pt_camera cam;
    pt_camera_init(&cam, &map, &vc, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);
    unsigned seed = 12345;
    uint8_t input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        seed = seed * 1664525u + 1013904223u;
        if (((seed >> 16) & 15) == 0) {
            unsigned r = seed >> 20;
            input = (uint8_t)(PT_IN_FWD | ((r & 1) ? PT_IN_TURNL : 0)
                              | ((r & 2) ? PT_IN_TURNR : 0) | ((r & 4) ? PT_IN_LEFT : 0)
                              | ((r & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, &map, &vc, input);
        if ((tick & 7) == 0) {
            pt_render_view(&render, cam.angle, cam.cell);
            fwrite(render.fb, 1, PT_FB_BYTES, fp);
        }
    }
    fclose(fp);
    return 0;
}
