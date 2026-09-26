// probe_fan.c - ground truth for portal transitions. Drive the camera across
// every portal-only fan vertex (one the centre can legally pass over) from
// all 256 directions, and after every tick check its cell assignment against
// exact point location. The camera may disagree with pt_map_find_cell only
// when the centre sits exactly on a boundary line - a benign tie, either
// side is legitimate - so the exact penetration into a wrongly-claimed
// cell's interior must always be zero.

#include <math.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "richc/test.h"

static pt_map map_;

RC_TEST(probe, fan_crossings)
{
    pt_arith_init();
    pt_scene_init(&map_);
    const pt_map *m = &map_;

    // vertices no solid edge touches: the camera can cross these fans
    uint8_t open[PT_MAX_VERTS];
    uint8_t nopen = 0;
    for (uint8_t v = 0; v < m->num_verts; v++) {
        bool solid = false;
        bool used = false;
        for (uint8_t c = 1; c < m->num_cells; c++) {
            const pt_cell *cc = &m->cells[c];
            for (uint8_t k = 0; k < cc->num_edges; k++) {
                uint8_t e = (uint8_t)(cc->first_edge + k);
                uint8_t v1 = m->edges[pt_map_ring_next(cc, e)].v0;
                if (m->edges[e].v0 == v || v1 == v) {
                    used = true;
                    if (m->edges[e].adj == 0)
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
            // start 24 units before the vertex, walk 6 ticks (48 units)
            // through it
            double ang = a * (3.14159265358979323846 * 2.0 / 256.0);
            double sx = vx - cos(ang) * 24.0;
            double sy = vy - sin(ang) * 24.0;
            if (pt_map_find_cell(m, (uint16_t)sx, (uint16_t)sy) == 0)
                continue;
            pt_camera cam;
            pt_camera_init(&cam, m, (uint16_t)sx, (uint16_t)sy, (uint8_t)a);
            for (int t = 0; t < 6; t++) {
                pt_camera_tick(&cam, m, PT_IN_FWD);
                uint8_t want = pt_map_find_cell(m, (uint16_t)(cam.x >> 8),
                                                (uint16_t)(cam.y >> 8));
                if (want == 0 || want == cam.cell)
                    continue;
                // a mismatch: the centre must lie exactly on the claimed
                // cell's boundary, never strictly inside another cell
                const pt_cell *cc = &m->cells[cam.cell];
                int32_t minside = INT32_MAX;
                for (uint8_t k = 0; k < cc->num_edges; k++) {
                    int32_t side = pt_map_edge_side(m, cc, (uint8_t)(cc->first_edge + k),
                                                    (uint16_t)(cam.x >> 8),
                                                    (uint16_t)(cam.y >> 8));
                    if (side < minside)
                        minside = side;
                }
                RC_CHECK(minside, ==, (int32_t)0);
            }
        }
    }
}
