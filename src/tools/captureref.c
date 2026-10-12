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

// The camera's places, cell by cell: the centroid (the mean of its ring's vertices, truncated),
// then 7/8 of the way from it to each vertex in turn, near the corners, where the view's right
// edge can land on the cell's last side. test/capture.6502 makes the same.
typedef struct place {
    uint8_t cell;
    uint16_t x, y;
} place;

static unsigned places(place *out)
{
    unsigned n = 0;
    for (uint8_t cell = 1; cell < m.num_cells; cell++) {
        const pt_cell *c = &m.cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            sx += m.verts[pt_map_side(&m, s)->v].x;
            sy += m.verts[pt_map_side(&m, s)->v].y;
        }
        uint32_t k = (uint32_t)(pt_map_ring_end(c) - c->first);
        uint32_t cx = sx / k;
        uint32_t cy = sy / k;
        out[n++] = (place) {cell, (uint16_t)cx, (uint16_t)cy};
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            pt_vertex v = m.verts[pt_map_side(&m, s)->v];
            out[n++] = (place) {cell, (uint16_t)((cx + 7u * v.x) / 8),
                                (uint16_t)((cy + 7u * v.y) / 8)};
        }
    }
    for (unsigned i = 0; i < n; i++)
        RC_ASSERT(pt_map_cell_contains(&m, out[i].cell, out[i].x, out[i].y));
    return n;
}

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // from each place, at every angle: the frame's setup and root walk, then the eye's cell
    // walked as the walk walks it, from the root side until a side ends at or past the portal's
    // right column, with the ends of each side that has columns: as test/capture.6502 sums them
    static place at[PT_MAX_CELLS + PT_MAX_SIDES];
    unsigned n = places(at);
    unsigned last = 0;    // frames whose walk reaches the eye's cell's last side, a turn on
    printf("{\n  \"frames\": [");
    int first = 1;
    for (unsigned p = 0; p < n; p++) {
        pt_vcache_reset(&vc, at[p].x, at[p].y);
        for (unsigned a = 0; a < 256; a++) {
            pt_walk w = pt_render_root(&r, (uint8_t)a, at[p].cell);
            const pt_visit *vs = &r.visits[w.vi];
            comma(&first);
            printf("[%u,%u,%u,%u,%u,%d,%d,%u,%u,", w.vi, w.x, w.tmax, w.bmin, w.wrap, w.u0, w.u1,
                   w.c0, w.c1);
            printf("%u,%u,%u,%u,%u,%d,%u,%d,", vs->cell, vs->side, vs->xr, vs->kept,
                   vs->hc.below, vs->hc.l, vs->hf.below, vs->hf.l);
            printf("%u,%u,%u,%u", r.eye_z, r.num_visits, r.num_kept, r.num_recs);
            for (unsigned i = 0; i < sizeof r.toggle_end; i++)
                printf(",%u", r.toggle_end[i]);
            unsigned turned = 0;
            for (;;) {
                printf(",%u,%d,%d,%u,%u", vs->side, w.u0, w.u1, w.c0, w.c1);
                if (w.c0 < w.c1) {
                    pt_ends e = pt_render_ends(&r, &w);
                    printf(",%d,%d,%d", e.logdepth0, e.logdepth1, e.logcolumns);
                }
                turned |= w.u1 >= (int16_t)w.wrap;
                if (w.c1 >= vs->xr)
                    break;
                pt_render_side_next(&r, &w);
            }
            last += turned;
            printf("]");
            // the visit cache emptied, as a frame does after its walk
            memset(r.cell_visit, 0, sizeof r.cell_visit);
        }
    }
    printf("],\n  \"places\": %u,\n", n);
    fprintf(stderr, "captureref: %u places, %u of %u frames reach the eye's cell's last side\n", n,
            last, n * 256);

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
