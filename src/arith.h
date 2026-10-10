// arith.h - 6502-friendly fixed-point arithmetic: every multiply, divide,
// angle and log in the engine goes through these helpers, which use only
// tables plus shifts and adds (as the BBC build would).
// Values that are only added or subtracted are ordinary two's complement;
// a multiply or divide works on unsigned magnitudes, with the sign held in a
// condition that picks between two paths (or a negated-product lookup).

#ifndef PT_ARITH_H_
#define PT_ARITH_H_

#include <stdint.h>

#include "richc/macros.h"

// Build the tables; call once before anything else (idempotent).
void pt_arith_init(void);

// Narrowing, checked: every value is held in the type the 6502 will hold
// it in, and these assert (in a debug build) that it fits. A narrowing
// that wraps on purpose is a plain cast, with its mask where it has one.
static inline uint8_t pt_u8(int32_t v)
{
    RC_ASSERT(v >= 0 && v <= 0xFF);
    return (uint8_t)v;
}

static inline uint16_t pt_u16(int32_t v)
{
    RC_ASSERT(v >= 0 && v <= 0xFFFF);
    return (uint16_t)v;
}

static inline int16_t pt_i16(int32_t v)
{
    RC_ASSERT(v >= INT16_MIN && v <= INT16_MAX);
    return (int16_t)v;
}

// A vector.
typedef struct pt_xy {
    int16_t x, y;
} pt_xy;

// ---- angles and logs ----
// A world angle is 1024ths of a turn from +x towards +y, in 10 bits: the top
// two are the quadrant, which the offset's signs give, and the low byte the
// angle within it, so an edge needs only the byte (its other side differs
// by a half turn). A computed angle is the step the true one lies in: its
// middle is angle + 0.5. The camera's 256ths are four steps each. A view
// angle is a world angle less the camera's, and in view when it's in
// [-128, 128): 45 degrees either side. Logs are 256 * log2, in 16 bits.

// 256 * log2(x) for x >= 1, within ~0.2% of x plus the table's half step:
// the 8 bits under x's leading one, rounded, index a fraction table.
uint16_t pt_log2(uint16_t x);

// The same for a byte b >= 1, with the same answers and no shifts: under 256
// the bits under the leading one are fewer than 8, so the normalised byte,
// its leading one shifted out, indexes the fraction exactly.
//   LDX b : LDA nrm,X : ASL A : TAY : LDA logf,Y    ; the fraction
//   LDY k8,X : DEY                                  ; the whole part
uint16_t pt_log2_8(uint8_t b);

// 2^(l / 256), rounded, saturating at 0xFFFF: pt_log2's inverse, within ~0.2%.
// Under 8 * 256 the result is a byte (or 256, from the rounding's carry),
// and the 9-bit mantissa never needs a second byte: its 1 comes in from the
// carry on the first shift right.
uint16_t pt_exp2(uint16_t l);

// The quadrant of a direction other than (0, 0), from its signs: 0 for +x
// +y, 1 for -x +y, 2 for -x -y, 3 for +x -y. A direction exactly along an
// axis starts its quadrant (+x 0, +y 1, -x 2, -y 3), so a direction and its
// reverse always differ by exactly 2, and an edge's angle byte serves both
// its sides.
uint8_t pt_quadrant(int16_t dx, int16_t dy);

// A world offset in polar form.
typedef struct pt_polar {
    uint16_t angle;    // the world angle (the step it lies in), 10 bits
    int16_t lr;        // 256 * log2 of the length
} pt_polar;

// The polar form of a world offset other than (0, 0), with no multiply or
// divide: the minor component over the major, a byte from the difference
// of their logs, indexes the angle and the length's factor over the major,
// which the major's log takes. The angle is within 1.4 steps of the true
// one's middle (0.38 on average), the length's log within 2.5 units (0.7%).
pt_polar pt_polar_of(int16_t dx, int16_t dy);

// -256 * log2 cos of view angle v at its step's middle: a length's log less
// this is the depth's.
uint8_t pt_lsec(int8_t v);

// -256 log2 cos at the middle of step i of the right angle, i = 0..255: 0 to
// 2,137, past 45 degrees (128 on) in 16 bits.
uint16_t pt_lsec16(uint8_t i);

// 256 log2 |sin a| at the middle of step a, for any 10-bit angle: 0 down to
// -2,137 a half step off the axis.
int16_t pt_logsin(uint16_t a);

// The screen column of the middle of step u of the view, counted from its
// left edge (view angle u - 128), 0..128: the first column whose centre
// lies right of it.
uint8_t pt_view_col(uint8_t u);

// ---- directions ----
// Multiplies by the sine and cosine of a world angle in 1024ths, each
// product rounded to nearest, from the quarter table (its 1.0 compared,
// never looked up). An edge's frame is two dot products: along its
// direction a, and across it at a + 256, towards its cell's inside.

// The quarter table, sinq, is sin of j 256ths of a right angle at scale
// 256: 0..256. From PT_SIN_ONE on the sine is 256, so a multiply by it
// compares the index first and skips the table and the multiply; below
// that, only index 0 reads 0:
//   CPX #PT_SIN_ONE : BCS one : LDA sinq,X : BEQ zero : ...
// A cosine is sin(256 - a): 256 for a <= 256 - PT_SIN_ONE, and otherwise
// never 0, read one entry on at index a EOR $FF:
//   CPX #256 - PT_SIN_ONE + 1 : BCC one : TXA : EOR #$FF : TAY : LDA sinq + 1,Y
// pt_mul_sin(256, j) reads it back.
#define PT_SIN_ONE 246

// v sin a and v cos a, in v's own fixed point.
int16_t pt_mul_sin(int16_t v, uint16_t a);
int16_t pt_mul_cos(int16_t v, uint16_t a);

// The component of (ux, uy) along direction a: ux cos a + uy sin a.
int16_t pt_dot_dir(int16_t ux, int16_t uy, uint16_t a);

// mag along direction a: (mag cos a, mag sin a). A whole number of units
// shifted up 8 comes back as an exact s.8 step.
pt_xy pt_dir(int16_t mag, uint16_t a);

// ---- unsigned multiplies ----

// a * b via the quarter-square tables; the primitive everything else builds on.
uint16_t pt_mulu8(uint8_t a, uint8_t b);

// (a * f) >> 8, f a u0.8 fraction; exact, and needs no width beyond u16.
uint16_t pt_mulu16_frac(uint16_t a, uint8_t f);

// gradient magnitude (u7.8) times a column count, saturating at 0xFFFF.
uint16_t pt_mulg_u16(uint16_t g, uint8_t dx);

// ---- the camera's posts ----

// The squared distances the posts divide by run over the camera's circle,
// [256, PT_RPOST_END): its radius squared, which camera.h checks.
#define PT_RPOST_END 1600

// 1.25 * 2^15 / d2, a byte: the posts' division and their quarter's
// overcorrection in one. Read at the start of d2's 8 and rounded up, so it
// errs high only, by up to 7%, and the overcorrection is never under a
// quarter.
uint8_t pt_rpost(uint16_t d2);

#endif
