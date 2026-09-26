// probe_collision.c - ground truth for wall collision. A long pseudo-random
// walkthrough, measuring after every tick the exact closest approach of the
// camera centre to any solid wall segment of its cell or of an overlapped
// neighbour. The resolve invariant holds the centre near PT_CAM_RADIUS; the
// quantised edge angles and the corner arc's outward drift admit a few
// units of shortfall (29.17 observed at radius 40), so the tripwire sits at
// 28 - a regression in resolve, slide or transition trips it long before
// the camera visibly enters a wall.

#include <math.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "richc/test.h"

static pt_map map_;

// Exact distance from a point to the edge's segment.
static double seg_dist(const pt_map *m, const pt_cell *c, uint8_t e, double x, double y)
{
    const pt_edge *ed = &m->edges[e];
    double ax = m->verts[ed->v0].x;
    double ay = m->verts[ed->v0].y;
    uint8_t n = pt_map_ring_next(c, e);
    double bx = m->verts[m->edges[n].v0].x;
    double by = m->verts[m->edges[n].v0].y;
    double dx = bx - ax;
    double dy = by - ay;
    double t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    double px = ax + t * dx - x;
    double py = ay + t * dy - y;
    return sqrt(px * px + py * py);
}

RC_TEST(probe, collision_distance)
{
    pt_arith_init();
    pt_scene_init(&map_);
    const pt_map *m = &map_;

    pt_camera cam;
    pt_camera_init(&cam, m, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);

    unsigned seed = 12345;
    uint8_t input = PT_IN_FWD;
    double worst = 1e9;
    for (unsigned tick = 0; tick < 40000; tick++) {
        seed = seed * 1664525u + 1013904223u;
        if (((seed >> 16) & 15) == 0) {
            unsigned r = seed >> 20;
            input = (uint8_t)(PT_IN_FWD | ((r & 1) ? PT_IN_TURNL : 0)
                              | ((r & 2) ? PT_IN_TURNR : 0) | ((r & 4) ? PT_IN_LEFT : 0)
                              | ((r & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, m, input);

        double x = cam.x / 256.0;
        double y = cam.y / 256.0;
        const pt_cell *cc = &m->cells[cam.cell];
        for (uint8_t k = 0; k < cc->num_edges; k++) {
            uint8_t ei = (uint8_t)(cc->first_edge + k);
            const pt_edge *ed = &m->edges[ei];
            if (ed->adj == 0) {
                double d = seg_dist(m, cc, ei, x, y);
                if (d < worst)
                    worst = d;
            } else {
                const pt_cell *nb = &m->cells[ed->adj];
                for (uint8_t j = 0; j < nb->num_edges; j++) {
                    uint8_t nj = (uint8_t)(nb->first_edge + j);
                    if (m->edges[nj].adj != 0)
                        continue;
                    double d = seg_dist(m, nb, nj, x, y);
                    if (d < worst)
                        worst = d;
                }
            }
        }
    }

    RC_CHECK_TRUE(worst >= 28.0);
    // and the walk really pressed against walls: the invariant is exercised
    RC_CHECK_TRUE(worst < (double)PT_CAM_RADIUS);
}
