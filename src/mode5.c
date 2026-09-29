// mode5.c - back buffer plotting, the EOR fill, and RGBA expansion.

#include "mode5.h"

#include <string.h>

#include "richc/macros.h"

void pt_mode5_clear(uint8_t *fb)
{
    memset(fb, 0, PT_FB_BYTES);
}

void pt_mode5_plot(uint8_t *fb, uint8_t col, uint8_t row, pt_pattern pat)
{
    RC_ASSERT(col < PT_SCREEN_WIDTH);
    pt_mode5_eor(fb, pt_mode5_at(col >> 2, row), pt_mode5_pixmask(col), pat);
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
        uint8_t *out = rgba + sl * PT_SCREEN_WIDTH * 4;
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
