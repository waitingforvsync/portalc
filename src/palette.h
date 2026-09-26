// palette.h - MODE 5 colours and 2x2 dither patterns. A drawable colour is a
// pair of byte patterns (E, O), one for even scanlines and one for odd; the
// EOR fill works on these pairs throughout. Every engine colour comes from
// the hardcoded 35-shade catalogue in palette.c.

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

// One scanline byte from its four pixel colours, left to right: pixel p
// (0 leftmost) is bits 7-p (colour high bit) and 3-p (low bit).
#define PT_PX4(p0, p1, p2, p3)                                                             \
    ((uint8_t)((((p0) >> 1) << 7 | ((p1) >> 1) << 6 | ((p2) >> 1) << 5 | ((p3) >> 1) << 4) \
               | (((p0) & 1) << 3 | ((p1) & 1) << 2 | ((p2) & 1) << 1 | ((p3) & 1))))

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

// The 35 distinct mixes of four pixels from the four colours, named by mix
// and indexed from darkest to brightest by summed Rec. 601 luminance of the
// hardware colours (black < blue < red < green).
enum {
    PT_SH_KKKK,
    PT_SH_KKKB,
    PT_SH_KKBB,
    PT_SH_KKKR,
    PT_SH_KBBB,
    PT_SH_KKBR,
    PT_SH_BBBB,
    PT_SH_KBBR,
    PT_SH_KKKG,
    PT_SH_KKRR,
    PT_SH_BBBR,
    PT_SH_KKBG,
    PT_SH_KBRR,
    PT_SH_KBBG,
    PT_SH_BBRR,
    PT_SH_KKRG,
    PT_SH_KRRR,
    PT_SH_BBBG,
    PT_SH_KBRG,
    PT_SH_BRRR,
    PT_SH_BBRG,
    PT_SH_KKGG,
    PT_SH_KRRG,
    PT_SH_RRRR,
    PT_SH_KBGG,
    PT_SH_BRRG,
    PT_SH_BBGG,
    PT_SH_KRGG,
    PT_SH_RRRG,
    PT_SH_BRGG,
    PT_SH_KGGG,
    PT_SH_RRGG,
    PT_SH_BGGG,
    PT_SH_RGGG,
    PT_SH_GGGG,
    PT_NUM_SHADES,
};

// Shade i of the catalogue. Two-colour blends dither far better than mixes
// of three or four hues, and blends of colours adjacent in brightness
// (black-blue, blue-red, red-green) best of all.
pt_pattern pt_shade(uint8_t i);

// Hardware colour as packed RGBA (R in the low byte), for presentation.
uint32_t pt_palette_rgba(uint8_t c);

#endif
