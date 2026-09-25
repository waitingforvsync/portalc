// palette.c - MODE 5 pixel pattern builders.

#include "palette.h"

#include "richc/macros.h"

// In a MODE 5 byte, pixel p (0 leftmost) is bits 7-p (high) and 3-p (low).
static uint8_t pattern_byte(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
    uint8_t hi = (uint8_t)(((p0 >> 1) << 3 | (p1 >> 1) << 2 | (p2 >> 1) << 1 | (p3 >> 1)) << 4);
    uint8_t lo = (uint8_t)((p0 & 1) << 3 | (p1 & 1) << 2 | (p2 & 1) << 1 | (p3 & 1));
    return (uint8_t)(hi | lo);
}

pt_pattern pt_pattern_solid(uint8_t c)
{
    RC_ASSERT(c < 4);

    uint8_t b = pattern_byte(c, c, c, c);
    return (pt_pattern) {.e = b, .o = b};
}

pt_pattern pt_pattern_check(uint8_t a, uint8_t b)
{
    RC_ASSERT(a < 4 && b < 4);

    return (pt_pattern) {
        .e = pattern_byte(a, b, a, b),
        .o = pattern_byte(b, a, b, a),
    };
}

pt_pattern pt_pattern_cell(uint8_t tl, uint8_t tr, uint8_t bl, uint8_t br)
{
    RC_ASSERT(tl < 4 && tr < 4 && bl < 4 && br < 4);

    return (pt_pattern) {
        .e = pattern_byte(tl, tr, tl, tr),
        .o = pattern_byte(bl, br, bl, br),
    };
}

// One row of a fill shape: which of the byte's four pixels take colour b,
// written left to right so a PT_FILL line reads like the tile it draws.
#define PT_FILL(e0, e1, e2, e3, o0, o1, o2, o3)                        \
    {                                                                  \
        (uint8_t)((e0) << 3 | (e1) << 2 | (e2) << 1 | (e3)),           \
        (uint8_t)((o0) << 3 | (o1) << 2 | (o2) << 1 | (o3)),           \
    }

// The b pixels of one scanline's mask, over a background of a.
static uint8_t fill_byte(uint8_t mask, uint8_t a, uint8_t b)
{
    uint8_t px[4];
    for (uint8_t p = 0; p < 4; p++)
        px[p] = (mask >> (3 - p)) & 1 ? b : a;
    return pattern_byte(px[0], px[1], px[2], px[3]);
}

pt_pattern pt_pattern_blend(uint8_t a, uint8_t b, uint8_t k)
{
    RC_ASSERT(a < 4 && b < 4 && k <= 8);
    RC_ASSERT((k & 1) == 0);    // quarter densities only

    // The dither shapes, hardcoded per quarter (even scanline row first).
    // 50% is the single-pixel checkerboard, which MODE 5's double-wide
    // pixels already render chunky; the quarter and three-quarter levels
    // put one dot (or hole) in each 2x2 half of the tile, the second half
    // offset onto the other scanline so nothing stacks into stripes.
    static const struct fill {
        uint8_t e, o;
    } fills[5] = {
        PT_FILL(0, 0, 0, 0,    // solid a
                0, 0, 0, 0),
        PT_FILL(1, 0, 0, 0,    // quarter
                0, 0, 1, 0),
        PT_FILL(1, 0, 1, 0,    // half: the checkerboard
                0, 1, 0, 1),
        PT_FILL(1, 0, 1, 1,    // three quarters
                1, 1, 1, 0),
        PT_FILL(1, 1, 1, 1,    // solid b
                1, 1, 1, 1),
    };

    const struct fill *f = &fills[k >> 1];
    return (pt_pattern) {
        .e = fill_byte(f->e, a, b),
        .o = fill_byte(f->o, a, b),
    };
}

