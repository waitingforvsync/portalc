// walkref.c - the walk's answers as JSON, for make test's check of the Beeb's
// (beeb/test/walk.6502). Run as portalc_walkref > walk.json, over the demo scene. Built as
// portalc_walk3ref (WALK_STRESS), it's over the stress scene with the walk's depth capped at 3,
// so the cut-off runs (the demo scene nests 10 deep at most), and so do the children with no
// room, which the demo scene never has.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "places.h"
#include "render.h"
#include "vcache.h"

#define ANGLES 32    // from each place: every 8th of the C's 256ths

static pt_map m;
static pt_vcache vc;
static pt_render r;

static void hex(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        printf("%02X", b[i]);
}

int main(void)
{
    pt_arith_init();
#ifdef WALK_STRESS
    pt_stress_scene_init(&m);
#else
    pt_scene_init(&m);
#endif
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // whole captures from each place, at each angle: each frame's counts, records, toggle_end and
    // visits, then T after the place's frames, the junk rows too
    static place at[PT_MAX_PLACES];
    unsigned n = places(&m, at);
    printf("{\n  \"places\": [");
    memset(r.tmem, 0, sizeof r.tmem);
    for (unsigned p = 0; p < n; p++) {
        pt_vcache_reset(&vc, at[p].x, at[p].y);
        printf("%s\n    {\"answers\": \"", p ? "," : "");
        for (unsigned a = 0; a < ANGLES; a++) {
            pt_render_capture(&r, (uint8_t)(a * (256 / ANGLES)), at[p].cell);
            uint8_t head[3] = {r.num_visits, r.num_kept, r.num_recs};
            hex(head, sizeof head);
            for (unsigned i = 0; i < r.num_recs; i++) {
                const pt_rec *q = &r.recs[i];
                uint8_t rec[6] = {q->x0, q->x1, (uint8_t)q->y, (uint8_t)(q->y >> 8),
                                  (uint8_t)q->g, (uint8_t)(q->g >> 8)};
                hex(rec, sizeof rec);
            }
            hex(r.toggle_end, sizeof r.toggle_end);
            for (unsigned i = 1; i < r.num_visits; i++) {
                const pt_visit *vs = &r.visits[i];
                uint8_t v[4] = {vs->cell, vs->side, vs->xr, vs->kept};
                hex(v, sizeof v);
            }
        }
        printf("\", \"t\": \"");
        hex(r.tmem, sizeof r.tmem);
        printf("\"}");
        memset(r.tmem, 0, sizeof r.tmem);
    }
    printf("\n  ]\n}\n");
    fprintf(stderr, "walkref: %u places, %u frames, depth cap %d\n", n, n * ANGLES, PT_MAX_DEPTH);
    return 0;
}
