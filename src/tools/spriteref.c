// spriteref.c - the sprite plotters' answers as JSON, for make test's check of the Beeb's
// (beeb/test/sprite.6502); and with --arts, the arts they plot as Baron source, for
// beeb/test/spritedata.6502 (make map).
//
//   portalc_spriteref > sprite.json
//   portalc_spriteref --arts > spritedata.6502

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "palette.h"
#include "sprite.h"

#define ARTS 8       // the demo scene's art, the stress scene's, and six made up
#define CASES 1024   // made-up sprites, plotted into screen A and B by turns

static pt_map m;
static pt_sprite_art arts[ARTS];
static uint8_t fb[PT_FB_BYTES];
static uint8_t lo[PT_SCREEN_WIDTH], hi[PT_SCREEN_WIDTH];

// A 32-bit Galois LFSR (taps &80200003), stepped a byte on (eight steps) before each made-up
// value, as test/sprite.6502 steps it: a case draws some 140, and a 16-bit one's 65,535 come
// round in under 500 cases.
static uint32_t random = 0xACE1;

static uint8_t step(void)
{
    for (unsigned i = 0; i < 8; i++)
        random = (random >> 1) ^ ((random & 1) ? 0x80200003u : 0);
    return (uint8_t)random;
}

// 0 to 64: 0 to 127 made up, less 64 past 64.
static uint8_t up_to_64(void)
{
    uint8_t v = step() & 127;
    return v > 64 ? (uint8_t)(v - 64) : v;
}

// The made-up arts' own generator (the Beeb only sees their bytes): a 32-bit xorshift.
static uint32_t art_seed = 0x2545F491u;

static uint32_t art_next(void)
{
    art_seed ^= art_seed << 13;
    art_seed ^= art_seed >> 17;
    art_seed ^= art_seed << 5;
    return art_seed;
}

// The arts: the demo scene's and the stress scene's first, then six made up, from a column to 32
// and a row to 32, each column's colour changing an eighth of the rows down it, transparent a
// third of the time, in seven made-up shades; but the second made up by hand for the stretch
// engine's swallowing: a band of runs a row deep up every column, which shrink away under 1x,
// and its crossover forced low, so the engine draws it there.
static void make_arts(void)
{
    pt_scene_init(&m);
    arts[0] = m.arts[0];
    pt_stress_scene_init(&m);
    arts[1] = m.arts[0];
    static const uint8_t sizes[ARTS - 2][2] = {{1, 32}, {3, 5},  {7, 17},
                                               {13, 1}, {22, 9}, {32, 24}};
    for (unsigned a = 2; a < ARTS; a++) {
        uint8_t w = sizes[a - 2][0], h = sizes[a - 2][1];
        static char text[PT_SPRITE_MAX_ROWS][PT_SPRITE_MAX_COLS + 1];
        const char *rows[PT_SPRITE_MAX_ROWS + 1];
        if (a == 3) {
            w = 4;
            h = 24;
            for (uint8_t r = 0; r < h; r++)
                for (uint8_t c = 0; c < w; c++)
                    text[r][c] = r >= 4 && r < 20 ? "2031"[(r + c) % 4] : '1';
        }
        for (uint8_t c = 0; c < w && a != 3; c++) {
            char code = '0';
            for (uint8_t r = 0; r < h; r++) {
                if (r == 0 || art_next() % 8 == 0)
                    code = art_next() % 3 == 0 ? '0' : (char)('1' + art_next() % 7);
                text[r][c] = code;
            }
        }
        for (uint8_t r = 0; r < h; r++) {
            text[r][w] = 0;
            rows[r] = text[r];
        }
        rows[h] = NULL;
        uint8_t pal[7];
        for (unsigned i = 0; i < 7; i++)
            pal[i] = (uint8_t)(art_next() % PT_NUM_SHADES);
        pt_sprite_encode(&arts[a], rows, pal);
        if (a == 3)
            arts[a].data[2] = 40;
    }
}

