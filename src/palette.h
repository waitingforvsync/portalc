// palette.h - MODE 5 colours and 2x2 dither patterns. A drawable colour is a
// pair of byte patterns (E, O), one for even scanlines and one for odd; the
// EOR fill works on these pairs throughout.

#ifndef PT_PALETTE_H_
#define PT_PALETTE_H_

#include <stdint.h>

// The four MODE 5 hardware colours.
enum {
    PT_BLACK = 0,
    PT_BLUE = 1,
    PT_RED = 2,
    PT_GREEN = 3,
};

// A dithered colour: full-byte MODE 5 pixel patterns for even/odd scanlines.
typedef struct pt_pattern {
    uint8_t e;
    uint8_t o;
} pt_pattern;

// Component-wise EOR of two patterns (the line colour rule).
static inline pt_pattern pt_pattern_xor(pt_pattern a, pt_pattern b)
{
    return (pt_pattern) {.e = (uint8_t)(a.e ^ b.e), .o = (uint8_t)(a.o ^ b.o)};
}

// Solid colour c (0..3), all four pixels of the byte.
pt_pattern pt_pattern_solid(uint8_t c);

// 2x2 checkerboard of colours a and b (a leads on even scanlines).
pt_pattern pt_pattern_check(uint8_t a, uint8_t b);

// Any 2x2 dither cell from its four pixel colours (top-left, top-right,
// bottom-left, bottom-right), tiled across the screen. Beware: a cell whose
// scanlines differ in hue reads as horizontal stripes on the doubled rows;
// pt_pattern_blend spreads a mix over the full 4x2 byte tile instead.
pt_pattern pt_pattern_cell(uint8_t tl, uint8_t tr, uint8_t bl, uint8_t br);

// A blend of colours a and b: k of the 8 pixels of the 4x2 byte tile are b,
// in quarter densities only (k = 0, 2, 4, 6, 8), from a hardcoded table of
// fill shapes. 50% is the checkerboard (already chunky on screen, since
// MODE 5 pixels are double wide); the quarter and three-quarter levels put
// a dot (or hole) in each 2x2 half of the tile, staggered across the
// scanlines so nothing stacks into stripes. Two-colour blends dither far
// better than mixes of three or four, and blends of colours adjacent in
// brightness (black-blue, blue-red, red-green) best of all.
pt_pattern pt_pattern_blend(uint8_t a, uint8_t b, uint8_t k);

// The 35 distinct mixes of four pixels from the four colours, indexed from
// darkest to brightest by the luminance of the hardware colours
// (black < blue < red < green), each arranged over the 4x2 tile so every
// scanline carries every hue of the mix.
#define PT_NUM_SHADES 35
pt_pattern pt_shade(uint8_t i);

// Hardware colour as packed RGBA (R in the low byte), for presentation.
uint32_t pt_palette_rgba(uint8_t c);

#endif
