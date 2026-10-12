// test_map.c - structural invariants of the demo scene and the stress map.

#include <math.h>

#include "arith.h"
#include "map.h"
#include "richc/test.h"

// The rings, and the edges they name.
static void check_structure(const pt_map *m)
{
    RC_CHECK_TRUE(pt_map_validate(m));

    // the rings partition sides[], each [first, last) and stored clockwise:
    // clockwise is counting up, from the last back round to the first
    uint16_t total = 0;
    uint16_t walls = 0;
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        RC_CHECK(pt_map_ring_end(cell) - cell->first, >=, 3);
        RC_CHECK(cell->first, ==, (uint8_t)total);
        total = pt_map_ring_end(cell);
        for (uint8_t s = cell->first; s < pt_map_ring_end(cell); s++) {
            uint8_t want = s + 1 == pt_map_ring_end(cell) ? cell->first : (uint8_t)(s + 1);
            RC_CHECK(pt_map_ring_next(cell, s), ==, want);

            // across a portal, the twin is the same edge the other way round
            const pt_side *sd = pt_map_side(m, s);
            if (sd->adj == 0) {
                walls++;
                continue;
            }
            const pt_cell *d = &m->cells[sd->adj];
            uint8_t ts = pt_map_twin(m, s);
            RC_CHECK(ts >= d->first && ts < pt_map_ring_end(d), ==, true);
            const pt_side *t = pt_map_side(m, ts);
            RC_CHECK(t->edge, ==, sd->edge);
            RC_CHECK(t->adj, ==, c);
            RC_CHECK(t->v, ==, pt_map_side_v1(m, cell, s));
            RC_CHECK(pt_map_side_v1(m, d, ts), ==, sd->v);
        }
    }
    RC_CHECK(total, ==, (uint16_t)m->num_sides);
    RC_CHECK((uint16_t)m->num_edges, ==, (uint16_t)((total + walls) / 2));

    // the angle byte, from every side: the quadrant from its direction's
    // signs and the byte make the true angle's 1024th
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        for (uint8_t s = cell->first; s < pt_map_ring_end(cell); s++) {
            pt_vertex a = m->verts[pt_map_side(m, s)->v];
            pt_vertex b = m->verts[pt_map_side_v1(m, cell, s)];
            int32_t dx = (int32_t)b.x - a.x;
            int32_t dy = (int32_t)b.y - a.y;
            double t = atan2((double)dy, (double)dx) * (512.0 / 3.14159265358979323846);
            if (t < 0)
                t += 1024.0;
            uint16_t q = pt_quadrant((int16_t)dx, (int16_t)dy);
            double d = (q << 8 | m->edges[pt_map_side(m, s)->edge].angle) - t;
            RC_CHECK(d > -1.0 - 1e-9 && d < 1e-9, ==, true);
        }
    }

    // collision's frame: a side's own vector, dotted with its direction,
    // lands along it (at least the foot cap) and, across it, within the
    // angle's step and the products' rounding of its line
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        for (uint8_t s = cell->first; s < pt_map_ring_end(cell); s++) {
            const pt_side *sd = pt_map_side(m, s);
            pt_vertex a = m->verts[sd->v];
            pt_vertex b = m->verts[pt_map_side_v1(m, cell, s)];
            int16_t dx = (int16_t)(b.x - a.x);
            int16_t dy = (int16_t)(b.y - a.y);
            uint16_t ang = (uint16_t)(pt_quadrant(dx, dy) << 8 | m->edges[sd->edge].angle);
            int16_t along = pt_dot_dir(dx, dy, ang);
            int16_t across = pt_dot_dir(dx, dy, (uint16_t)(ang + 256));
            RC_CHECK(along / 8, >=, (int16_t)m->edges[sd->edge].len8);
            int32_t tilt = (along / 128) + 2;    // len * 2 pi / 1024, and rounding
            RC_CHECK(across, <=, (int16_t)tilt);
            RC_CHECK(across, >=, (int16_t)-tilt);
        }
    }

    // the point-to-cell query maps every cell's centroid back to that cell,
    // and a point beyond the map to the outside
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        uint32_t cx = 0;
        uint32_t cy = 0;
        for (uint8_t s = cell->first; s < pt_map_ring_end(cell); s++) {
            cx += m->verts[pt_map_side(m, s)->v].x;
            cy += m->verts[pt_map_side(m, s)->v].y;
        }
        uint16_t px = (uint16_t)(cx / (pt_map_ring_end(cell) - cell->first));
        uint16_t py = (uint16_t)(cy / (pt_map_ring_end(cell) - cell->first));
        RC_CHECK(pt_map_find_cell(m, px, py), ==, c);
    }
    RC_CHECK(pt_map_find_cell(m, 10, 10), ==, (uint8_t)0);
}

RC_TEST(map, scene_valid)
{
    static pt_map m;
    pt_arith_init();
    pt_scene_init(&m);
    RC_CHECK(m.num_cells, ==, (uint8_t)28);    // 27 cells plus the reserved outside
    check_structure(&m);
}

RC_TEST(map, stress_valid)
{
    static pt_map m;
    pt_arith_init();
    pt_stress_scene_init(&m);
    check_structure(&m);
}
