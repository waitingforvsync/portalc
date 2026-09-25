// test_mode5.c - screen layout, plotting and the EOR fill.

#include <string.h>

#include "mode5.h"
#include "richc/test.h"

RC_TEST(mode5, offsets)
{
    // BBC layout: 8 consecutive bytes per character cell, 256 per char row
    RC_CHECK(pt_mode5_offset(0, 0), ==, (uint16_t)0);
    RC_CHECK(pt_mode5_offset(0, 7), ==, (uint16_t)7);
    RC_CHECK(pt_mode5_offset(1, 0), ==, (uint16_t)8);
    RC_CHECK(pt_mode5_offset(0, 8), ==, (uint16_t)256);
    RC_CHECK(pt_mode5_offset(31, 127), ==, (uint16_t)4095);
}

RC_TEST(mode5, plot_and_fill)
{
    static uint8_t fb[PT_FB_BYTES];
    pt_mode5_clear(fb);

    // a solid red span from row 10 down to row 20 in pixel column 5:
    // toggle red on at row 10 and off at row 20
    pt_pattern red = pt_pattern_solid(PT_RED);
    pt_mode5_plot(fb, 5, 10, red);
    pt_mode5_plot(fb, 5, 20, red);
    pt_mode5_fill(fb);

    static uint8_t rgba[PT_SCREEN_W * PT_SCREEN_SCANLINES * 4];
    pt_mode5_to_rgba(fb, rgba);

    // pixel column 5: black above, red for scanlines 20..39, black below
    for (uint32_t sl = 0; sl < PT_SCREEN_SCANLINES; sl++) {
        const uint8_t *p = &rgba[(sl * PT_SCREEN_W + 5) * 4];
        bool red_here = sl >= 20 && sl < 40;
        RC_CHECK(p[0], ==, (uint8_t)(red_here ? 255 : 0));
        RC_CHECK(p[1], ==, (uint8_t)0);
        RC_CHECK(p[2], ==, (uint8_t)0);
    }

    // neighbouring pixel columns untouched
    for (uint32_t sl = 0; sl < PT_SCREEN_SCANLINES; sl++) {
        RC_CHECK(rgba[(sl * PT_SCREEN_W + 4) * 4], ==, (uint8_t)0);
        RC_CHECK(rgba[(sl * PT_SCREEN_W + 6) * 4], ==, (uint8_t)0);
    }
}

RC_TEST(mode5, dither_alignment)
{
    static uint8_t fb[PT_FB_BYTES];
    pt_mode5_clear(fb);

    // a red/green checker from row 0: even scanlines lead red
    pt_pattern chk = pt_pattern_check(PT_RED, PT_GREEN);
    pt_mode5_plot(fb, 0, 0, chk);
    pt_mode5_plot(fb, 1, 0, chk);
    pt_mode5_fill(fb);

    static uint8_t rgba[PT_SCREEN_W * PT_SCREEN_SCANLINES * 4];
    pt_mode5_to_rgba(fb, rgba);

    // scanline 0 pixel 0 red, pixel 1 green; scanline 1 swapped
    RC_CHECK(rgba[0 * 4 + 1], ==, (uint8_t)0);      // (0,0) green channel: red
    RC_CHECK(rgba[1 * 4 + 1], ==, (uint8_t)255);    // (1,0): green
    const uint8_t *sl1 = &rgba[PT_SCREEN_W * 4];
    RC_CHECK(sl1[0 * 4 + 1], ==, (uint8_t)255);     // (0,1): green
    RC_CHECK(sl1[1 * 4 + 1], ==, (uint8_t)0);       // (1,1): red
}