// An art's encoded length: its last column's runs end on its top row.
static unsigned art_len(const pt_sprite_art *a)
{
    uint8_t w = pt_sprite_width(a);
    uint8_t h = pt_sprite_height(a);
    unsigned n = a->data[3 + w - 1];
    while ((a->data[n++] >> 3) != h - 1u)
        ;
    return n;
}

static void list(const char *name, const uint8_t *v, unsigned n)
{
    printf("%s = {", name);
    for (unsigned i = 0; i < n; i++)
        printf(i == 0 ? "%u" : ", %u", v[i]);
    printf("}\n");
}

// The arts as mapdata.6502 has them: each one's patterns by art * 8 + colour code, its data by
// art_lo and art_hi.
static void print_arts(void)
{
    printf("\\\\ spritedata.6502 - the sprite check's arts, as mapdata.6502 has its own: the demo "
           "scene's,\n"
           "\\\\ the stress scene's and six made up. Made by portalc_spriteref --arts (make map): "
           "don't edit it.\n\n");
    uint8_t e[8 * ARTS], o[8 * ARTS];
    for (unsigned i = 0; i < 8 * ARTS; i++) {
        e[i] = arts[i / 8].pats[i % 8].e;
        o[i] = arts[i / 8].pats[i % 8].o;
    }
    list("map_art_e", e, sizeof e);
    list("map_art_o", o, sizeof o);
    char name[16];
    for (unsigned a = 0; a < ARTS; a++) {
        snprintf(name, sizeof name, "map_art%u", a);
        list(name, arts[a].data, art_len(&arts[a]));
    }
    printf("\nMAP_ARTS = LEN(map_art_e) DIV 8\n\n");
    printf("    FITPAGE LEN(map_art_e)\n.art_pate        EQUB map_art_e\n");
    printf("    FITPAGE LEN(map_art_o)\n.art_pato        EQUB map_art_o\n");
    printf("    FITPAGE MAP_ARTS\n.art_lo          EQUB LO({");
    for (unsigned a = 0; a < ARTS; a++)
        printf(a == 0 ? "art%u" : ", art%u", a);
    printf("})\n    FITPAGE MAP_ARTS\n.art_hi          EQUB HI({");
    for (unsigned a = 0; a < ARTS; a++)
        printf(a == 0 ? "art%u" : ", art%u", a);
    printf("})\n");
    for (unsigned a = 0; a < ARTS; a++)
        printf("    FITPAGE LEN(map_art%u)\n.art%-12u EQUB map_art%u\n", a, a, a);
}

// A case's clip, for every column: whole (and said to be), full, even, sloping, noisy, gappy,
// noisy and gappy, or ragged; the even and the ragged inside small sprites, bottoms 0 to 16 and
// tops 16 to 48 (the even's top a closing at every slot at once).
static void make_clip(uint8_t kind)
{
    switch (kind) {
    case 0:
    case 1:
        memset(lo, 0, sizeof lo);
        memset(hi, PT_SCREEN_HEIGHT, sizeof hi);
        break;
    case 2: {
        uint8_t b = up_to_64() >> 2;
        uint8_t t = (uint8_t)((up_to_64() >> 1) + 16);
        memset(lo, b, sizeof lo);
        memset(hi, t, sizeof hi);
        break;
    }
    case 3: {
        uint8_t b = up_to_64();
        int8_t slope = (int8_t)step();
        uint8_t span = up_to_64();
        int16_t acc = (int16_t)(b * 8);
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            int16_t v = (int16_t)(acc >> 3);
            lo[c] = pt_u8(v < 0 ? 0 : v > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : v);
            unsigned t = lo[c] + span;
            hi[c] = t > PT_SCREEN_HEIGHT ? (uint8_t)PT_SCREEN_HEIGHT : (uint8_t)t;
            acc = (int16_t)(acc + slope);
        }
        break;
    }
    case 4:
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            lo[c] = up_to_64();
            hi[c] = up_to_64();
        }
        break;
    case 5:
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            bool gap = (step() & 3) == 0;
            lo[c] = 0;
            hi[c] = gap ? (uint8_t)0 : (uint8_t)PT_SCREEN_HEIGHT;
        }
        break;
    case 6:
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            if ((step() & 3) == 0) {
                lo[c] = 0;
                hi[c] = 0;
            } else {
                lo[c] = up_to_64();
                hi[c] = up_to_64();
            }
        }
        break;
    default:
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            lo[c] = up_to_64() >> 2;
            hi[c] = (uint8_t)((up_to_64() >> 1) + 16);
        }
        break;
    }
}

