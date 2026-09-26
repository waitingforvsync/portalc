// mapdump.c - dump the demo scene's semantic content (cells with their
// heights and surface patterns, edges with their twins, wall patterns and
// collision data) for diffing map changes across builds.

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
        printf("cell %d fe=%d ne=%d fz=%d cz=%d floor=%02X%02X ceil=%02X%02X\n", c,
               cc->first_edge, cc->num_edges, cc->floor_z, cc->ceil_z, f.e, f.o, ce.e, ce.o);
        for (uint8_t k = 0; k < cc->num_edges; k++) {
            uint8_t e = (uint8_t)(cc->first_edge + k);
            const pt_edge *ed = &m.edges[e];
            int soft = ed->colour == PT_EDGE_SOFT;
            pt_pattern w = soft ? (pt_pattern) {0, 0} : pt_shade((uint8_t)(ed->colour - 1));
            int twin = ed->adj ? pt_map_twin(&m, cc, e) : -1;
            printf("  edge %d v0=%d adj=%d twin=%d soft=%d wall=%02X%02X angle=%d len8=%d\n",
                   e, ed->v0, ed->adj, twin, soft, w.e, w.o, ed->angle, ed->len8);
        }
    }
    return 0;
}
