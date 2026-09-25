// test_map.c - structural invariants of the demo scene.

#include "map.h"
#include "richc/test.h"

RC_TEST(map, scene_valid)
{
    static pt_map m;
    pt_scene_build(&m);
    RC_CHECK_TRUE(pt_map_validate(&m));

    // 9 rooms plus the reserved outside
    RC_CHECK(m.num_cells, ==, (uint8_t)9);

    // every ring closes: each edge's end is the next edge's start
    for (uint8_t c = 1; c < m.num_cells; c++) {
        const pt_cell *cell = &m.cells[c];
        RC_CHECK(cell->num_edges, >=, (uint8_t)3);
        for (uint8_t k = 0; k < cell->num_edges; k++) {
            uint8_t e = (uint8_t)(cell->first_edge + k);
            uint8_t n = pt_map_ring_next(&m, e);
            RC_CHECK(m.edges[n].cell, ==, c);
        }
    }

    // twins are mutual and run the shared edge in opposite directions
    for (uint8_t e = 0; e < m.num_edges; e++) {
        if (m.edges[e].adj == 0)
            continue;
        uint8_t t = m.edges[e].twin;
        RC_CHECK(m.edges[t].twin, ==, e);
        RC_CHECK(m.edges[t].adj, ==, m.edges[e].cell);
        RC_CHECK(m.edges[t].v0, ==, m.edges[pt_map_ring_next(&m, e)].v0);
    }

    // collision data: unit vectors within rounding, n perpendicular-inward
    for (uint8_t e = 0; e < m.num_edges; e++) {
        const pt_edge *ed = &m.edges[e];
        int32_t lensq = (int32_t)ed->ex * ed->ex + (int32_t)ed->ey * ed->ey;
        RC_CHECK(lensq, >=, (int32_t)(250 * 250));
        RC_CHECK(lensq, <=, (int32_t)(262 * 262));
        RC_CHECK(ed->nx, ==, (int16_t)-ed->ey);
        RC_CHECK(ed->ny, ==, ed->ex);
    }
}
