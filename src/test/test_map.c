// test_map.c - structural invariants of the demo scene.

#include "arith.h"
#include "map.h"
#include "richc/test.h"

RC_TEST(map, scene_valid)
{
    static pt_map m;
    pt_arith_init();
    pt_scene_init(&m);
    RC_CHECK_TRUE(pt_map_validate(&m));

    // 27 cells plus the reserved outside
    RC_CHECK(m.num_cells, ==, (uint8_t)28);

    // every ring stays within its cell's range and closes back on its first
    // edge; the edges partition the table between the cells
    uint16_t total = 0;
    for (uint8_t c = 1; c < m.num_cells; c++) {
        const pt_cell *cell = &m.cells[c];
        RC_CHECK(cell->num_edges, >=, (uint8_t)3);
        RC_CHECK(cell->first_edge, ==, (uint8_t)total);
        total += cell->num_edges;
        for (uint8_t k = 0; k < cell->num_edges; k++) {
            uint8_t e = (uint8_t)(cell->first_edge + k);
            uint8_t n = pt_map_ring_next(cell, e);
            uint8_t want = k + 1 == cell->num_edges ? cell->first_edge : (uint8_t)(e + 1);
            RC_CHECK(n, ==, want);
        }
    }
    RC_CHECK(total, ==, (uint16_t)m.num_edges);

    for (uint8_t c = 1; c < m.num_cells; c++) {
        const pt_cell *cell = &m.cells[c];
        for (uint8_t k = 0; k < cell->num_edges; k++) {
            uint8_t e = (uint8_t)(cell->first_edge + k);
            const pt_edge *ed = &m.edges[e];
            uint8_t v1 = m.edges[pt_map_ring_next(cell, e)].v0;

            // twins are mutual and run the shared edge in opposite
            // directions, with an angle exactly the reverse of ours
            if (ed->adj != 0) {
                const pt_cell *bc = &m.cells[ed->adj];
                uint8_t t = pt_map_twin(&m, e);
                RC_CHECK(t, >=, bc->first_edge);
                RC_CHECK(t, <, (uint8_t)(bc->first_edge + bc->num_edges));
                RC_CHECK(pt_map_twin(&m, t), ==, e);
                RC_CHECK(m.edges[t].adj, ==, c);
                RC_CHECK(m.edges[t].v0, ==, v1);
                RC_CHECK((uint8_t)(m.edges[t].angle + 128), ==, ed->angle);
            }

            // collision data: rotating the edge vector by its own angle
            // must land it along +x within the angle quantisation
            int16_t dx = (int16_t)(m.verts[v1].x - m.verts[ed->v0].x);
            int16_t dy = (int16_t)(m.verts[v1].y - m.verts[ed->v0].y);
            pt_xy f = pt_rot(dx, dy, ed->angle);
            RC_CHECK(f.x, >, (int16_t)0);
            RC_CHECK(f.x / 8, >=, (int16_t)ed->len8);
            // |y| <= len * pi/256 plus rounding
            int32_t tilt = (f.x / 64) + 2;
            RC_CHECK(f.y, <=, (int16_t)tilt);
            RC_CHECK(f.y, >=, (int16_t)-tilt);
        }
    }

    // the point-to-cell query maps every cell's centroid back to that cell,
    // and a point beyond the map to the outside
    for (uint8_t c = 1; c < m.num_cells; c++) {
        const pt_cell *cell = &m.cells[c];
        uint32_t cx = 0;
        uint32_t cy = 0;
        for (uint8_t k = 0; k < cell->num_edges; k++) {
            cx += m.verts[m.edges[cell->first_edge + k].v0].x;
            cy += m.verts[m.edges[cell->first_edge + k].v0].y;
        }
        uint16_t px = (uint16_t)(cx / cell->num_edges);
        uint16_t py = (uint16_t)(cy / cell->num_edges);
        RC_CHECK(pt_map_find_cell(&m, px, py), ==, c);
    }
    RC_CHECK(pt_map_find_cell(&m, 10, 10), ==, (uint8_t)0);
}
