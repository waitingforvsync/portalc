// arith.h - 6502-friendly fixed-point arithmetic: every multiply, divide,
// angle and log in the engine goes through these helpers, which use only
// tables plus shifts and adds (as the BBC build would).
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

// The angle's ray basis for exact frustum-side tests: k1 = C - S and
// k2 = C + S, where C and S are the angle's cosine and sine at scale 256 --
// exactly the signed values pt_rot's terms round. The ray sums of a vertex
// at world offset (x, y) are then, unrounded and linear in the offset,
//   a * 256 = x * k1 + y * k2        b * 256 = x * k2 - y * k1
// so their signs are exact. Used only by push_root's retry when the rounded
// winding search fails, never in per-vertex code.
void pt_ray_basis(uint8_t angle, int16_t *k1, int16_t *k2);

// ---- angles and logs ----
// A world angle is 1024ths of a turn from +x towards +y, in 10 bits: the top
// two are the quadrant, which the offset's signs give, and the low byte the
// angle within it, so an edge needs only the byte (its other side differs
// by a half turn). A computed angle is the step the true one lies in: its
// middle is angle + 0.5. The camera's 256ths are four steps each. A view
// angle is a world angle less the camera's, and in view when it's in
// [-128, 128): 45 degrees either side. Logs are 256 * log2, in 16 bits.

// sin of j 256ths of a right angle at scale 256: 0..256. From PT_SIN_ONE on
// the sine is 256, so a multiply by it compares the index first and skips
// the table and the multiply; below that, only index 0 reads 0:
//   CPX #PT_SIN_ONE : BCS one : LDA sinq,X : BEQ zero : ...
// A cosine is sin(256 - a): 256 for a <= 256 - PT_SIN_ONE, and otherwise
// never 0, read one entry on at index a EOR $FF:
//   CPX #256 - PT_SIN_ONE + 1 : BCC one : TXA : EOR #$FF : TAY : LDA sinq + 1,Y
#define PT_SIN_ONE 246
uint16_t pt_sinq(uint8_t j);

// 256 * log2(x) for x >= 1, within ~0.2% of x plus the table's half step:
// the 8 bits under x's leading one, rounded, index a fraction table.
uint16_t pt_log2(uint16_t x);

// 2^(l / 256), rounded, saturating at 0xFFFF: pt_log2's inverse, within ~0.2%.
uint16_t pt_exp2(uint16_t l);

// The quadrant of a direction other than (0, 0), from its signs: 0 for +x
// +y, 1 for -x +y, 2 for -x -y, 3 for +x -y. A direction exactly along an
// axis starts its quadrant (+x 0, +y 1, -x 2, -y 3), so a direction and its
// reverse always differ by exactly 2, and an edge's angle byte serves both
// its sides.
uint8_t pt_quadrant(int16_t dx, int16_t dy);

// A world offset in polar form.
typedef struct pt_polar {
    uint16_t angle;    // the world angle (the step it lies in)
    uint16_t lr;       // 256 * log2 of the length
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

// The screen column of view angle v's middle, 0..128: the first column whose
// centre lies right of it.
uint8_t pt_view_col(int8_t v);

// ---- directions ----
// Multiplies by the sine and cosine of a world angle in 1024ths, each
// product rounded to nearest, from the quarter table (its 1.0 compared,
// never looked up). An edge's frame is two dot products: along its
// direction a, and across it at a + 256, towards its cell's inside.

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

// -(a * b) as two's complement: the same two lookups subtracted the other way
// round, so an accumulate can add a negative product without negating.
uint16_t pt_mulu8_neg(uint8_t a, uint8_t b);

// (a * f) >> 8, f a u0.8 fraction; exact, and needs no width beyond u16.
uint16_t pt_mulu16_frac(uint16_t a, uint8_t f);

// -((a * f) >> 8) as two's complement, from the negated-product partials.
uint16_t pt_mulu16_frac_neg(uint16_t a, uint8_t f);

// gradient magnitude (u7.8) times a column count, saturating at 0xFFFF.
uint16_t pt_mulg_u16(uint16_t g, uint8_t dx);

// a + t * (b - a) for a u0.8 parameter t: the comparison picks the direction,
// and the difference is multiplied unsigned.
int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t);

// ---- reciprocal-table divisions ----
// Each is num * 2^k / den for its own k. den is normalised to n in [128, 255]
// with a shift count s, so 2^16 / den ~= (256 + f) * 2^-s, and the quotient
// is num * (256 + f) >> (16 + s - k). The shift is kept as e = s + 8
// (0..15, as the 6502 normaliser returns it), making that
// num * (256 + f) >> (8 + e - k). For a byte numerator the product is one
// lookup plus num in the high byte, exact in 17 bits, so a numerator is
// never widened by pre-shifting; a u16 numerator keeps num * (256 + f) >> 8
// (two lookups, the carry as a 17th bit) and shifts right by e - k from
// there. Error stays within ~0.2% of the result plus a count of truncation.

// A normalised reciprocal: 2^16 / den ~= (256 + f) * 2^(8 - e), den shifted
// into [128, 255] by s = e - 8 places (right shifts truncating, left shifts
// exact). f is a single byte. A power-of-two den normalises to 128, whose
// 2^16/128 = 512 is kept as f = 0 at one shift less, so 256 + f always
// holds.
typedef struct pt_recip {
    uint8_t f;
    uint8_t e;    // s + 8, 0..15
} pt_recip;

// The normalised reciprocal of n > 0: two lookups for a byte, a one-byte
// shift more for a u16. A depth's is formed once and handed to every
// projection at that depth.
pt_recip pt_recip8(uint8_t n);
pt_recip pt_recip16(uint16_t n);

// num / den as a u0.8 clip parameter t, clamped to 255; needs num <= den.
uint8_t pt_div_t(uint16_t num, uint16_t den);

// floor(num / den) for two u8.8 values, clamped to 127: the crossing quotient.
uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8);

// (num << 8) / den, clamped to 0xFFFF; needs den >= 128.
uint16_t pt_div_q8(uint16_t num, uint16_t den);

// (dy << 8) / dx: a gradient magnitude from a row delta and a column count.
uint16_t pt_grad_mag(uint8_t dy, uint8_t dx);

// (dy << 16) / dx_q8: the near clip's fractional-dx gradient magnitude,
// clamped to the u7.8-with-sign-bit range.
uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8);

// ---- projections ----

// floor(64 + 64*y/x): the screen column of an in-frustum vertex (|y| <= x);
// rx is pt_recip16(x).
uint8_t pt_project_col(int16_t y, uint16_t x, pt_recip rx);

// The same projection at depth d kept as u8.8 (the near clip's sx*).
uint16_t pt_project_col_q8(int16_t y, uint16_t d);

// 64*len/x in u8.8, saturating at 0xFFFF: a length's projected size at
// depth x, in screen pixels (a sprite's scale, span and placement). rx is
// pt_recip16(x).
uint16_t pt_project_q8(uint16_t len, uint16_t x, pt_recip rx);

// 64*h/x for a height magnitude h = 8*h8 (heights are quantised to 8-unit
// steps so they fit a byte): the row offset of a line endpoint from the
// screen centre, the caller adding or subtracting by the height's side.
// Needs the endpoint on screen vertically, x >= 128*h/PT_SCREEN_HEIGHT (the
// 64-row screen's classic x >= 2h). rx is pt_recip16(x): one lookup and a
// shift, no normalisation.
uint8_t pt_project_rowd(uint8_t h8, uint16_t x, pt_recip rx);

#endif
