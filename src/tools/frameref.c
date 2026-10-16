// frameref.c - whole frames' answers as JSON, for make test's check of the Beeb's
// (beeb/test/frame.6502): from each place, at each angle, the frame as pt_render_view draws it
// (the capture, the fill and the sprites, then the unplot), its screen summed by place. Run as
// portalc_frameref > frame.json, over the demo scene. Built as portalc_frame3ref (FRAME_STRESS),
// it's over the stress scene with the walk's depth capped at 3, as walk3ref's.
//
// The sums are made here, a pair a place, as the Beeb's FLETCHER makes them: the screens' bytes
// themselves would be 32K a place.

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

// A byte into a pair of Fletcher sums, mod 255 as FLETCHER makes them: a carry out goes back in.
static void fletcher(uint8_t *s1, uint8_t *s2, uint8_t b)
{
    unsigned t = (unsigned)*s1 + b;
    *s1 = (uint8_t)(t > 255 ? t - 255 : t);
    t = (unsigned)*s2 + *s1;
    *s2 = (uint8_t)(t > 255 ? t - 255 : t);
}

int main(void)
{
    pt_arith_init();
#ifdef FRAME_STRESS
    pt_stress_scene_init(&m);
#else
    pt_scene_init(&m);
#endif
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    static place at[PT_MAX_PLACES];
    static uint8_t sum1[PT_MAX_PLACES], sum2[PT_MAX_PLACES];
    unsigned n = places(&m, at);
    unsigned drawn = 0;
    unsigned most = 0;    // the most in one frame
    for (unsigned p = 0; p < n; p++) {
        pt_vcache_reset(&vc, at[p].x, at[p].y);
        for (unsigned a = 0; a < ANGLES; a++) {
            pt_render_view(&r, (uint8_t)(a * (256 / ANGLES)), at[p].cell);
            drawn += r.num_order;
            if (r.num_order > most)
                most = r.num_order;
            for (unsigned i = 0; i < sizeof r.fb; i++)
                fletcher(&sum1[p], &sum2[p], r.fb[i]);
        }
    }
    printf("{\n  \"screen1\": \"");
    for (unsigned p = 0; p < n; p++)
        printf("%02X", sum1[p]);
    printf("\",\n  \"screen2\": \"");
    for (unsigned p = 0; p < n; p++)
        printf("%02X", sum2[p]);
    printf("\"\n}\n");
    fprintf(stderr, "frameref: %u places, %u frames, %u sprites drawn (%u in one at most), depth cap %d\n",
            n, n * ANGLES, drawn, most, PT_MAX_DEPTH);
    return 0;
}
