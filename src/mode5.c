// mode5.c - back buffer plotting, the EOR fill, and RGBA expansion.

#include "mode5.h"

#include "richc/macros.h"

void pt_mode5_plot(uint8_t *fb, uint8_t col, uint8_t row, pt_pattern pat)
{
    RC_ASSERT(col < PT_SCREEN_WIDTH);
    pt_mode5_eor(fb, pt_mode5_at(col >> 2, row), pt_mode5_pixmask(col), pat);
}

// The 6502's fill. T and S share the layout, a byte column's block at
// column * 8 in each 256-byte character row, so one unrolled block, a
// canvas row's EOR and store each,
//   .body
//       EOR T + r * 256 + k, X : STA S + r * 256 + k, X    ; rows 0..63
//       RTS
// serves both passes, X the column's block for the even scanlines and one
// on for the odd (abs,X never leaves its page). A column leaves it after
// its last toggled row by an RTS patched over the next row's EOR opcode
// (&5D back to &60 and back), one byte for both passes, and goes on into a
// second block of stores alone, entered at that row:
//   .tailblock
//       STA S + r * 256 + k, X                              ; rows 0..63
//       RTS
// A column, from tables by the row (the exit's address, the tail's; 65,
// past the junk row, the same as 64):
//       LDY lastp1,X
//       LDA exitlo,Y : STA patch+1 : STA unpatch+1
//       LDA exithi,Y : STA patch+2 : STA unpatch+2
//       LDA taillo,Y : STA tail+1 : LDA tailhi,Y : STA tail+2
//       LDA #&60
//   .patch   STA &FFFF                    ; RTS over the next row's EOR
//       LDA start_e : JSR body : JSR tail
//       INX
//       LDA start_o : JSR body : JSR tail
//       DEX
//       LDA #&5D
//   .unpatch STA &FFFF                    ; the EOR back
//   .tail    JMP &FFFF
// About 140 cycles a column on top of the 9 a byte (5 a byte stored alone):
// about 4k a frame, for 4 cycles on each byte under the columns' last
// toggles, half the screen and more. Each S buffer has its own blocks.
void pt_mode5_fill(uint8_t *s, const uint8_t *t, pt_pattern start,
                   const uint8_t lastp1[PT_SCREEN_WIDTH / 4])
{
    for (uint8_t bc = 0; bc < PT_SCREEN_WIDTH / 4; bc++) {
        RC_ASSERT(lastp1[bc] <= PT_SCREEN_HEIGHT + 1);
        uint8_t end = lastp1[bc] > PT_SCREEN_HEIGHT ? (uint8_t)PT_SCREEN_HEIGHT : lastp1[bc];
        for (uint8_t par = 0; par < PT_SCANLINES_PER_ROW; par++) {
            uint8_t acc = par ? start.o : start.e;
            uint8_t row = 0;
            for (; row < end; row++) {
                uint16_t off = pt_mode5_offset(bc, (uint8_t)(2u * row + par));
                acc ^= t[off];
                s[off] = acc;
            }
            for (; row < PT_SCREEN_HEIGHT; row++) {
                uint16_t off = pt_mode5_offset(bc, (uint8_t)(2u * row + par));
                RC_ASSERT(t[off] == 0);    // nothing toggled under the last
                s[off] = acc;
            }
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
