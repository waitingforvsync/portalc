// vcacheref.c - the vertex cache's answers as JSON, for make test's check of the Beeb's
// (beeb/test/vcache.6502). Run as portalc_vcacheref > vcache.json.

#include <stdint.h>
#include <stdio.h>

#include "arith.h"
#include "map.h"
#include "places.h"
#include "vcache.h"

#define LOCATE_STEP 9         // the located positions round a place: from -31 to 32
#define LOCATE_POSITIONS 8    // each way

static unsigned crossings, halves, halves_crossed, hops2, most;    // the locates' paths

// A locate's paths, counted as pt_vcache_locate takes them: each side crossed, each side the
// position's exactly on (a half turn), and each locate of two hops or more.
static void count(pt_vcache *vc, const pt_map *m, uint8_t cell)
{
    unsigned hops = 0;
    for (uint8_t pass = 0; pass < 8; pass++) {
        const pt_cell *c = &m->cells[cell];
        uint8_t next = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c) && next == 0; s++) {
            uint8_t adj = pt_map_side(m, s)->adj;
            if (adj == 0)
                continue;
            uint16_t a0 = pt_vcache_vert(vc, m, pt_map_side(m, s)->v)->angle;
            uint16_t a1 = pt_vcache_vert(vc, m, pt_map_side_v1(m, c, s))->angle;
            uint16_t d = (uint16_t)((uint16_t)(a1 - a0) & 1023u);
            halves += d == 512;
            halves_crossed += d == 512 && adj < cell;
            if (d > 512 || (d == 512 && adj < cell))
                next = adj;
        }
        if (next == 0)
            break;
        crossings++;
        hops++;
        cell = next;
    }
    hops2 += hops >= 2;
    most = hops > most ? hops : most;
}

// From each vertex in turn: empty the cache, ask for every vertex in the order 7 + 23k mod n,
// then again. The second time round, each vertex's first entry, its entry now, its angle and its
// lr, as test/vcache.6502 sums them.
int main(void)
{
    static pt_map m;
    static pt_vcache vc;
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);

    unsigned n = m.num_verts;
    printf("{\n  \"vcache\": [");
    for (unsigned p = 0; p < n; p++) {
        uint8_t first[PT_MAX_VERTS];
        pt_vcache_reset(&vc, m.verts[p].x, m.verts[p].y);
        for (unsigned k = 0; k < n; k++) {
            uint8_t v = (uint8_t)((7 + 23 * k) % n);
            pt_vcache_vert(&vc, &m, v);
            first[v] = vc.slot[v];
        }
        for (unsigned k = 0; k < n; k++) {
            uint8_t v = (uint8_t)((7 + 23 * k) % n);
            const pt_polar *pp = pt_vcache_vert(&vc, &m, v);
            printf(p == 0 && k == 0 ? "%u,%u,%u,%d" : ",%u,%u,%u,%d", (unsigned)first[v],
                   (unsigned)vc.slot[v], (unsigned)pp->angle, pp->lr);
        }
    }
    printf("],\n  \"verts\": %u,\n", n);

    // from each place's cell, the cell located at each of 8 by 8 positions round it, -31 to 32
    // units off each way in steps of 9, the cache emptied at each
    static place at[PT_MAX_PLACES];
    unsigned np = places(&m, at);
    printf("  \"locate\": [");
    for (unsigned p = 0; p < np; p++) {
        printf(p ? ",\n    \"" : "\n    \"");
        for (unsigned j = 0; j < LOCATE_POSITIONS; j++)
            for (unsigned i = 0; i < LOCATE_POSITIONS; i++) {
                uint16_t x = (uint16_t)(at[p].x + LOCATE_STEP * i - 31);
                uint16_t y = (uint16_t)(at[p].y + LOCATE_STEP * j - 31);
                pt_vcache_reset(&vc, x, y);
                count(&vc, &m, at[p].cell);
                pt_vcache_reset(&vc, x, y);
                printf("%02X", pt_vcache_locate(&vc, &m, at[p].cell));
            }
        printf("\"");
    }
    printf("\n  ]\n}\n");
    fprintf(stderr,
            "vcacheref: %u locates, %u sides crossed, %u half turns (%u crossed), %u of two hops or "
            "more, %u at most\n",
            np * LOCATE_POSITIONS * LOCATE_POSITIONS, crossings, halves, halves_crossed, hops2, most);
    return 0;
}
