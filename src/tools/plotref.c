// plotref.c - the plotters' answers as JSON, for make test's check of the Beeb's
// (beeb/test/plot.6502). Run as portalc_plotref > plot.json.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "plot.h"
#include "render.h"
#include "vcache.h"

#define RUNS 128                                       // runs at each gradient
#define ROW_LAST ((PT_SCREEN_HEIGHT << 8) + 255)       // the junk row's last 256th, u8.8

// The gradients' magnitudes: flat; shallow, either side of 64 (one carry a byte at most, or the
// tree), 128 and 255; steep, either side of 256, on to the steepest. Each is plotted falling,
// then (bar flat) rising. test/plot.6502 has the same.
static const uint16_t mags[] = {0,    1,    2,    3,    7,    16,   31,    32,    37,   63,
                                64,   65,   85,   127,  128,  129,  170,   200,   255,  256,
                                257,  300,  384,  511,  512,  640,  1000,  1023,  1024, 1500,
                                2048, 3000, 4096, 6000, 8192, 12000, 16384, 0x7FFF};
#define MAGS (sizeof mags / sizeof mags[0])

// The runs' lengths, before they're cut to the screen and to 64 rows' fall.
static const uint8_t lengths[16] = {1, 2, 3, 4, 5, 6, 7, 8, 11, 16, 23, 32, 47, 64, 97, 128};

static pt_map m;
static pt_vcache vc;
static pt_render r;

// The made-up values, each stepped on before it's used, as test/plot.6502 steps them.
static uint8_t fraction, row, base, even = 0x5A, odd = 0xC3;

static void hex(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        printf("%02X", b[i]);
}

static pt_pattern next_colour(void)
{
    even = (uint8_t)(even + 29);
    odd = (uint8_t)(odd + 53);
    return (pt_pattern) {.e = even, .o = odd};
}

static uint8_t next_row(uint8_t step)
{
    row = (uint8_t)(row + step);
    if (row >= 65)
        row = (uint8_t)(row - 65);
    return row;
}

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // at each gradient, made-up runs into an empty T: from each column in turn, a length from
    // the list cut to the screen and to a fall of 64 rows, a row and fraction made up and then
    // kept to the screen, a colour made up but every 32nd none, and toggle_end made up: each
    // run's count, record and toggle_end; then T after them all, the junk rows too, and after
    // the unplot clears them from their records
    printf("{\n  \"runs\": [");
    for (unsigned k = 0; k < 2 * MAGS - 1; k++) {
        uint16_t mag = mags[k < MAGS ? k : k - MAGS + 1];
        uint16_t g = (uint16_t)(k < MAGS ? mag : mag | 0x8000u);
        unsigned most = mag == 0 ? 128 : 16384u / mag + 1;
        memset(r.tmem, 0, sizeof r.tmem);
        r.num_recs = 0;
        printf("%s\n    {\"runs\": \"", k ? "," : "");
        for (unsigned j = 0; j < RUNS; j++) {
            fraction = (uint8_t)(fraction + 37);
            next_row(5);
            base = (uint8_t)(base + 13);
            pt_line L = {.g = g};
            if ((j & 31) != 31) {
                pt_pattern c = next_colour();
                L.ce = c.e;
                L.co = c.o;
            }
            uint8_t x0 = (uint8_t)(j & 127);
            unsigned len = lengths[j & 15];
            if (len > most)
                len = most;
            if (len > 128u - x0)
                len = 128u - x0;
            unsigned span = (len - 1) * mag;
            unsigned y = (unsigned)row << 8 | fraction;
            if (pt_g_isneg(g)) {
                if (y < span)
                    y = span;
            } else if (y > ROW_LAST - span) {
                y = ROW_LAST - span;
            }
            for (unsigned i = 0; i < sizeof r.toggle_end; i++)
                r.toggle_end[i] = (uint8_t)(((base + 5 * i) & 63) + 1);
            pt_render_plot_run(&r, &L, x0, (uint8_t)(x0 + len), (uint16_t)y);
            const pt_rec *q = &r.recs[r.num_recs - 1];
            uint8_t rec[7] = {r.num_recs, q->x0, q->x1, (uint8_t)q->y, (uint8_t)(q->y >> 8),
                              (uint8_t)q->g, (uint8_t)(q->g >> 8)};
            hex(rec, sizeof rec);
            hex(r.toggle_end, sizeof r.toggle_end);
        }
        printf("\", \"t\": \"");
        hex(r.tmem, sizeof r.tmem);
        pt_render_unplot(&r);
        printf("\", \"unplot\": \"");
        hex(r.tmem, sizeof r.tmem);
        printf("\"}");
    }

    // flat runs from each start column to every end, each on a row made up: T after each four
    // start columns'
    printf("\n  ],\n  \"flat\": [");
    for (unsigned x0 = 0; x0 < 128; x0++) {
        if ((x0 & 3) == 0)
            memset(r.tmem, 0, sizeof r.tmem);
        for (unsigned x1 = x0 + 1; x1 <= 128; x1++) {
            uint8_t y = next_row(7);
            pt_plot_flat(r.tb, (uint8_t)x0, (uint8_t)x1, y, next_colour());
        }
        if ((x0 & 3) == 3) {
            printf("%s\n    \"", x0 > 3 ? "," : "");
            hex(r.tmem, sizeof r.tmem);
            printf("\"");
        }
    }

    printf("\n  ]\n}\n");
    return 0;
}
