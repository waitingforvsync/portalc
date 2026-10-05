// plot.h - the line plotters, as the 6502 runs them. A run is plotted
// through a screen cursor (a character block's address and Y,
// pt_mode5_cur) placed once at its first pixel and then only moved: a row
// down or up, a byte right, a pixel right. Every pixel is a toggle, EORed
// into both scanlines of its row, the even and the odd pattern.

#ifndef PT_PLOT_H_
#define PT_PLOT_H_

#include <stdint.h>

#include "mode5.h"
#include "palette.h"
#include "richc/macros.h"

// A gradient: a u7.8 magnitude with the sign in bit 15 (not two's
// complement); zero is always stored positive.
static inline uint16_t pt_g_make(uint16_t mag, uint8_t neg)
{
    RC_ASSERT(mag <= 0x7FFF);
    return (neg && mag) ? (uint16_t)(mag | 0x8000u) : mag;
}

static inline uint16_t pt_g_mag(uint16_t g)
{
    return (uint16_t)(g & 0x7FFFu);
}

static inline uint8_t pt_g_isneg(uint16_t g)
{
    return (uint8_t)(g >> 15);
}

// A stretch of a line: columns [x0, x1) from the row at x0 (y0, and its
// fraction yf with the rounding half in), stepping by g.
typedef struct pt_run {
    uint8_t x0;
    uint8_t x1;
    uint8_t y0;
    uint8_t yf;
    uint16_t g;
} pt_run;

// A run whose row moves by at most one a column (|g| < 1): a row byte and a
// fraction, whole bytes four pixels at a time through eight fixed routines
// picked by their carries, the partial bytes at the ends a pixel at a time.
// A flat run is whole bytes on one row, the ends masked.
void pt_plot_shallow(uint8_t *fb, const pt_run *p, pt_pattern pat);

// A steep run (|g| >= 1): a pixel pair a column.
void pt_plot_steep(uint8_t *fb, const pt_run *p, pt_pattern pat);

// A flat run on one row, over [x0, x1).
void pt_plot_flat(uint8_t *fb, uint8_t x0, uint8_t x1, uint8_t row, pt_pattern pat);

#endif
