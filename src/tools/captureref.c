// captureref.c - the capture's answers as JSON, for make test's check of the Beeb's
// (beeb/test/capture.6502). Run as portalc_captureref > capture.json.

#include <stdint.h>
#include <stdio.h>

#include "arith.h"
#include "map.h"
#include "render.h"
#include "vcache.h"

// From each cell's centroid (the mean of its ring's vertices, truncated), at every angle: the root
// walk's side and split, as test/capture.6502 sums them.
int main(void)
{
    static pt_map m;
    static pt_vcache vc;
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);

    printf("{\n  \"root\": [");
    for (uint8_t cell = 1; cell < m.num_cells; cell++) {
        const pt_cell *c = &m.cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            sx += m.verts[pt_map_side(&m, s)->v].x;
            sy += m.verts[pt_map_side(&m, s)->v].y;
        }
        uint32_t n = (uint32_t)(pt_map_ring_end(c) - c->first);
        pt_vcache_reset(&vc, (uint16_t)(sx / n), (uint16_t)(sy / n));
        for (unsigned a = 0; a < 256; a++) {
            uint16_t vl = (uint16_t)((uint16_t)((a << 2) - 128u) & 1023u);
            pt_root_side rs = pt_render_root_side(&vc, &m, cell, vl);
            printf(cell == 1 && a == 0 ? "%u,%u" : ",%u,%u", (unsigned)rs.side, (unsigned)rs.wrap);
        }
    }
    printf("],\n  \"cells\": %u\n}\n", (unsigned)m.num_cells);
    return 0;
}
