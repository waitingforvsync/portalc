// arith.h - 6502-friendly fixed-point arithmetic: every multiply and divide in
// the engine goes through these helpers, which use only the quarter-square,
// reciprocal and sine tables plus shifts and adds (as the BBC build would).
// Values that are only added or subtracted are ordinary two's complement;
// a multiply or divide works on unsigned magnitudes, with the sign held in a
// condition that picks between two paths (or a negated-product lookup).

#ifndef PT_ARITH_H_
#define PT_ARITH_H_

#include <stdint.h>

// A normalised reciprocal: r approximates 2^(16+s) / n.
typedef struct pt_recip {
    uint16_t r;
    uint8_t s;
} pt_recip;

// Build the tables; call once before anything else (idempotent).
void pt_arith_init(void);

// sin(angle) in s0.8 for 256ths of a turn: a 128-entry table over [0, pi)
// reused negated for the second half, with 0 and 128 special cased.
int16_t pt_sin(uint8_t angle);

// cos(angle) in s0.8.
static inline int16_t pt_cos(uint8_t angle)
{
    return pt_sin((uint8_t)(angle + 64));
}

// ---- unsigned multiplies ----

// a * b via the quarter-square tables; the primitive everything else builds on.
uint16_t pt_mulu8(uint8_t a, uint8_t b);

// -(a * b) as two's complement: the same two lookups subtracted the other way
// round, so an accumulate can add a negative product without negating.
uint16_t pt_mulu8_neg(uint8_t a, uint8_t b);

// (a * f) >> 8, f a u0.8 fraction; exact, and needs no width beyond u16.
uint16_t pt_mulu16_frac(uint16_t a, uint8_t f);

// -((a * f) >> 8) as two's complement, from the negated-product partials.
uint16_t pt_mulu16_frac_neg(uint16_t a, uint8_t f);

// gradient magnitude (u7.8) times a column count, saturating at 0xFFFF.
uint16_t pt_mulg_u16(uint16_t g, uint8_t dx);

// (a * s) >> 8, rounded to nearest, for signed a and a sine value s in
// [-256, 256]: the sign pair picks the positive or the negated product path.
// Rounding keeps the vertex transform unbiased.
int16_t pt_muls16_sine(int16_t a, int16_t s);

// a + t * (b - a) for a u0.8 parameter t: the comparison picks the direction,
// and the difference is multiplied unsigned.
int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t);

// ---- reciprocal-table divisions ----

// Normalised reciprocal of n (n > 0).
pt_recip pt_recip16(uint16_t n);

// num / den as a u0.8 clip parameter t, clamped to 255; needs num <= den.
uint8_t pt_div_t(uint16_t num, uint16_t den);

// floor(num / den) for two u8.8 values, clamped to 127: the crossing quotient.
uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8);

// (num << 8) / den for a numerator up to 24 bits, clamped to 0xFFFF.
uint16_t pt_div_q8(uint32_t num, uint16_t den);

// (dy << 8) / dx: a gradient magnitude from a row delta and a column count.
uint16_t pt_grad_mag(uint8_t dy, uint8_t dx);

// (dy << 16) / dx_q8: the near clip's fractional-dx gradient magnitude,
// clamped to the u7.8-with-sign-bit range.
uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8);

// ---- projections ----

// floor(64 + 64*y/x): the screen column of an in-frustum vertex (|y| <= x).
uint8_t pt_project_col(int16_t y, uint16_t x);

// The same projection at depth d kept as u8.8 (the near clip's sx*).
uint16_t pt_project_col_q8(int16_t y, uint16_t d);

// PT_SCREEN_HEIGHT/2 + 64*h/x: the screen row of a line endpoint; needs the
// endpoint on screen vertically, x >= 128|h|/PT_SCREEN_HEIGHT (the 64-row
// screen's classic x >= 2|h|).
uint8_t pt_project_row(int16_t h, uint16_t x);

#endif
