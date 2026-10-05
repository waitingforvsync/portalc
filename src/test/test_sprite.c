// test_sprite.c - sprite art encoding, the demo scene's boulder, and the two
// plotters against a reference worked out a pixel at a time.

#include <stdbool.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "richc/random.h"
#include "richc/test.h"
#include "sprite.h"

// Every pixel decodes back to the code it was encoded from (rows counted up
// from the bottom).
static void check_round_trip(const char *const rows[])
{
    static pt_sprite_art a;
    static const uint8_t pal[7] = {1, 2, 3, 4, 5, 6, 7};
    pt_sprite_encode(&a, rows, pal);
    uint8_t h = 0;
    while (h < PT_SPRITE_MAX_ROWS && rows[h] != NULL)
        h++;
    RC_CHECK(pt_sprite_height(&a), ==, h);
    for (uint8_t r = 0; r < h; r++)
        for (uint8_t c = 0; c < pt_sprite_width(&a); c++)
            RC_CHECK(pt_sprite_code(&a, c, (uint8_t)(h - 1 - r)), ==, (uint8_t)(rows[r][c] - '0'));
}

RC_TEST(sprite, round_trip)
{
    // every code, runs of one row, a colour change on every row, and a
    // column that is one run throughout
    static const char *const small[] = {
        "0123",
        "4567",
        "7777",
        "0000",
        "0707",
        NULL,
    };
    check_round_trip(small);

    // the full height: runs ending on every row
    static const char *tall[PT_SPRITE_MAX_ROWS];
    static char buf[PT_SPRITE_MAX_ROWS][4];
    for (uint8_t r = 0; r < PT_SPRITE_MAX_ROWS; r++) {
        buf[r][0] = (char)('0' + r % 8);
        buf[r][1] = (char)('0' + (r / 4) % 8);
        buf[r][2] = '5';
        buf[r][3] = 0;
        tall[r] = buf[r];
    }
    check_round_trip(tall);
}

RC_TEST(sprite, format)
{
    static const char *const rows[] = {
        "10",
        "12",
        "22",
        NULL,
    };
    static const uint8_t pal[7] = {0};
    static pt_sprite_art a;
    pt_sprite_encode(&a, rows, pal);

    // the header (the crossover: 3 rows ending a run in the one group of 3
    // rows, 320 * 3 / 3, at most 255), the column offsets, then the runs
    // bottom up: column 0 is 2 to row 0 and 1 to row 2; column 1 is 2 to
    // row 1 and 0 to row 2
    static const uint8_t want[] = {2, 3, 255, 5, 7, 0 << 3 | 2, 2 << 3 | 1, 1 << 3 | 2, 2 << 3 | 0};
    for (uint8_t i = 0; i < sizeof want; i++)
        RC_CHECK(a.data[i], ==, want[i]);
}

RC_TEST(sprite, boulder)
{
    static pt_map m;
    pt_scene_init(&m);

    // the pit (cell 1) holds the boulder: 14 x 16, 128 units tall
    const pt_cell *pit = &m.cells[1];
    RC_CHECK(pit->sprite, !=, (uint8_t)0);
    const pt_sprite *sp = &m.sprites[pit->sprite];
    const pt_sprite_art *a = &m.arts[sp->art];
    RC_CHECK(pt_sprite_width(a), ==, (uint8_t)14);
    RC_CHECK(pt_sprite_height(a), ==, (uint8_t)16);
    RC_CHECK(sp->size, ==, (uint16_t)128);
    RC_CHECK_TRUE(pt_map_cell_contains(&m, 1, sp->x, sp->y));

    // byte for byte the boulder Baron assembles for spritescale, crossover
    // and all (from its listing)
    static const uint8_t baron[] = {
        0x0E, 0x10, 0xFA, 0x11, 0x15, 0x1B, 0x22, 0x2A, 0x31, 0x36, 0x3F, 0x43, 0x4A, 0x52, 0x56,
        0x61, 0x66, 0x20, 0x42, 0x5B, 0x78, 0x00, 0x09, 0x10, 0x3A, 0x6B, 0x78, 0x00, 0x09, 0x2A,
        0x43, 0x4A, 0x73, 0x78, 0x09, 0x22, 0x4B, 0x52, 0x5B, 0x62, 0x73, 0x78, 0x09, 0x1A, 0x2B,
        0x32, 0x4B, 0x52, 0x7B, 0x09, 0x12, 0x3B, 0x42, 0x7B, 0x09, 0x12, 0x23, 0x2A, 0x3B, 0x4A,
        0x63, 0x74, 0x7B, 0x09, 0x5B, 0x74, 0x7B, 0x09, 0x1B, 0x22, 0x2B, 0x3A, 0x53, 0x7C, 0x09,
        0x2B, 0x32, 0x4B, 0x54, 0x5B, 0x74, 0x78, 0x11, 0x43, 0x6C, 0x78, 0x00, 0x11, 0x18, 0x33,
        0x3C, 0x43, 0x4C, 0x53, 0x5C, 0x6B, 0x78, 0x00, 0x09, 0x20, 0x5B, 0x78, 0x30, 0x4B, 0x78,
    };
    for (uint8_t i = 0; i < sizeof baron; i++)
        RC_CHECK(a->data[i], ==, baron[i]);

    // no other cell has one
    for (uint8_t c = 2; c < m.num_cells; c++)
        RC_CHECK(m.cells[c].sprite, ==, (uint8_t)0);
}

