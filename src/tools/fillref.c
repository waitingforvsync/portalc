// fillref.c - the fill's answers as JSON, for make test's check of the Beeb's
// (beeb/test/fill.6502). Run as portalc_fillref > fill.json.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "map.h"
#include "mode5.h"
#include "palette.h"

#define CASES 64    // made-up frames, filled into screen A and B by turns

static pt_map m;
static uint8_t tmem[PT_FB_BYTES + 2 * PT_FB_GUARD];
static uint8_t screen[PT_FB_BYTES];
static uint8_t toggle_end[PT_SCREEN_WIDTH / 4];

// A 16-bit Galois LFSR, stepped before each made-up value, as test/fill.6502 steps it.
static uint16_t random = 0xACE1;

static uint8_t step(void)
{
    random = (uint16_t)((random >> 1) ^ ((random & 1) ? 0xB400 : 0));
    return (uint8_t)random;
}

int main(void)
{
    uint8_t *t = tmem + PT_FB_GUARD;
    unsigned seen[2][PT_SCREEN_HEIGHT + 2] = {{0}};
    pt_scene_init(&m);
    RC_ASSERT(m.num_cells > 16);

    // made-up frames: a colour, or for every other pair, a cell, 1 to 16, whose ceiling's colour
    // the frame takes; each byte column's end, 1 to 65; and T made up above each end, a canvas
    // row's even scanline then its odd, and 0 below: each one's screen after the fill
    printf("{\n  \"screens\": [");
    for (unsigned k = 0; k < CASES; k++) {
        memset(tmem, 0, sizeof tmem);
        pt_pattern start = {.e = step()};
        start.o = step();
        if (k & 2)
            start = pt_shade(m.cells[(step() & 15) + 1].ceil);
        for (unsigned c = 0; c < sizeof toggle_end; c++) {
            uint8_t e = step() & 127;
            toggle_end[c] = (uint8_t)(e >= 65 ? e - 64 : e + 1);
            seen[k & 1][toggle_end[c]]++;
        }
        for (uint8_t c = 0; c < sizeof toggle_end; c++) {
            uint8_t rows = toggle_end[c] > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : toggle_end[c];
            for (uint8_t y = 0; y < rows; y++) {
                t[pt_mode5_offset(c, (uint8_t)(2 * y))] = step();
                t[pt_mode5_offset(c, (uint8_t)(2 * y + 1))] = step();
            }
        }
        pt_mode5_fill(screen, t, start, toggle_end);
        printf("%s\n    \"", k ? "," : "");
        for (unsigned i = 0; i < sizeof screen; i++)
            printf("%02X", screen[i]);
        printf("\"");
    }
    printf("\n  ]\n}\n");

    // every end, into each screen
    unsigned missed = 0;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned e = 1; e <= PT_SCREEN_HEIGHT + 1; e++)
            missed += seen[s][e] == 0;
    fprintf(stderr, "fillref: %u cases, %u of the ends into each screen never made up\n", CASES,
            missed);
    return 0;
}
