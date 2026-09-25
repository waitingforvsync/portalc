// test_palette.c - the dither cell builder and the 35-shade catalogue.

#include "palette.h"
#include "richc/test.h"

// The colour of pixel p (0 leftmost) in a MODE 5 pattern byte.
static uint8_t pixel(uint8_t byte, uint8_t p)
{
    return (uint8_t)((((byte >> (7 - p)) & 1) << 1) | ((byte >> (3 - p)) & 1));
}

RC_TEST(palette, cell_pixels)
{
    for (uint8_t tl = 0; tl < 4; tl++)
        for (uint8_t tr = 0; tr < 4; tr++)
            for (uint8_t bl = 0; bl < 4; bl++)
                for (uint8_t br = 0; br < 4; br++) {
                    pt_pattern c = pt_pattern_cell(tl, tr, bl, br);
                    RC_CHECK(pixel(c.e, 0), ==, tl);
                    RC_CHECK(pixel(c.e, 1), ==, tr);
                    RC_CHECK(pixel(c.e, 2), ==, tl);
                    RC_CHECK(pixel(c.e, 3), ==, tr);
                    RC_CHECK(pixel(c.o, 0), ==, bl);
                    RC_CHECK(pixel(c.o, 1), ==, br);
                    RC_CHECK(pixel(c.o, 2), ==, bl);
                    RC_CHECK(pixel(c.o, 3), ==, br);
                }

    // solid and check are the corresponding cells
    for (uint8_t a = 0; a < 4; a++) {
        pt_pattern s = pt_pattern_solid(a);
        pt_pattern c = pt_pattern_cell(a, a, a, a);
        RC_CHECK(s.e, ==, c.e);
        RC_CHECK(s.o, ==, c.o);
        for (uint8_t b = 0; b < 4; b++) {
            pt_pattern k = pt_pattern_check(a, b);
            pt_pattern d = pt_pattern_cell(a, b, b, a);
            RC_CHECK(k.e, ==, d.e);
            RC_CHECK(k.o, ==, d.o);
        }
    }
}

RC_TEST(palette, blend)
{
    for (uint8_t a = 0; a < 4; a++)
        for (uint8_t b = 0; b < 4; b++) {
            if (a == b)
                continue;
            for (uint8_t k = 0; k <= 8; k++) {
                pt_pattern p = pt_pattern_blend(a, b, k);
                uint8_t nb = 0;
                uint8_t ne = 0;
                for (uint8_t px = 0; px < 4; px++) {
                    uint8_t ce = pixel(p.e, px);
                    uint8_t co = pixel(p.o, px);
                    RC_CHECK_TRUE(ce == a || ce == b);
                    RC_CHECK_TRUE(co == a || co == b);
                    nb = (uint8_t)(nb + (ce == b) + (co == b));
                    ne = (uint8_t)(ne + (ce == b));
                }
                RC_CHECK(nb, ==, k);
                // partial fills stagger: the scanlines never differ by more
                // than one pixel of b for the mid levels
                if (k >= 2 && k <= 6) {
                    uint8_t no = (uint8_t)(k - ne);
                    uint8_t d = ne > no ? (uint8_t)(ne - no) : (uint8_t)(no - ne);
                    RC_CHECK(d, <=, (uint8_t)1);
                }
            }
        }
}

RC_TEST(palette, shades_ordered)
{
    // Rec. 601 luminance of the hardware colours, per thousand. Every one of
    // the 35 mixes has a distinct sum, so a strictly increasing sweep proves
    // both the ordering and that no mix appears twice.
    static const uint16_t lum[4] = {0, 114, 299, 587};

    RC_CHECK(pt_shade(0).e, ==, pt_pattern_solid(PT_BLACK).e);
    RC_CHECK(pt_shade(PT_NUM_SHADES - 1).e, ==, pt_pattern_solid(PT_GREEN).e);

    uint32_t prev = 0;
    for (uint8_t i = 0; i < PT_NUM_SHADES; i++) {
        pt_pattern s = pt_shade(i);
        uint32_t b = 0;
        for (uint8_t px = 0; px < 4; px++)
            b += (uint32_t)lum[pixel(s.e, px)] + lum[pixel(s.o, px)];
        if (i > 0)
            RC_CHECK(b, >, prev);
        prev = b;
    }
}
