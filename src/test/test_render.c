// test_render.c - render smoke tests: the walk's internal invariants are
// asserted throughout render.c, so driving it across many poses in a debug
// build is a broad structural check.

#include <string.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "render.h"
#include "richc/random.h"
#include "richc/test.h"

static pt_map map_;
static pt_render render_;
static pt_vcache vc_;

static void centroid(const pt_map *m, uint8_t cell, uint16_t *x, uint16_t *y)
{
    const pt_cell *c = &m->cells[cell];
    uint32_t sx = 0;
    uint32_t sy = 0;
    for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
        sx += m->verts[pt_map_side(m, s)->v].x;
        sy += m->verts[pt_map_side(m, s)->v].y;
    }
    *x = (uint16_t)(sx / (pt_map_ring_end(c) - c->first));
    *y = (uint16_t)(sy / (pt_map_ring_end(c) - c->first));
}

RC_TEST(render, all_cells_all_angles)
{
    pt_arith_init();
    pt_scene_init(&map_);
    pt_vcache_init(&vc_);
    pt_render_init(&render_, &map_, &vc_);

    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        uint16_t x;
        uint16_t y;
        centroid(&map_, cell, &x, &y);
        for (uint32_t a = 0; a < 256; a += 8) {
            pt_render_frame(&render_, x, y, (uint8_t)a, cell);
            RC_CHECK(render_.num_visits, <=, (uint8_t)PT_MAX_VISITS);
            RC_CHECK(render_.depth_high, <, (uint8_t)PT_MAX_DEPTH);    // never cut off
        }
    }
}

RC_TEST(render, deterministic)
{
    pt_arith_init();
    pt_scene_init(&map_);
    pt_vcache_init(&vc_);
    pt_render_init(&render_, &map_, &vc_);

    static uint8_t fb0[PT_FB_BYTES];
    pt_render_frame(&render_, 1756, 1536, 128, 2);
    memcpy(fb0, render_.fb, PT_FB_BYTES);
    pt_render_frame(&render_, 1756, 1536, 128, 2);
    RC_CHECK(memcmp(fb0, render_.fb, PT_FB_BYTES), ==, 0);
}

RC_TEST(render, random_walkthrough)
{
    pt_arith_init();
    pt_scene_init(&map_);
    pt_vcache_init(&vc_);
    pt_render_init(&render_, &map_, &vc_);

    pt_camera cam;
    pt_camera_init(&cam, &map_, &vc_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);

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
        pt_camera_tick(&cam, &map_, &vc_, input);
        RC_CHECK(cam.cell, >=, (uint8_t)1);
        RC_CHECK(cam.cell, <, map_.num_cells);

        if ((tick & 7) == 0) {
            pt_render_view(&render_, cam.angle, cam.cell);
            RC_CHECK(render_.num_visits, <=, (uint8_t)PT_MAX_VISITS);
        }
    }
}
