// mode5.h - the back buffer: a shrunken BBC MODE 5 screen, PT_SCREEN_WIDTH
// pixels (a byte holds 4) by PT_SCREEN_SCANLINES scanlines, laid out exactly
// as BBC screen memory (8-scanline character rows). The engine works in
// canvas rows: row y is the scanline pair (2y, 2y+1), carrying the even/odd
// dither patterns.

#ifndef PT_MODE5_H_
#define PT_MODE5_H_

#include <stdint.h>

#include "palette.h"
#include "richc/macros.h"

#define PT_SCREEN_WIDTH 128
#define PT_SCREEN_HEIGHT 64    // canvas rows
#define PT_SCANLINES_PER_ROW 2
#define PT_SCREEN_SCANLINES (PT_SCREEN_HEIGHT * PT_SCANLINES_PER_ROW)
#define PT_FB_BYTES (PT_SCREEN_WIDTH / 4 * PT_SCREEN_SCANLINES)

#define PT_FB_BLOCK 8                               // bytes in a character block, a scanline each
#define PT_FB_CHAR_ROW (PT_SCREEN_WIDTH / 4 * 8)    // bytes in a character row

// A junk character row either side of the screen: a line's rounding can
// carry its last rows just past the top or bottom, and they are plotted
// there, never displayed or filled. Canvas rows -4..-1 above, H..H+3 below.
#define PT_FB_GUARD_ROWS (PT_FB_BLOCK / PT_SCANLINES_PER_ROW)
#define PT_FB_GUARD PT_FB_CHAR_ROW

// Byte offset of (byte column, scanline) in the BBC character-cell layout.
static inline uint16_t pt_mode5_offset(uint8_t bytecol, uint8_t scanline)
{
    return (uint16_t)(((scanline & 0xF8u) << 5) + ((unsigned)bytecol << 3) + (scanline & 7u));
}

// A screen position as the 6502 addresses it, for EOR (blk),Y : STA (blk),Y:
// a character block's address in zero page and the scanline within it in Y.
// A canvas row is the scanline pair Y, Y + 1 with Y even (so the odd one is
// never past the block's end). The address is kept relative to the screen's
// first byte (negative in the top junk row), and only dereferenced by a plot.
typedef struct pt_mode5_cur {
    int16_t blk;
    uint8_t y;
} pt_mode5_cur;

// The two bits of a pixel column in its byte, 0x88 the leftmost.
static inline uint8_t pt_mode5_pixmask(uint8_t col)
{
    return (uint8_t)(0x88u >> (col & 3));
}

// The position of canvas row at byte column bytecol, row 0..H: a run or a
// toggle only ever starts on the screen or in the junk row below it, and
// steps from there (into the junk rows, which fb must have either side of
// the screen, if it overruns). On the 6502 a character row is 256 bytes,
// so the block's high byte is the buffer's plus row / 4: a table's for T,
// the one buffer (trowhi), and LSR A twice for a screen. Its low byte is
// bytecol * 8, and Y (row & 3) * 2 (AND #3 : ASL A).
static inline pt_mode5_cur pt_mode5_at(uint8_t bytecol, uint8_t row)
{
    uint8_t grow = (uint8_t)(row + PT_FB_GUARD_ROWS);    // counted from the top junk row
    RC_ASSERT(bytecol < PT_SCREEN_WIDTH / 4 && row <= PT_SCREEN_HEIGHT);
    return (pt_mode5_cur) {
        .blk = (int16_t)((grow >> 2) * PT_FB_CHAR_ROW + bytecol * PT_FB_BLOCK - PT_FB_GUARD),
        .y = (uint8_t)((grow & 3) << 1),
    };
}

// EOR the pattern bits under mask (any of the byte's pixels) into the canvas
// row: E into scanline Y, O into Y + 1.
static inline void pt_mode5_eor(uint8_t *fb, pt_mode5_cur c, uint8_t mask, pt_pattern pat)
{
    int16_t a = (int16_t)(c.blk + c.y);
    RC_ASSERT(a >= -PT_FB_GUARD && a < PT_FB_BYTES + PT_FB_GUARD - 1);
    fb[a] ^= pat.e & mask;
    fb[a + 1] ^= pat.o & mask;
}

// Down a canvas row: Y up two, and from the block's last pair to the top of
// the block below, a character row on.
static inline void pt_mode5_down(pt_mode5_cur *c)
{
    c->y = (uint8_t)(c->y + 2);
    if (c->y == PT_FB_BLOCK) {
        c->y = 0;
        c->blk = (int16_t)(c->blk + PT_FB_CHAR_ROW);
    }
}

// Up a canvas row: Y down two, and from the block's first pair to the
// bottom of the block above.
static inline void pt_mode5_up(pt_mode5_cur *c)
{
    if (c->y == 0) {
        c->y = PT_FB_BLOCK;
        c->blk = (int16_t)(c->blk - PT_FB_CHAR_ROW);
    }
    c->y = (uint8_t)(c->y - 2);
}

// Right a byte column: the next block along the character row.
static inline void pt_mode5_next(pt_mode5_cur *c)
{
    c->blk = (int16_t)(c->blk + PT_FB_BLOCK);
}

// Right a pixel column: the mask moves along the byte, and from its last
// pixel to the first of the next block (LSR mask, the carry out marking it).
static inline void pt_mode5_right(pt_mode5_cur *c, uint8_t *mask)
{
    *mask >>= 1;
    if (*mask == 0x08) {
        *mask = 0x88;
        pt_mode5_next(c);
    }
}

// Zero a canvas row's two scanlines in the byte at the cursor, whole: the
// unplotter's store, no read and no mask.
//   LDA #0 : STA (c),Y : INY : STA (c),Y
static inline void pt_mode5_zero(uint8_t *fb, pt_mode5_cur c)
{
    int16_t a = (int16_t)(c.blk + c.y);
    RC_ASSERT(a >= -PT_FB_GUARD && a < PT_FB_BYTES + PT_FB_GUARD - 1);
    fb[a] = 0;
    fb[a + 1] = 0;
}

// EOR-plot one pixel pair, pixel column col, canvas row row (as pt_mode5_at).
void pt_mode5_plot(uint8_t *fb, uint8_t col, uint8_t row, pt_pattern pat);

// The EOR vertical fill, from the toggle buffer t into the screen s, which
// it writes every byte of: down each byte column, its even scanlines from
// start.e and its odd from start.o, each byte the EOR of everything toggled
// above and in it. Under a column's last toggled row (toggle_end, the row after
// it: 1..64, or 65 for the junk row, the same as 64) nothing more is
// toggled, and the rest of the column is stored as it is.
void pt_mode5_fill(uint8_t *s, const uint8_t *t, pt_pattern start,
                   const uint8_t toggle_end[PT_SCREEN_WIDTH / 4]);

// Expand the filled buffer to 128x128 RGBA8 (R first in memory) for display.
void pt_mode5_to_rgba(const uint8_t *fb, uint8_t *rgba);

#endif
