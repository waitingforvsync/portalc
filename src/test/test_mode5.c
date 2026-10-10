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

// Every byte column filled EORing all the way down, from black.
static const uint8_t all_rows[PT_SCREEN_WIDTH / 4] = {
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
    PT_SCREEN_HEIGHT, PT_SCREEN_HEIGHT,
};

RC_TEST(mode5, plot_and_fill)
{
    static uint8_t t[PT_FB_BYTES];
    static uint8_t fb[PT_FB_BYTES];
    memset(t, 0, sizeof t);

    // a solid red span from row 10 down to row 20 in pixel column 5:
    // toggle red on at row 10 and off at row 20
    pt_pattern red = pt_shade(PT_SH_RRRR);
    pt_mode5_plot(t, 5, 10, red);
    pt_mode5_plot(t, 5, 20, red);
    pt_mode5_fill(fb, t, (pt_pattern) {0, 0}, all_rows);

    static uint8_t rgba[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];
    pt_mode5_to_rgba(fb, rgba);

    // pixel column 5: black above, red for scanlines 20..39, black below
    for (uint32_t sl = 0; sl < PT_SCREEN_SCANLINES; sl++) {
        const uint8_t *p = &rgba[(sl * PT_SCREEN_WIDTH + 5) * 4];
        bool red_here = sl >= 20 && sl < 40;
        RC_CHECK(p[0], ==, (uint8_t)(red_here ? 255 : 0));
        RC_CHECK(p[1], ==, (uint8_t)0);
        RC_CHECK(p[2], ==, (uint8_t)0);
    }

    // neighbouring pixel columns untouched
    for (uint32_t sl = 0; sl < PT_SCREEN_SCANLINES; sl++) {
        RC_CHECK(rgba[(sl * PT_SCREEN_WIDTH + 4) * 4], ==, (uint8_t)0);
        RC_CHECK(rgba[(sl * PT_SCREEN_WIDTH + 6) * 4], ==, (uint8_t)0);
    }
}

RC_TEST(mode5, dither_alignment)
{
    static uint8_t t[PT_FB_BYTES];
    static uint8_t fb[PT_FB_BYTES];
    memset(t, 0, sizeof t);

    // a red/green checker from row 0: even scanlines lead red
    pt_pattern chk = {PT_PX4(PT_RED, PT_GREEN, PT_RED, PT_GREEN),
                      PT_PX4(PT_GREEN, PT_RED, PT_GREEN, PT_RED)};
    pt_mode5_plot(t, 0, 0, chk);
    pt_mode5_plot(t, 1, 0, chk);
    pt_mode5_fill(fb, t, (pt_pattern) {0, 0}, all_rows);

    static uint8_t rgba[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];
    pt_mode5_to_rgba(fb, rgba);

    // scanline 0 pixel 0 red, pixel 1 green; scanline 1 swapped
    RC_CHECK(rgba[0 * 4 + 1], ==, (uint8_t)0);      // (0,0) green channel: red
    RC_CHECK(rgba[1 * 4 + 1], ==, (uint8_t)255);    // (1,0): green
    const uint8_t *sl1 = &rgba[PT_SCREEN_WIDTH * 4];
    RC_CHECK(sl1[0 * 4 + 1], ==, (uint8_t)255);     // (0,1): green
    RC_CHECK(sl1[1 * 4 + 1], ==, (uint8_t)0);       // (1,1): red
}

RC_TEST(mode5, junk_rows)
{
    // a run starts on the screen or in the junk row below it, and stepping
    // off either edge lands in the junk character rows either side, leaving
    // the screen itself untouched
    static uint8_t mem[PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD];
    uint8_t *fb = mem + PT_FB_GUARD;
    pt_pattern red = pt_shade(PT_SH_RRRR);
    for (uint8_t bc = 0; bc < PT_SCREEN_WIDTH / 4; bc++) {
        pt_mode5_cur up = pt_mode5_at(bc, 0);
        pt_mode5_up(&up);
        pt_mode5_eor(fb, up, 0xFF, red);                                    // row -1
        for (uint8_t i = 1; i < PT_FB_GUARD_ROWS; i++)
            pt_mode5_up(&up);
        pt_mode5_eor(fb, up, 0xFF, red);                                    // the top junk row
        pt_mode5_cur down = pt_mode5_at(bc, PT_SCREEN_HEIGHT);
        pt_mode5_eor(fb, down, 0xFF, red);                                  // row H
        for (uint8_t i = 1; i < PT_FB_GUARD_ROWS; i++)
            pt_mode5_down(&down);
        pt_mode5_eor(fb, down, 0xFF, red);                                  // the last
    }
    for (uint32_t i = 0; i < PT_FB_BYTES; i++)
        RC_CHECK(fb[i], ==, (uint8_t)0);
    RC_CHECK(mem[PT_FB_GUARD - 1], !=, (uint8_t)0);                        // row -1's odd scanline
    RC_CHECK(mem[0], !=, (uint8_t)0);                                      // row -4's even scanline
    RC_CHECK(mem[PT_FB_GUARD + PT_FB_BYTES], !=, (uint8_t)0);             // row H's even scanline
    RC_CHECK(mem[sizeof mem - 1], !=, (uint8_t)0);                         // row H+3's odd scanline
}

// The fill's start and exit: a column starts from the start pattern, EORs
// down to its last toggled row and stores the rest as it is, the same as
// EORing all the way.
RC_TEST(mode5, fill_start_and_exit)
{
    static uint8_t t[PT_FB_BYTES];
    static uint8_t a[PT_FB_BYTES];
    static uint8_t b[PT_FB_BYTES];
    memset(t, 0, sizeof t);
    pt_pattern red = pt_shade(PT_SH_RRRR);
    pt_pattern sky = pt_shade(PT_SH_KKBB);
    pt_mode5_plot(t, 5, 10, red);      // byte column 1: last toggled row 20
    pt_mode5_plot(t, 5, 20, red);
    pt_mode5_plot(t, 40, 3, red);      // byte column 10: row 3

    uint8_t lastp1[PT_SCREEN_WIDTH / 4];
    memset(lastp1, 1, sizeof lastp1);
    lastp1[1] = 21;
    lastp1[10] = 4;
    pt_mode5_fill(a, t, sky, lastp1);
    pt_mode5_fill(b, t, sky, all_rows);
    RC_CHECK(memcmp(a, b, sizeof a), ==, 0);

    // with nothing toggled, a column is its start pattern all the way down
    RC_CHECK(a[pt_mode5_offset(0, 0)], ==, sky.e);
    RC_CHECK(a[pt_mode5_offset(0, 127)], ==, sky.o);
}
