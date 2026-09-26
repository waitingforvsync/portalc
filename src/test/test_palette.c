// test_palette.c - the hardcoded 35-shade catalogue against its rules.

#include "palette.h"
#include "richc/test.h"

// The colour of pixel p (0 leftmost) in a MODE 5 pattern byte.
static uint8_t pixel(uint8_t byte, uint8_t p)
{
    return (uint8_t)((((byte >> (7 - p)) & 1) << 1) | ((byte >> (3 - p)) & 1));
}

RC_TEST(palette, px4_roundtrip)
{
    for (uint32_t c = 0; c < 4; c++)
        for (uint8_t p = 0; p < 4; p++) {
            uint8_t px[4] = {0, 0, 0, 0};
            px[p] = (uint8_t)c;
            uint8_t b = PT_PX4(px[0], px[1], px[2], px[3]);
            for (uint8_t q = 0; q < 4; q++)
                RC_CHECK(pixel(b, q), ==, q == p ? (uint8_t)c : (uint8_t)0);
        }
}

RC_TEST(palette, shades)
{
    // Rec. 601 luminance of the hardware colours, per thousand; the colour
    // indices are already in brightness order. Every one of the 35 mixes has
    // a distinct sum, so deriving the expected catalogue by enumerating the
    // multisets in luminance order pins the ordering, the content of each
    // tile, and (for the checker classes) the exact tile.
    static const uint16_t lum[4] = {0, 114, 299, 587};

    uint8_t mix[35][4];
    uint16_t sum[35];
    uint8_t n = 0;
    for (uint8_t a = 0; a < 4; a++)
        for (uint8_t b = a; b < 4; b++)
            for (uint8_t c = b; c < 4; c++)
                for (uint8_t d = c; d < 4; d++) {
                    uint16_t s = (uint16_t)(lum[a] + lum[b] + lum[c] + lum[d]);
                    uint8_t j = n;
                    while (j > 0 && sum[j - 1] > s) {
                        sum[j] = sum[j - 1];
                        mix[j][0] = mix[j - 1][0];
                        mix[j][1] = mix[j - 1][1];
                        mix[j][2] = mix[j - 1][2];
                        mix[j][3] = mix[j - 1][3];
                        j--;
                    }
                    sum[j] = s;
                    mix[j][0] = a;
                    mix[j][1] = b;
                    mix[j][2] = c;
                    mix[j][3] = d;
                    n++;
                }
    RC_CHECK(n, ==, (uint8_t)PT_NUM_SHADES);

    for (uint8_t i = 0; i < PT_NUM_SHADES; i++) {
        pt_pattern s = pt_shade(i);
        const uint8_t *m = mix[i];

        // the tile holds the mix exactly: each mix pixel twice over the pair
        uint8_t want[4] = {0, 0, 0, 0};
        for (uint8_t j = 0; j < 4; j++)
            want[m[j]] += 2;
        uint8_t got[4] = {0, 0, 0, 0};
        for (uint8_t p = 0; p < 4; p++) {
            got[pixel(s.e, p)]++;
            got[pixel(s.o, p)]++;
        }
        for (uint8_t c = 0; c < 4; c++)
            RC_CHECK(got[c], ==, want[c]);

        uint8_t used[4];
        uint8_t nu = 0;
        for (uint8_t c = 0; c < 4; c++)
            if (want[c])
                used[nu++] = c;

        if (nu == 2 && want[used[0]] == 4) {
            // 2+2: the single-pixel checkerboard, darkest colour top left
            RC_CHECK(s.e, ==, PT_PX4(used[0], used[1], used[0], used[1]));
            RC_CHECK(s.o, ==, PT_PX4(used[1], used[0], used[1], used[0]));
        } else if (nu == 3) {
            // 2+1+1: the pair colour checkers against the singles, the
            // darker single on the even line, the darkest colour top left
            uint8_t p = want[used[0]] == 4 ? used[0] : (want[used[1]] == 4 ? used[1] : used[2]);
            uint8_t s1 = p == used[0] ? used[1] : used[0];
            uint8_t s2 = p == used[2] ? used[1] : used[2];
            if (p < s1) {
                RC_CHECK(s.e, ==, PT_PX4(p, s1, p, s1));
                RC_CHECK(s.o, ==, PT_PX4(s2, p, s2, p));
            } else {
                RC_CHECK(s.e, ==, PT_PX4(s1, p, s1, p));
                RC_CHECK(s.o, ==, PT_PX4(p, s2, p, s2));
            }
        } else {
            // solids, 3+1 fills and the full mix: every scanline of a
            // non-solid tile carries every hue of the mix
            uint8_t ine = 0;
            uint8_t ino = 0;
            for (uint8_t p = 0; p < 4; p++) {
                ine |= (uint8_t)(1 << pixel(s.e, p));
                ino |= (uint8_t)(1 << pixel(s.o, p));
            }
            for (uint8_t c = 0; c < 4; c++)
                if (want[c]) {
                    RC_CHECK((ine >> c) & 1, ==, (uint8_t)1);
                    RC_CHECK((ino >> c) & 1, ==, (uint8_t)1);
                }
        }
    }

    RC_CHECK(pt_shade(PT_SH_KKKK).e, ==, (uint8_t)0x00);
    RC_CHECK(pt_shade(PT_SH_KKKK).o, ==, (uint8_t)0x00);
    RC_CHECK(pt_shade(PT_SH_GGGG).e, ==, (uint8_t)0xFF);
    RC_CHECK(pt_shade(PT_SH_GGGG).o, ==, (uint8_t)0xFF);
}
