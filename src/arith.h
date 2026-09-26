// arith.h - 6502-friendly fixed-point arithmetic: every multiply and divide in
// the engine goes through these helpers, which use only the quarter-square,
// reciprocal and sine tables plus shifts and adds (as the BBC build would).
// Values that are only added or subtracted are ordinary two's complement;
// a multiply or divide works on unsigned magnitudes, with the sign held in a
// condition that picks between two paths (or a negated-product lookup).

#ifndef PT_ARITH_H_
#define PT_ARITH_H_

#include <stdint.h>

// Build the tables; call once before anything else (idempotent).
void pt_arith_init(void);

// A rotated vector.
typedef struct pt_xy {
    int16_t x, y;
} pt_xy;

// Rotate (x, y) into the frame of an angle in 256ths of a turn:
// x' = x cos + y sin, y' = y cos - x sin, each product rounded to nearest.
// A sine value never exists: each angle's low 7 bits index the magnitude
// table (index 0 forking to a zero term, a stored 0 to a copy) and its top
// bit is the sign, decided at the point of use. The two magnitude/sign
// splits and the four unsigned products are shared between the components,
// and every product enters its sum by add or subtract -- opposite signs are
// one subtraction whose wraparound is the two's complement, and only two
// negative terms negate at all (once, of the finished sum), which the 6502
// sidesteps by accumulating that pair from the negated-product table
// (pt_mulu8_neg).
pt_xy pt_rot(int16_t x, int16_t y, uint8_t angle);

// The world direction of an angle scaled by a whole-unit magnitude:
// (mag cos, mag sin) in s8.8, one exact 8-bit product per component (the
// camera's facing step).
pt_xy pt_dir8(uint8_t mag, uint8_t angle);

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

// (v * f) >> 8 for a u24 value: the keep-high byte chain, three lookups
// (fewer when v's top bytes are zero); exact like pt_mulu16_frac.
uint32_t pt_mulu24_frac(uint32_t v, uint8_t f);

// gradient magnitude (u7.8) times a column count, saturating at 0xFFFF.
uint16_t pt_mulg_u16(uint16_t g, uint8_t dx);

// a + t * (b - a) for a u0.8 parameter t: the comparison picks the direction,
// and the difference is multiplied unsigned.
int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t);

// ---- reciprocal-table divisions ----
// Each is num * 2^k / den for its own k: den is normalised, the numerator is
// pre-shifted when the result scale calls for it, and the product is one add
// plus a single u24 x u8 fractional multiply (at most three table lookups,
// fewer when the numerator's top bytes are zero). Error stays within ~0.2%
// of the result plus a count of truncation.

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

// 64*h/x for a height magnitude h = 8*h8 (heights are quantised to 8-unit
// steps so they fit a byte): the row offset of a line endpoint from the
// screen centre, the caller adding or subtracting by the height's side.
// Needs the endpoint on screen vertically, x >= 128*h/PT_SCREEN_HEIGHT (the
// 64-row screen's classic x >= 2h).
uint8_t pt_project_rowd(uint8_t h8, uint16_t x);

#endif
