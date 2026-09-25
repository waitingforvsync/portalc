// mode5.h - the back buffer: a shrunken BBC MODE 5 screen, 128 pixels wide by
// 128 scanlines (32 byte columns x 16 character rows x 8 scanlines = 4K), laid
// out exactly as BBC screen memory. The engine works in canvas rows: row y is
// the scanline pair (2y, 2y+1), carrying the even/odd dither patterns.

#ifndef PT_MODE5_H_
#define PT_MODE5_H_

#include <stdint.h>

#include "palette.h"

#define PT_FB_BYTES 4096
#define PT_SCREEN_W 128
#define PT_SCREEN_SCANLINES 128
#define PT_SCREEN_ROWS 64

// Byte offset of (byte column, scanline) in the BBC character-cell layout.
static inline uint16_t pt_mode5_offset(uint8_t bytecol, uint8_t scanline)
{
    return (uint16_t)(((scanline & 0xF8u) << 5) + ((uint16_t)bytecol << 3) + (scanline & 7u));
}

// Clear the whole buffer to zero.
void pt_mode5_clear(uint8_t *fb);

// EOR-plot one pixel pair: pattern bits for pixel column col into the two
// scanlines of canvas row.
void pt_mode5_plot(uint8_t *fb, uint8_t col, uint8_t row, pt_pattern pat);

// The EOR vertical fill: even-scanline pass then odd-scanline pass.
void pt_mode5_fill(uint8_t *fb);

// Expand the filled buffer to 128x128 RGBA8 (R first in memory) for display.
void pt_mode5_to_rgba(const uint8_t *fb, uint8_t *rgba);

#endif
