// mapdump.c - dump the demo scene's semantic content (cells with their
// heights, surface patterns and rings of sides; edges with their angles and
// wall patterns) for diffing map changes across builds.

#include <stdio.h>

#include "arith.h"
#include "map.h"
#include "palette.h"

static pt_map m;

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);

    for (uint8_t c = 1; c < m.num_cells; c++) {
        const pt_cell *cc = &m.cells[c];
        pt_pattern f = pt_shade(cc->floor);
        pt_pattern ce = pt_shade(cc->ceil);
        printf("cell %d first=%d num-1=%d fz=%d cz=%d floor=%02X%02X ceil=%02X%02X\n", c,
               cc->first, cc->num, cc->floor_z, cc->ceil_z, f.e, f.o, ce.e, ce.o);
        for (uint8_t k = 0; k <= cc->num; k++) {
            const pt_side *sd = pt_map_side(&m, cc, k);
            pt_vertex a = m.verts[sd->v];
            pt_vertex b = m.verts[pt_map_side_v1(&m, cc, k)];
            uint8_t q = pt_quadrant((int16_t)(b.x - a.x), (int16_t)(b.y - a.y));
            printf("  side %d: v%d -> v%d edge %d adj %d twin %d angle %d\n", k, sd->v,
                   pt_map_side_v1(&m, cc, k), sd->edge, sd->adj, sd->twin,
                   q << 8 | m.edges[sd->edge].angle);
        }
    }

    for (uint8_t e = 0; e < m.num_edges; e++) {
        const pt_edge *ed = &m.edges[e];
        pt_pattern w = ed->colour == PT_EDGE_SOFT ? (pt_pattern) {0, 0}
                                                  : pt_shade((uint8_t)(ed->colour - 1));
        printf("edge %d angle %d %s %02X%02X\n", e, ed->angle,
               ed->colour == PT_EDGE_SOFT ? "soft" : "wall", w.e, w.o);
    }
    return 0;
}
