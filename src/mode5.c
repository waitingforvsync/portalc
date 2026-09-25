// mode5.c - back buffer plotting, the EOR fill, and RGBA expansion.

#include "mode5.h"

#include <string.h>

#include "richc/macros.h"

// The two bits of pixel p within a MODE 5 byte.
static const uint8_t pixmask_[4] = {0x88, 0x44, 0x22, 0x11};

void pt_mode5_clear(uint8_t *fb)
{
    memset(fb, 0, PT_FB_BYTES);
}

void pt_mode5_plot(uint8_t *fb, uint8_t col, uint8_t row, pt_pattern pat)
{
    RC_ASSERT(col < PT_SCREEN_W && row < PT_SCREEN_ROWS);

    uint8_t mask = pixmask_[col & 3];
    uint16_t off = pt_mode5_offset(col >> 2, (uint8_t)(row << 1));

    // An even scanline is never the last of its character cell, so the odd
    // partner is always at off + 1.
    fb[off] ^= pat.e & mask;
    fb[off + 1] ^= pat.o & mask;
}

void pt_mode5_fill(uint8_t *fb)
{
    for (uint8_t bc = 0; bc < 32; bc++) {

        // Even pass: each byte becomes the EOR of itself and everything above.
        uint8_t acc = 0;
        for (uint8_t sl = 0; sl < PT_SCREEN_SCANLINES; sl = (uint8_t)(sl + 2)) {
            uint16_t off = pt_mode5_offset(bc, sl);
            acc ^= fb[off];
            fb[off] = acc;
        }

        // Odd pass.
        acc = 0;
        for (uint8_t sl = 1; sl < PT_SCREEN_SCANLINES; sl = (uint8_t)(sl + 2)) {
            uint16_t off = pt_mode5_offset(bc, sl);
            acc ^= fb[off];
            fb[off] = acc;
        }
    }
}

void pt_mode5_to_rgba(const uint8_t *fb, uint8_t *rgba)
{
    for (uint32_t sl = 0; sl < PT_SCREEN_SCANLINES; sl++) {
        uint8_t *out = rgba + sl * PT_SCREEN_W * 4;
        for (uint32_t bc = 0; bc < 32; bc++) {
            uint8_t b = fb[pt_mode5_offset((uint8_t)bc, (uint8_t)sl)];
            for (uint32_t p = 0; p < 4; p++) {
                uint8_t hi = (uint8_t)((b >> (7 - p)) & 1);
                uint8_t lo = (uint8_t)((b >> (3 - p)) & 1);
                uint32_t c = pt_palette_rgba((uint8_t)((hi << 1) | lo));
                out[0] = (uint8_t)(c & 0xFF);
                out[1] = (uint8_t)((c >> 8) & 0xFF);
                out[2] = (uint8_t)((c >> 16) & 0xFF);
                out[3] = (uint8_t)((c >> 24) & 0xFF);
                out += 4;
            }
        }
    }
}
