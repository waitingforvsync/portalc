// test_render.c - render smoke tests: the walk's internal invariants are
// asserted throughout render.c, so driving it across many poses in a debug
// build is a broad structural check.

#include <string.h>

#include "camera.h"
#include "map.h"
#include "render.h"
#include "richc/random.h"
#include "richc/test.h"

static pt_map map_;
static pt_render render_;

static void centroid(const pt_map *m, uint8_t cell, uint16_t *x, uint16_t *y)
{
    const pt_cell *c = &m->cells[cell];
    uint32_t sx = 0;
    uint32_t sy = 0;
    for (uint8_t k = 0; k < c->num_edges; k++) {
        sx += m->verts[m->edges[c->first_edge + k].v0].x;
        sy += m->verts[m->edges[c->first_edge + k].v0].y;
    }
    *x = (uint16_t)(sx / c->num_edges);
    *y = (uint16_t)(sy / c->num_edges);
}

RC_TEST(render, all_cells_all_angles)
{
    pt_scene_build(&map_);
    pt_render_init(&render_, &map_);

    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        uint16_t x;
        uint16_t y;
        centroid(&map_, cell, &x, &y);
        for (uint32_t a = 0; a < 256; a += 8) {
            pt_render_frame(&render_, x, y, (uint8_t)a, cell);
            RC_CHECK(render_.pool_num, <=, (uint16_t)PT_POOL_SIZE);
            RC_CHECK(render_.depth, ==, (uint8_t)0);
        }
    }
}

RC_TEST(render, deterministic)
{
    pt_scene_build(&map_);
    pt_render_init(&render_, &map_);

    static uint8_t fb0[PT_FB_BYTES];
    pt_render_frame(&render_, PT_WORLD_OX + 220, PT_WORLD_OY + 0, 128, 2);
    memcpy(fb0, render_.fb, PT_FB_BYTES);
    pt_render_frame(&render_, PT_WORLD_OX + 220, PT_WORLD_OY + 0, 128, 2);
    RC_CHECK(memcmp(fb0, render_.fb, PT_FB_BYTES), ==, 0);
}

RC_TEST(render, random_walkthrough)
{
    pt_scene_build(&map_);
    pt_render_init(&render_, &map_);

    pt_camera cam;
    pt_camera_init(&cam, &map_, 2, PT_WORLD_OX + 220, PT_WORLD_OY + 0, 128);

    rc_random rng = rc_random_make(12345);
    uint8_t input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        // wander: mostly keep going, sometimes change plan
        if ((rc_random_next(&rng) & 15) == 0) {
            uint32_t r = rc_random_next(&rng);
            input = (uint8_t)(PT_IN_FWD | ((r & 1) ? PT_IN_TURNL : 0)
                              | ((r & 2) ? PT_IN_TURNR : 0) | ((r & 4) ? PT_IN_LEFT : 0)
                              | ((r & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, &map_, input);
        RC_CHECK(cam.cell, >=, (uint8_t)1);
        RC_CHECK(cam.cell, <, map_.num_cells);

        if ((tick & 7) == 0) {
            pt_render_frame(&render_, (uint16_t)(cam.x >> 8), (uint16_t)(cam.y >> 8), cam.angle,
                            cam.cell);
            RC_CHECK(render_.pool_num, <=, (uint16_t)PT_POOL_SIZE);
        }
    }
}