pt_pattern pt_shade(uint8_t i)
{
    RC_ASSERT(i < PT_NUM_SHADES);

    // Every mix of four pixels from the four colours, ordered by summed
    // Rec. 601 luminance (K 0, B 114, R 299, G 587 per thousand).
    static const uint8_t mixes[PT_NUM_SHADES][4] = {
        {0, 0, 0, 0},    //  0 KKKK    0
        {0, 0, 0, 1},    //  1 KKKB  114
        {0, 0, 1, 1},    //  2 KKBB  228
        {0, 0, 0, 2},    //  3 KKKR  299
        {0, 1, 1, 1},    //  4 KBBB  342
        {0, 0, 1, 2},    //  5 KKBR  413
        {1, 1, 1, 1},    //  6 BBBB  456
        {0, 1, 1, 2},    //  7 KBBR  527
        {0, 0, 0, 3},    //  8 KKKG  587
        {0, 0, 2, 2},    //  9 KKRR  598
        {1, 1, 1, 2},    // 10 BBBR  641
        {0, 0, 1, 3},    // 11 KKBG  701
        {0, 1, 2, 2},    // 12 KBRR  712
        {0, 1, 1, 3},    // 13 KBBG  815
        {1, 1, 2, 2},    // 14 BBRR  826
        {0, 0, 2, 3},    // 15 KKRG  886
        {0, 2, 2, 2},    // 16 KRRR  897
        {1, 1, 1, 3},    // 17 BBBG  929
        {0, 1, 2, 3},    // 18 KBRG 1000
        {1, 2, 2, 2},    // 19 BRRR 1011
        {1, 1, 2, 3},    // 20 BBRG 1114
        {0, 0, 3, 3},    // 21 KKGG 1174
        {0, 2, 2, 3},    // 22 KRRG 1185
        {2, 2, 2, 2},    // 23 RRRR 1196
        {0, 1, 3, 3},    // 24 KBGG 1288
        {1, 2, 2, 3},    // 25 BRRG 1299
        {1, 1, 3, 3},    // 26 BBGG 1402
        {0, 2, 3, 3},    // 27 KRGG 1473
        {2, 2, 2, 3},    // 28 RRRG 1484
        {1, 2, 3, 3},    // 29 BRGG 1587
        {0, 3, 3, 3},    // 30 KGGG 1761
        {2, 2, 3, 3},    // 31 RRGG 1772
        {1, 3, 3, 3},    // 32 BGGG 1875
        {2, 3, 3, 3},    // 33 RGGG 2060
        {3, 3, 3, 3},    // 34 GGGG 2348
    };
    const uint8_t *m = mixes[i];

    uint8_t cnt[4] = {0, 0, 0, 0};
    for (uint8_t j = 0; j < 4; j++)
        cnt[m[j]]++;
    uint8_t used[4];
    uint8_t nu = 0;
    for (uint8_t c = 0; c < 4; c++)
        if (cnt[c])
            used[nu++] = c;

    if (nu <= 2) {
        // one or two colours: a straight blend
        uint8_t b = used[nu - 1];
        return pt_pattern_blend(used[0], b, (uint8_t)(cnt[b] * 2));
    }
    if (nu == 3) {
        // 2+1+1: the pair checkers, the singles stagger over the rest so
        // both scanlines carry both hues
        uint8_t p = cnt[used[1]] == 2 ? used[1] : (cnt[used[2]] == 2 ? used[2] : used[0]);
        uint8_t s1 = p == used[0] ? used[1] : used[0];
        uint8_t s2 = p == used[2] ? used[1] : used[2];
        return (pt_pattern) {
            .e = pattern_byte(s1, p, s2, p),
            .o = pattern_byte(p, s2, p, s1),
        };
    }
    // 1+1+1+1: every scanline carries every colour
    return (pt_pattern) {
        .e = pattern_byte(0, 1, 2, 3),
        .o = pattern_byte(2, 3, 0, 1),
    };
}

uint32_t pt_palette_rgba(uint8_t c)
{
    RC_ASSERT(c < 4);

    static const uint32_t rgba[4] = {
        0xFF000000u,    // black
        0xFFFF0000u,    // blue
        0xFF0000FFu,    // red
        0xFF00FF00u,    // green
    };
    return rgba[c];
}
