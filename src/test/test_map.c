// test_map.c - structural invariants of the demo scene and the stress map.

#include <math.h>

#include "arith.h"
#include "map.h"
#include "richc/test.h"

// The old view's half for position k of cell c's ring: the same range,
// reversed.
static uint8_t half_of(const pt_cell *c, uint8_t k)
{
    return (uint8_t)(c->first + c->num - k);
}

// The rings, the edges they name, and the old view derived from them.
static void check_structure(const pt_map *m)
{
    RC_CHECK_TRUE(pt_map_validate(m));

    // the rings partition sides[], stored counter-clockwise: clockwise is
    // counting down, from the first back round to the last
    uint16_t total = 0;
    uint16_t walls = 0;
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        RC_CHECK(cell->num, >=, (uint8_t)2);
        RC_CHECK(cell->first, ==, (uint8_t)total);
        total += cell->num + 1;
        for (uint8_t k = 0; k <= cell->num; k++) {
            uint8_t want = k == 0 ? cell->num : (uint8_t)(k - 1);
            RC_CHECK(pt_map_ring_next(cell, k), ==, want);

            // across a portal, the twin is the same edge the other way round
            const pt_side *sd = pt_map_side(m, cell, k);
            if (sd->adj == 0) {
                walls++;
                continue;
            }
            const pt_cell *d = &m->cells[sd->adj];
            const pt_side *t = pt_map_side(m, d, sd->twin);
            RC_CHECK(t->edge, ==, sd->edge);
            RC_CHECK(t->adj, ==, c);
            RC_CHECK(t->v, ==, pt_map_side_v1(m, cell, k));
            RC_CHECK(pt_map_side_v1(m, d, sd->twin), ==, sd->v);
            RC_CHECK(pt_map_twin(m, half_of(cell, k)), ==, half_of(d, sd->twin));
        }
    }
    RC_CHECK(total, ==, (uint16_t)m->num_sides);
    RC_CHECK((uint16_t)m->num_edges, ==, (uint16_t)((total + walls) / 2));

    // the angle byte, from every side: the quadrant from its direction's
    // signs and the byte make the true angle's 1024th
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        for (uint8_t k = 0; k <= cell->num; k++) {
            pt_vertex a = m->verts[pt_map_side(m, cell, k)->v];
            pt_vertex b = m->verts[pt_map_side_v1(m, cell, k)];
            int32_t dx = (int32_t)b.x - a.x;
            int32_t dy = (int32_t)b.y - a.y;
            double t = atan2((double)dy, (double)dx) * (512.0 / 3.14159265358979323846);
            if (t < 0)
                t += 1024.0;
            uint16_t q = pt_quadrant((int16_t)dx, (int16_t)dy);
            double d = (q << 8 | m->edges[pt_map_side(m, cell, k)->edge].angle) - t;
            RC_CHECK(d > -1.0 - 1e-9 && d < 1e-9, ==, true);
        }
    }

    // collision's frame: a side's own vector, dotted with its direction,
    // lands along it (at least the foot cap) and, across it, within the
    // angle's step and the products' rounding of its line
    for (uint8_t c = 1; c < m->num_cells; c++) {
        const pt_cell *cell = &m->cells[c];
        for (uint8_t k = 0; k <= cell->num; k++) {
            const pt_side *sd = pt_map_side(m, cell, k);
            RC_CHECK(m->halves[half_of(cell, k)].v0, ==, sd->v);
            pt_vertex a = m->verts[sd->v];
            pt_vertex b = m->verts[pt_map_side_v1(m, cell, k)];
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
        for (uint8_t k = 0; k <= cell->num; k++) {
            cx += m->verts[pt_map_side(m, cell, k)->v].x;
            cy += m->verts[pt_map_side(m, cell, k)->v].y;
        }
        uint16_t px = (uint16_t)(cx / (cell->num + 1));
        uint16_t py = (uint16_t)(cy / (cell->num + 1));
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