// ---- the plotters ----

// The reference: every pixel worked out alone. Source column j covers the
// pixels from x + round(j * scale) to x + round((j + 1) * scale), and
// source row k the pairs from y + round(k * scale) up, spritescale's
// rounding (from half a step); a pixel takes the sprite's colour there,
// unless that's transparent or its column's clip hides its pair.
static void ref_plot(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p,
                     const uint8_t clo[PT_SCREEN_WIDTH], const uint8_t chi[PT_SCREEN_WIDTH])
{
    uint8_t w = pt_sprite_width(a);
    uint8_t h = pt_sprite_height(a);
    for (uint8_t j = 0; j < w; j++) {
        int32_t x0 = p.x + (int32_t)((128u + (uint32_t)j * p.scale) >> 8);
        int32_t x1 = p.x + (int32_t)((128u + (uint32_t)(j + 1) * p.scale) >> 8);
        for (int32_t x = x0 < 0 ? 0 : x0; x < x1 && x < PT_SCREEN_WIDTH; x++) {
            for (uint8_t k = 0; k < h; k++) {
                uint8_t code = pt_sprite_code(a, j, k);
                if (code == 0)
                    continue;
                pt_pattern pat = pt_shade(a->pal[code - 1]);
                int32_t y0 = p.y + (int32_t)((128u + (uint32_t)k * p.scale) >> 8);
                int32_t y1 = p.y + (int32_t)((128u + (uint32_t)(k + 1) * p.scale) >> 8);
                if (y0 < clo[x])
                    y0 = clo[x];
                if (y1 > chi[x])
                    y1 = chi[x];
                for (int32_t pr = y0 < 0 ? 0 : y0; pr < y1 && pr < PT_SCREEN_HEIGHT; pr++) {
                    pt_mode5_cur cur = pt_mode5_at((uint8_t)(x >> 2), (uint8_t)(PT_SCREEN_HEIGHT - 1 - pr));
                    uint8_t m = pt_mode5_pixmask((uint8_t)x);
                    int16_t at = (int16_t)(cur.blk + cur.y);
                    fb[at] = (uint8_t)((fb[at] & ~m) | (pat.e & m));
                    fb[at + 1] = (uint8_t)((fb[at + 1] & ~m) | (pat.o & m));
                }
            }
        }
    }
}

static uint32_t rnd(rc_random *g, uint32_t n)
{
    return rc_random_next(g) % n;
}

// A clip of one of several kinds: all showing, one window throughout, two
// sloping bounds (ragged, a few pairs a pixel at most), each column at
// random, or a sloping pair with runs of columns hidden.
static void make_clip(rc_random *g, uint8_t clo[PT_SCREEN_WIDTH], uint8_t chi[PT_SCREEN_WIDTH])
{
    uint32_t kind = rnd(g, 5);
    int32_t lo0 = (int32_t)rnd(g, 65);
    int32_t hi0 = (int32_t)rnd(g, 65);
    int32_t glo = (int32_t)rnd(g, 1025) - 512;    // 256ths of a pair a pixel
    int32_t ghi = (int32_t)rnd(g, 1025) - 512;
    for (int32_t x = 0; x < PT_SCREEN_WIDTH; x++) {
        int32_t lo = 0;
        int32_t hi = PT_SCREEN_HEIGHT;
        if (kind == 1) {
            lo = lo0;
            hi = hi0;
        } else if (kind == 2 || kind == 4) {
            lo = lo0 / 2 + (x * glo >> 8);
            hi = 32 + hi0 / 2 + (x * ghi >> 8);
        } else if (kind == 3) {
            lo = (int32_t)rnd(g, 65);
            hi = (int32_t)rnd(g, 65);
        }
        if (kind == 4 && (x / 7) % 3 == 0)
            hi = lo;
        clo[x] = (uint8_t)(lo < 0 ? 0 : lo > 64 ? 64 : lo);
        chi[x] = (uint8_t)(hi < 0 ? 0 : hi > 64 ? 64 : hi);
    }
}

