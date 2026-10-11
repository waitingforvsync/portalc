// vcacheref.c - the vertex cache's answers as JSON, for make test's check of the Beeb's
// (beeb/test/vcache.6502). Run as portalc_vcacheref > vcache.json.

#include <stdint.h>
#include <stdio.h>

#include "arith.h"
#include "map.h"
#include "vcache.h"

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
    printf("],\n  \"verts\": %u\n}\n", n);
    return 0;
}
