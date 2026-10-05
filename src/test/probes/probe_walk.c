// probe_walk.c - the render walk from every reachable camera state. Drive
// the camera through every portal-only fan vertex from all 256 directions
// and render every post-tick state at 8 spread facings with the walk's
// internal asserts live: no reachable state may break the walk - including
// rest states on or within rounding of a portal line, which the walk's
// structural guards must handle outright.

#include <math.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "render.h"
#include "richc/test.h"

static pt_map map_;
static pt_render render_;
static pt_vcache vc_;

RC_TEST(probe, walk_from_reachable_states)
{
    pt_arith_init();
    pt_scene_init(&map_);
    pt_vcache_init(&vc_);
    pt_render_init(&render_, &map_, &vc_);
    const pt_map *m = &map_;

    uint8_t open[PT_MAX_VERTS];
    uint8_t nopen = 0;
    for (uint8_t v = 0; v < m->num_verts; v++) {
        bool solid = false;
        bool used = false;
        for (uint8_t c = 1; c < m->num_cells; c++) {
            const pt_cell *cc = &m->cells[c];
            for (uint8_t k = 0; k <= cc->num; k++) {
                const pt_side *sd = pt_map_side(m, cc, k);
                if (sd->v == v || pt_map_side_v1(m, cc, k) == v) {
                    used = true;
                    if (sd->adj == 0)
                        solid = true;
                }
            }
        }
        if (used && !solid)
            open[nopen++] = v;
    }
    RC_CHECK(nopen, >=, (uint8_t)1);

    for (uint8_t i = 0; i < nopen; i++) {
        uint16_t vx = m->verts[open[i]].x;
        uint16_t vy = m->verts[open[i]].y;
        for (unsigned a = 0; a < 256; a++) {
            double ang = a * (3.14159265358979323846 * 2.0 / 256.0);
            double sx = vx - cos(ang) * 24.0;
            double sy = vy - sin(ang) * 24.0;
            if (pt_map_find_cell(m, (uint16_t)sx, (uint16_t)sy) == 0)
                continue;
            pt_camera cam;
            pt_camera_init(&cam, m, &vc_, (uint16_t)sx, (uint16_t)sy, (uint8_t)a);
            for (int t = 0; t < 6; t++) {
                pt_camera_tick(&cam, m, &vc_, PT_IN_FWD);
                uint16_t px = (uint16_t)(cam.x >> 8);
                uint16_t py = (uint16_t)(cam.y >> 8);
                for (unsigned r = 0; r < 8; r++)
                    pt_render_frame(&render_, px, py, (uint8_t)(r * 32 + t * 4 + a), cam.cell);
            }
        }
    }
}