int main(int argc, char **argv)
{
    pt_arith_init();
    make_arts();
    if (argc == 2 && strcmp(argv[1], "--arts") == 0) {
        print_arts();
        return 0;
    }

    // made-up sprites: an art; a scale, 256 to 511 shifted up 0 to 7 then down 4 (a sixteenth
    // to 16x), and a left column and a bottom pair, signed bytes halved, the column 16 on, but
    // over 4x one of them far off the screen, the column a signed byte times 4, 16 on, or the
    // pair a signed byte, 256 on (past 255 for its top byte); or, every other, under 1x for the
    // rebuild and the engine's swallowing, a scale 32 to 222 (an eighth of them 1x itself, for
    // the engine's four columns), a column -16 to 142 (a quarter of them at the right edge, 128
    // less 0 to 15) and a pair -16 to 47; a clip, held from the sprite's first column on the
    // screen less 0 to 15; and the screen made up from a byte, each one its offset's low byte
    // EOR its high EOR that byte: the screen after each
    printf("{\n  \"screens\": [");
    for (unsigned k = 0; k < CASES; k++) {
        uint8_t art = step() & 7;
        uint16_t scale;
        int16_t x, y;
        if ((k & 1) == 0) {
            uint8_t a = step() & 127;
            scale = (uint16_t)(32 + a + (step() & 63));
            if (a < 16)
                scale = 256;
            uint8_t b = step() & 127;
            uint8_t c = step() & 31;
            x = (int16_t)((k & 6) == 4 ? 128 - (c & 15) : b + c - 16);
            y = (int16_t)((step() & 63) - 16);
        } else {
            uint8_t mantissa = step();
            uint8_t shift = step() & 7;
            scale = (uint16_t)(((256u + mantissa) << shift) >> 4);
            uint8_t far = shift >= 6 ? (uint8_t)(1 + (step() & 1)) : 0;    // 1 column, 2 pair
            int8_t v = (int8_t)step();
            x = (int16_t)(far == 1 ? v * 4 + 16 : (v >> 1) + 16);    // (CMP #&80 : ROR A, + 16)
            v = (int8_t)step();
            y = (int16_t)(far == 2 ? v + 256 : v >> 1);              // (CMP #&80 : ROR A)
        }
        uint8_t kind = step() & 7;
        make_clip(kind);
        uint8_t first = pt_u8(x < 0 ? 0 : x > PT_SCREEN_WIDTH - 1 ? PT_SCREEN_WIDTH - 1 : x);
        uint8_t back = step() & 15;
        uint8_t x0 = back > first ? (uint8_t)0 : (uint8_t)(first - back);
        uint8_t seed = step();
        for (unsigned i = 0; i < sizeof fb; i++)
            fb[i] = (uint8_t)(i ^ (i >> 8) ^ seed);
        pt_sprite_plot(fb, &arts[art], (pt_sprite_place) {.scale = scale, .x = x, .y = y},
                       (pt_sprite_clip) {.lo = lo + x0, .hi = hi + x0, .x0 = x0,
                                         .x1 = PT_SCREEN_WIDTH, .whole = kind == 0});
        printf("%s\n    \"", k ? "," : "");
        for (unsigned i = 0; i < sizeof fb; i++)
            printf("%02X", fb[i]);
        printf("\"");
    }
    printf("\n  ]\n}\n");
    return 0;
}