// Random art: up to the format's limits, runs of random lengths, plenty of
// transparent.
static void make_art(rc_random *g, pt_sprite_art *a)
{
    static char buf[PT_SPRITE_MAX_ROWS][PT_SPRITE_MAX_COLS + 1];
    static const char *rows[PT_SPRITE_MAX_ROWS + 1];
    uint8_t w;
    uint8_t h;
    do {
        w = (uint8_t)(1 + rnd(g, PT_SPRITE_MAX_COLS));
        h = (uint8_t)(1 + rnd(g, PT_SPRITE_MAX_ROWS));
    } while ((uint32_t)w * h > 220);    // under the format's 256 bytes, every run a row
    for (uint8_t c = 0; c < w; c++) {
        char code = '0';
        for (uint8_t r = 0; r < h; r++) {
            if (rnd(g, 4) == 0)
                code = rnd(g, 3) == 0 ? '0' : (char)('1' + rnd(g, 7));
            buf[r][c] = code;
        }
    }
    for (uint8_t r = 0; r < h; r++) {
        buf[r][w] = 0;
        rows[r] = buf[r];
    }
    rows[h] = NULL;
    static const uint8_t pal[7] = {1, 5, 9, 13, 17, 21, 25};
    pt_sprite_encode(a, rows, pal);
}

RC_TEST(sprite, plotters_match_reference)
{
    pt_arith_init();
    static pt_map m;
    pt_scene_init(&m);
    rc_random g = rc_random_make(7);
    static uint8_t bg[PT_FB_BYTES];
    static uint8_t want[PT_FB_BYTES];
    static uint8_t got[PT_FB_BYTES];
    static pt_sprite_art art;
    static uint8_t clo[PT_SCREEN_WIDTH];
    static uint8_t chi[PT_SCREEN_WIDTH];
    uint32_t bad = 0;
    uint32_t wholes = 0;
    for (uint32_t n = 0; n < 6000; n++) {
        const pt_sprite_art *a = &art;
        if (n % 3 == 0)
            a = &m.arts[0];    // the boulder
        else
            make_art(&g, &art);
        for (uint32_t i = 0; i < PT_FB_BYTES; i++)
            bg[i] = (uint8_t)rc_random_next(&g);
        make_clip(&g, clo, chi);

        // scales from a sixteenth to 16x, many under 1x; placed anywhere
        // it might show, often off an edge
        uint16_t scale = (uint16_t)(rnd(&g, 2) ? 16 + rnd(&g, 240) : 256 + rnd(&g, 3841));
        uint32_t span = (uint32_t)pt_sprite_width(a) * scale / 256 + 1;
        uint32_t tall = (uint32_t)pt_sprite_height(a) * scale / 256 + 1;
        pt_sprite_place p = {
            .scale = scale,
            .x = (int16_t)((int32_t)rnd(&g, 128 + span + 8) - (int32_t)span - 4),
            .y = (int16_t)((int32_t)rnd(&g, 64 + tall + 8) - (int32_t)tall - 4),
        };

        memcpy(want, bg, sizeof want);
        ref_plot(want, a, p, clo, chi);

        // the clip as the renderer holds it, over the sprite's own columns
        // (spritescale's plotted width) and no more: the plotters assert
        // they read nowhere else. Whole when every one of them shows all
        // the sprite's pairs on the screen, and then tried both ways
        int32_t pw = (int32_t)((128u + (uint32_t)pt_sprite_width(a) * scale) >> 8);
        int32_t ph = (int32_t)((128u + (uint32_t)pt_sprite_height(a) * scale) >> 8);
        int32_t cx0 = p.x < 0 ? 0 : p.x;
        int32_t cx1 = p.x + pw > PT_SCREEN_WIDTH ? PT_SCREEN_WIDTH : p.x + pw;
        int32_t p0 = p.y < 0 ? 0 : p.y > 64 ? 64 : p.y;
        int32_t p1 = p.y + ph < 0 ? 0 : p.y + ph > 64 ? 64 : p.y + ph;
        if (cx1 < cx0)
            cx1 = cx0 = 0;
        bool whole = true;
        for (int32_t x = cx0; x < cx1; x++)
            whole = whole && clo[x] <= p0 && chi[x] >= p1;
        for (uint8_t pass = 0; pass < (whole ? 2 : 1); pass++) {
            pt_sprite_clip clip = {.lo = &clo[cx0], .hi = &chi[cx0], .x0 = (uint8_t)cx0,
                                   .x1 = (uint8_t)cx1, .whole = pass == 1};
            wholes += pass;

            memcpy(got, bg, sizeof got);
            pt_sprite_plot_engine(got, a, p, clip);
            bad += memcmp(got, want, sizeof got) != 0;

            if (scale < 256) {
                memcpy(got, bg, sizeof got);
                pt_sprite_plot_rebuild(got, a, p, clip);
                bad += memcmp(got, want, sizeof got) != 0;
            }

            memcpy(got, bg, sizeof got);
            pt_sprite_plot(got, a, p, clip);
            bad += memcmp(got, want, sizeof got) != 0;
        }
    }
    RC_CHECK(bad, ==, 0u);
    RC_CHECK(wholes, >, 1000u);
}
