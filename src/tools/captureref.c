// captureref.c - the capture's answers as JSON, for make test's check of the Beeb's
// (beeb/test/capture.6502). Run as portalc_captureref > capture.json.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "render.h"
#include "vcache.h"

static pt_map m;
static pt_vcache vc;
static pt_render r;

static void comma(int *first)
{
    if (!*first)
        printf(",");
    *first = 0;
}

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // from each cell's centroid (the mean of its ring's vertices, truncated), at every angle: the
    // frame's setup and root walk, as test/capture.6502 sums them
    printf("{\n  \"frame\": [");
    int first = 1;
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
            pt_walk w = pt_render_root(&r, (uint8_t)a, cell);
            const pt_visit *vs = &r.visits[w.vi];
            comma(&first);
            printf("%u,%u,%u,%u,%u,%d,%d,%u,%u,", w.vi, w.x, w.tmax, w.bmin, w.wrap, w.u0, w.u1,
                   w.c0, w.c1);
            printf("%u,%u,%u,%u,%u,%d,%u,%d,", vs->cell, vs->side, vs->xr, vs->kept,
                   vs->hc.below, vs->hc.l, vs->hf.below, vs->hf.l);
            printf("%u,%u,%u,%u", r.eye_z, r.num_visits, r.num_kept, r.num_recs);
            for (unsigned i = 0; i < sizeof r.toggle_end; i++)
                printf(",%u", r.toggle_end[i]);
            // the visit cache emptied, as a frame does after its walk
            memset(r.cell_visit, 0, sizeof r.cell_visit);
        }
    }
    printf("],\n  \"cells\": %u,\n", (unsigned)m.num_cells);

    // every height from every eye height: whether it's below, and the log
    printf("  \"height\": [");
    first = 1;
    for (unsigned eye = 0; eye < 256; eye++)
        for (unsigned z = 0; z < 256; z++) {
            pt_height h = pt_render_height((uint8_t)z, (uint8_t)eye);
            comma(&first);
            printf("%u,%d", h.below, h.l);
        }
    printf("]\n}\n");
    return 0;
}
