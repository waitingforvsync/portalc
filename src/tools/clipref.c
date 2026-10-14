// clipref.c - the clips' answers as JSON, for make test's check of the Beeb's
// (beeb/test/clip.6502). Run as portalc_clipref > clip.json.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "render.h"
#include "vcache.h"

#define LINES 256                    // made-up lines
#define CASES 16                     // clips of each, against made-up bounds
#define LINES_HEIGHT (17 * 256 + 31) // their height's log (captureref's)

static pt_map m;
static pt_vcache vc;
static pt_render r;

// The made-up values, each stepped on before it's used, as test/clip.6502 steps them; and a
// 16-bit Galois LFSR for the cases' choices, so none of them falls in step with the line or case.
static uint8_t gi, gj, c0acc, s1, b1, s2, b2, even = 0x5A, odd = 0xC3;
static uint16_t random = 0xACE1;

static void hex(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        printf("%02X", b[i]);
}

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // made-up lines from make_line, on a grid of the ends' depths' logs (from -512 in steps of
    // 32, as captureref's) and of columns; each clipped CASES times against made-up bounds over
    // a span about its columns: plain, floor, ceiling and the columns closed, each with the bounds'
    // extents theirs over the span or either or both of them the screen's edges. Each case's
    // answers, then T after every two lines, the junk rows too
    printf("{\n  \"cases\": [");
    static uint8_t tsums[LINES / 2][sizeof r.tmem];
    memset(r.tmem, 0, sizeof r.tmem);
    for (unsigned line = 0; line < LINES; line++) {
        gi = (uint8_t)(gi + 5);
        gj = (uint8_t)(gj + 11);
        c0acc = (uint8_t)((c0acc + 13) & 63);
        pt_walk w = {.c0 = c0acc, .c1 = (uint8_t)(c0acc + 1 + (gj >> 2))};
        pt_ends e = {.logdepth0 = (int16_t)(-512 + 32 * (int)gi),
                     .logdepth1 = (int16_t)(-512 + 32 * (int)gj),
                     .logcolumns = (int16_t)pt_log2_8((uint8_t)(w.c1 - w.c0))};
        pt_height h = {.below = (uint8_t)(gj & 1), .l = LINES_HEIGHT};
        printf("%s\n    \"", line ? "," : "");
        for (unsigned k = 0; k < CASES; k++) {
            s1 = (uint8_t)(s1 + 23);
            b1 = (uint8_t)(b1 + 41);
            s2 = (uint8_t)(s2 + 67);
            b2 = (uint8_t)(b2 + 19);
            random = (uint16_t)((random >> 1) ^ ((random & 1) ? 0xB400 : 0));
            even = (uint8_t)(even + 29);
            odd = (uint8_t)(odd + 53);

            // the line and its span; and most cases' line shifted by whole rows so that a row
            // lands on the screen's edge, as cut lines' do: the one at its end (1), at its
            // start (2 and 3), or, most often, at its last column in the span (4 to 7), on the
            // edge it runs towards or from, if that's on its own side
            pt_line L = pt_render_line(&w, &e, h);
            int xa = L.x0 + (int)(random & 15) - 8;
            xa = xa < 0 ? 0 : xa > 127 ? 127 : xa;
            int xb = L.x1 + (int)((random >> 4) & 15) - 8;
            xb = xb < xa + 1 ? xa + 1 : xb > 128 ? 128 : xb;
            static const uint8_t touches[8] = {0, 1, 2, 2, 3, 3, 3, 3};
            unsigned touch = touches[(random >> 8) & 7];
            if (L.x0 < L.x1 && touch != 0) {
                bool up = (L.g >> 15) != 0;
                int row = L.rl;
                if (touch == 2)
                    row = L.y >> 8;
                if (touch == 3) {
                    int last = (L.x1 < xb ? L.x1 : xb) - 1;
                    last = last < L.x0 ? L.x0 : last;
                    uint16_t d = pt_mulg_u16((uint16_t)(L.g & 0x7FFF), (uint8_t)(last - L.x0));
                    row = (uint16_t)(up ? L.y - d : L.y + d) >> 8;
                }
                bool top = (touch == 2) != up;
                if (top != (L.below != 0)) {    // only onto the edge on its own side
                    int d = top ? -row : 64 - row;
                    L.y = (uint16_t)(L.y + d * 256);
                    L.rl = (uint8_t)(L.rl + d);
                }
            }
            unsigned kind = k & 3;
            unsigned edges = (k >> 2) & 3;
            bool none = ((random >> 11) & 7) == 7;
            pt_pattern col = none ? (pt_pattern) {0, 0} : (pt_pattern) {even, odd};
            L.ce = col.e;
            L.co = col.o;

            uint16_t tacc = (uint16_t)(b1 << 8);
            uint16_t bacc = (uint16_t)(b2 << 8);
            for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
                tacc = (uint16_t)(tacc + s1);
                bacc = (uint16_t)(bacc + s2);
                uint8_t t = (uint8_t)((tacc >> 8) & 63);
                unsigned b = t + ((bacc >> 8) & 63);
                r.top[c] = t;
                r.bot[c] = (uint8_t)(b > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : b);
            }
            // a bound that's the screen's edge, made misleading: a clip mustn't read it
            if (edges & 1)
                memset(r.top, 63, sizeof r.top);
            if (edges & 2)
                memset(r.bot, 0, sizeof r.bot);
            uint8_t tmax = 0;
            uint8_t bmin = PT_SCREEN_HEIGHT;
            for (int c = xa; c < xb; c++) {
                if (!(edges & 1) && r.top[c] > tmax)
                    tmax = r.top[c];
                if (!(edges & 2) && r.bot[c] < bmin)
                    bmin = r.bot[c];
            }

            memset(r.toggle_end, 1, sizeof r.toggle_end);
            r.num_recs = 0;
            uint8_t ext = 0;
            bool room = false;
            if (kind == 0)
                pt_render_clip_plain(&r, &L, (uint8_t)xa, (uint8_t)xb, tmax, bmin);
            else if (kind == 1)
                ext = pt_render_clip_floor(&r, &L, (uint8_t)xa, (uint8_t)xb, tmax, bmin, &room);
            else if (kind == 2)
                ext = pt_render_clip_ceil(&r, &L, (uint8_t)xa, (uint8_t)xb, tmax, bmin, &room);
            else
                pt_render_close_columns(&r, (uint8_t)xa, (uint8_t)xb, col,
                                        bmin == PT_SCREEN_HEIGHT);

            uint8_t head[3] = {ext, room ? 0x80 : 0, r.num_recs};
            hex(head, sizeof head);
            for (unsigned i = 0; i < r.num_recs; i++) {
                const pt_rec *q = &r.recs[i];
                uint8_t rec[6] = {q->x0, q->x1, (uint8_t)q->y, (uint8_t)(q->y >> 8),
                                  (uint8_t)q->g, (uint8_t)(q->g >> 8)};
                hex(rec, sizeof rec);
            }
            hex(r.toggle_end, sizeof r.toggle_end);
            hex(r.top, sizeof r.top);
            hex(r.bot, sizeof r.bot);
        }
        printf("\"");
        if (line & 1) {
            memcpy(tsums[line / 2], r.tmem, sizeof r.tmem);
            memset(r.tmem, 0, sizeof r.tmem);
        }
    }
    printf("\n  ],\n  \"t\": [");
    for (unsigned g = 0; g < LINES / 2; g++) {
        printf("%s\n    \"", g ? "," : "");
        hex(tsums[g], sizeof r.tmem);
        printf("\"");
    }
    printf("\n  ]\n}\n");
    return 0;
}
