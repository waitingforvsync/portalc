// probe_fan.c - ground truth for portal transitions. Drive the camera across
// every portal-only fan vertex (one the centre can legally pass over) from
// all 256 directions, and after every tick check its cell assignment against
// exact point location. The camera's cell comes from the angles of the
// vertices as seen from it, so near a portal's line it may disagree: but
// only by as far as those angles' error reaches. A point d from a side's
// line sees its two vertices d (1/r0 + 1/r1) radians off a half turn apart,
// r0 and r1 its distances from them, and each angle is within 1.82 steps
// of the truth, so it may lie up to 3.64 steps' worth beyond the side, and
// half a unit's diagonal more: the angles look from the middle of the
// position's whole-unit square.

#include <math.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "richc/test.h"

static pt_map map_;
static pt_vcache vc_;

RC_TEST(probe, fan_crossings)
{
    pt_arith_init();
    pt_scene_init(&map_);
    pt_vcache_init(&vc_);
    const pt_map *m = &map_;

    // vertices no solid edge touches: the camera can cross these fans
    uint8_t open[PT_MAX_VERTS];
    uint8_t nopen = 0;
    for (uint8_t v = 0; v < m->num_verts; v++) {
        bool solid = false;
        bool used = false;
        for (uint8_t c = 1; c < m->num_cells; c++) {
            const pt_cell *cc = &m->cells[c];
            for (uint8_t s = cc->first; s < pt_map_ring_end(cc); s++) {
                const pt_side *sd = pt_map_side(m, s);
                if (sd->v == v || pt_map_side_v1(m, cc, s) == v) {
                    used = true;
                    if (sd->adj == 0)
                        solid = true;
                }
            }
        }
        if (used && !solid)
            open[nopen++] = v;
    }
    RC_CHECK(nopen, >=, (uint8_t)1);    // the demo scene must exercise fans

    for (uint8_t i = 0; i < nopen; i++) {
        uint16_t vx = m->verts[open[i]].x;
        uint16_t vy = m->verts[open[i]].y;
        for (unsigned a = 0; a < 256; a++) {
            // start 24 units before the vertex, walk 6 frames of 8 ticks
            // (48 units) through it
            double ang = a * (3.14159265358979323846 * 2.0 / 256.0);
            double sx = vx - cos(ang) * 24.0;
            double sy = vy - sin(ang) * 24.0;
            if (pt_map_find_cell(m, (uint16_t)sx, (uint16_t)sy) == 0)
                continue;
            pt_camera cam;
            pt_camera_init(&cam, m, &vc_, (uint16_t)sx, (uint16_t)sy, (uint8_t)a);
            for (int t = 0; t < 6; t++) {
                pt_camera_tick(&cam, m, &vc_, PT_IN_FWD, PT_CAM_MAX_TICKS);
                uint8_t want = pt_map_find_cell(m, (uint16_t)(cam.x >> 8),
                                                (uint16_t)(cam.y >> 8));
                if (want == 0 || want == cam.cell)
                    continue;
                // a mismatch: the centre may lie outside the claimed cell
                // only within the angles' reach of the side it's beyond
                const pt_cell *cc = &m->cells[cam.cell];
                double px = (double)(cam.x >> 8);
                double py = (double)(cam.y >> 8);
                for (uint8_t s = cc->first; s < pt_map_ring_end(cc); s++) {
                    pt_vertex a = m->verts[pt_map_side(m, s)->v];
                    pt_vertex b = m->verts[pt_map_side_v1(m, cc, s)];
                    double len = hypot((double)b.x - a.x, (double)b.y - a.y);
                    double d = pt_map_edge_side(m, cc, s, (uint16_t)px, (uint16_t)py) / len;
                    if (d >= 0)
                        continue;
                    double r0 = hypot(a.x - px, a.y - py);
                    double r1 = hypot(b.x - px, b.y - py);
                    double reach = 3.64 * (2 * 3.14159265358979323846 / 1024) * r0 * r1 / (r0 + r1);
                    RC_CHECK(-d, <=, reach + 0.71);    // the angles look from the square's middle
                }
            }
        }
    }
}
