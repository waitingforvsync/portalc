// arith.c - table builders and the arithmetic helpers. The tables are built
// as the 6502 builds them at start up: the quarter squares by recurrence,
// and every table of a smooth function from a bit a delta (below). The
// helpers stick to lookups, adds, shifts and unsigned compares, with widths
// held to what the 6502 routine would keep.

#include "arith.h"

#include <math.h>
#include <stdbool.h>

#include "mode5.h"
#include "richc/macros.h"

// Quarter-square tables: sq1[i] = i*i/4, sq2[i] = (i-255)^2/4, both floored.
// a*b = sq1[a+b] - sq2[a-b+255], exact because a+b and a-b share parity;
// subtracting the other way round gives -(a*b) directly.
static uint16_t sq1_[512];
static uint16_t sq2_[512];

// Normalising tables, indexed by a byte b >= 1 with lz(b) its leading zero
// bits (entry 0 unused):
//   nrm_[b]   = b << lz(b), the byte shifted up until bit 7 is set
//   k8_[b]    = 8 - lz(b), the byte's bit length (1..8)
// Built an octave at a time: the bytes with bit length k normalise to 128
// on up in steps of 2^(8 - k), the step halving each octave.
static uint8_t nrm_[256];
static uint8_t k8_[256];

// The packed-delta tables, all bytes (see "packed delta tables" below):
//   sinq_[j]  round(256 sin(j/256 of a right angle)), j = 0..255; from
//             PT_SIN_ONE on it's 256, never read (the index is compared)
//   atan_[i]  floor(atan((i + 0.5) / 256)) in 1024ths of a turn, 0..127: the
//             angle of a ratio byte, at the middle of its 1/256
//   lhyp_[i]  round(256 log2 sqrt(1 + ((i + 0.5) / 256)^2)): log2 of the
//             length over the major component, for the same ratio byte
//   logf_[j]  round(256 log2(1 + j / 256)): log2's fraction for a mantissa
//   expf_[f]  round(256 (2^(f / 256) - 1)): 2^x's mantissa for a fraction
//   lsec_[i]  round(-256 log2 cos((i + 0.5) / 1024 of a turn)), i = 0..127:
//             from a length to a depth, at the middle of an angle's step
//             (the first half of lsec16_, below)
//   lsx_[j]   round(-256 log2(sin x / x)), x = (j + 0.5) / 1024 of a turn,
//             j = 0..127: how far a small angle's sine falls short of it
//   a2c_[k]   round(64 + 64 tan((k - 128 + 0.5) / 1024 of a turn)), 0..128:
//             the column of the middle of view angle k - 128
static uint8_t sinq_[256];
static uint8_t atan_[256];
static uint8_t lhyp_[256];
static uint8_t logf_[256];
static uint8_t expf_[256];
static uint8_t lsec_[128];
static uint8_t lsx_[128];
static uint8_t a2c_[256];

// -256 log2 cos at the middle of each step of the right angle, i = 0..255:
// lsec_ up to 45 degrees, then 16 bits (lo and hi bytes on the 6502),
// climbing too fast for a bit a delta, to 2,137 at the last step. Read
// backwards it's -256 log2 sin. Past 45 degrees it's made from logs: the
// sine of a small angle x is x, less sin x / x's shortfall, so
//   -256 log2 sin x = PT_LOG_STEP - 256 log2(2j + 1) + lsx_[j]
// for x the middle of step j, (2j + 1) pi / 1024, and PT_LOG_STEP =
// -256 log2(pi / 1024).
#define PT_LOG_STEP 2137
static uint16_t lsec16_[256];

static bool inited_;

// ---- packed delta tables ----
//
// The 6502 builds a table of a smooth function from its first entry and a
// bit for each delta after it, over a base for each byte of bits (0, 1 or
// 2, from comparing the byte index on the 6502: a run of bases per table).
// The bits come out by ROR through the carry. The first byte holds fewer
// than 8, with a set bit just above them; that bit, carried round into
// each next byte, sits above its 8, so a byte has run out when ROR leaves
// it zero. The bytes are stored last first, so X counts down:
//
//       LDA #first : LDY #0 : LDX #nbytes - 1 : CLC : BCC start
//   .loop
//       ADC #0                      ; the delta's bit
//       CPX #run : ADC #0           ; the byte's base, as runs need
//   .start
//       STA table,Y : INY
//   .next
//       ROR bits,X : BNE loop
//       DEX : BPL next              ; the set bit, in the carry, goes on
//
// The last add of an entry must leave the carry clear, as ROR shifts it
// in: the sine's wrap from 255 to 256 (a stored 0) comes on its delta's
// add, and the base's compare clears it. The bits here come from the
// ideal values, as Baron's FOR loops would make them at assembly time,
// and unpack is the loop above.

typedef struct packed {
    uint8_t bits[32];
    uint8_t base[32];
    uint8_t first;    // the first entry
} packed;

// The deltas of n ideal values (which may run past 255: the table stores
// them modulo 256), a bit each over their byte's base. A byte whose deltas
// fit its predecessor's base keeps it, so the bases run monotonically.
static packed pack(const int32_t *ideal, uint32_t n)
{
    packed p = {.first = (uint8_t)ideal[0]};
    uint32_t k = (n - 1) % 8;    // the first byte's share
    uint32_t nbytes = (n - 1) / 8 + 1;
    RC_ASSERT(nbytes <= 32);
    uint32_t d = 0;
    for (uint32_t b = 0; b < nbytes; b++) {
        uint32_t cnt = b == 0 ? k : 8;
        int32_t lo = 255, hi = 0;
        for (uint32_t j = 0; j < cnt; j++) {
            int32_t dj = ideal[d + j + 1] - ideal[d + j];
            lo = dj < lo ? dj : lo;
            hi = dj > hi ? dj : hi;
        }
        int32_t base = cnt == 0 ? 0 : lo;
        bool prev = b > 1 || (b == 1 && k > 0);    // the byte before holds deltas
        if (prev && p.base[b - 1] <= lo && hi <= p.base[b - 1] + 1)
            base = p.base[b - 1];
        RC_PANIC(cnt == 0 || (base >= 0 && hi - base <= 1));
        p.base[b] = (uint8_t)base;
        uint8_t bits = b == 0 ? (uint8_t)(1u << k) : 0;    // the first byte's set bit
        for (uint32_t j = 0; j < cnt; j++)
            if (ideal[d + j + 1] - ideal[d + j] > base)
                bits |= (uint8_t)(1u << j);
        p.bits[b] = bits;
        d += cnt;
    }
    return p;
}

// The 6502's loop: ROR each byte through the carry until it's empty.
static void unpack(uint8_t *table, uint32_t n, packed p)
{
    uint8_t v = p.first;
    uint32_t y = 0;
    table[y++] = v;
    bool c = false;
    for (uint32_t b = 0; y < n; b++) {
        uint8_t bits = p.bits[b];
        for (;;) {
            bool out = bits & 1;    // ROR bits,X
            bits = (uint8_t)(bits >> 1 | (c ? 0x80 : 0));
            c = out;
            if (bits == 0)
                break;    // c holds the set bit for the next byte
            v = (uint8_t)(v + c + p.base[b]);    // ADC #0, then the base
            c = false;
            table[y++] = v;
        }
    }
    RC_ASSERT(y == n);
}

static void build(uint8_t *table, uint32_t n, double (*f)(uint32_t), bool floored)
{
    int32_t ideal[256];
    for (uint32_t i = 0; i < n; i++)
        ideal[i] = (int32_t)(floored ? floor(f(i)) : floor(f(i) + 0.5));
    unpack(table, n, pack(ideal, n));
}

// 256 log2 x. The leading one's place is the whole part, and the 8 bits
// under it, rounded, index the fraction; a rounding that carries out is the
// next octave's fraction 0. k8 finds the place and nrm lines up the high
// byte's bits under its leading one; the low byte's go under them, shifted
// down k - 1 with LSR A, the last bit out rounding:
//   LDX hi : BEQ small
//   LDA nrm,X : ASL A : STA t        ; hi's bits under its leading one
//   LDY k8,X : LDA lo : CLC
//   DEY : BEQ join                    ; k = 1: lo is the rest, exactly
// .sh LSR A : DEY : BNE sh            ; C is the last bit out
// .join ORA t : ADC #0                ; carry out: the next octave's 0
// Under 256 the bits under the leading one are fewer than 8, so the
// normalised byte, its leading one shifted out, is the fraction exactly.
static uint16_t log2_(uint16_t x)
{
    RC_ASSERT(x > 0);
    uint8_t hi = (uint8_t)(x >> 8);
    uint8_t lo = (uint8_t)x;
    if (hi == 0)
        return (uint16_t)((k8_[lo] - 1) << 8 | logf_[(uint8_t)(nrm_[lo] << 1)]);
    uint8_t k = k8_[hi];
    uint8_t c = 0;
    for (uint8_t n = (uint8_t)(k - 1); n != 0; n--) {
        c = lo & 1;
        lo = (uint8_t)(lo >> 1);
    }
    uint16_t j = (uint16_t)(((uint8_t)(nrm_[hi] << 1) | lo) + c);
    if (j == 256)
        return (uint16_t)((8 + k) << 8);
    return (uint16_t)((7 + k) << 8 | logf_[j]);
}

#define PI 3.14159265358979323846

static double f_sinq(uint32_t j) { return 256.0 * sin(j * (PI / 512.0)); }
static double f_atan(uint32_t i) { return atan((i + 0.5) / 256.0) * (512.0 / PI); }
static double f_lhyp(uint32_t i) { return 128.0 * log2(1.0 + ((i + 0.5) / 256.0) * ((i + 0.5) / 256.0)); }
static double f_logf(uint32_t j) { return 256.0 * log2(1.0 + j / 256.0); }
static double f_expf(uint32_t f) { return 256.0 * (exp2(f / 256.0) - 1.0); }
static double f_lsec(uint32_t i) { return -256.0 * log2(cos((i + 0.5) * (PI / 512.0))); }
static double f_lsx(uint32_t j)
{
    double x = (j + 0.5) * (PI / 512.0);
    return -256.0 * log2(sin(x) / x);
}
static double f_a2c(uint32_t k) { return 64.0 + 64.0 * tan(((double)k - 128 + 0.5) * (PI / 512.0)); }

void pt_arith_init(void)
{
    if (inited_)
        return;

    // f(x + 1) = f(x) + floor((x + 1) / 2) for f(x) = x*x/4 floored; sq2 is
    // sq1 mirrored about 255
    sq1_[0] = 0;
    for (uint32_t i = 0; i < 511; i++)
        sq1_[i + 1] = (uint16_t)(sq1_[i] + (i + 1) / 2);
    for (uint32_t i = 0; i < 512; i++)
        sq2_[i] = sq1_[i >= 255 ? i - 255 : 255 - i];

    build(sinq_, 256, f_sinq, false);
    build(atan_, 256, f_atan, true);
    build(lhyp_, 256, f_lhyp, false);
    build(logf_, 256, f_logf, false);
    build(expf_, 256, f_expf, false);
    build(lsec_, 128, f_lsec, false);
    build(lsx_, 128, f_lsx, false);
    build(a2c_, 256, f_a2c, false);

    // an octave at a time: b = 2^(k-1) .. 2^k - 1 normalise to 128, 128 +
    // step, ..., the step 2^(8 - k); the last octave's end wraps b to 0
    //   LDX #1 : LDY #1 : LDA #128 : STA step
    // .octave
    //   LDA #128
    // .entry
    //   STA nrm,X : PHA : TYA : STA k8,X : PLA
    //   INX : CLC : ADC step : BCC entry
    //   INY : LSR step : CPX #0 : BNE octave
    uint8_t b = 1;
    uint8_t step = 128;
    for (uint8_t k = 1; k <= 8; k++, step >>= 1) {
        uint16_t v = 128;
        do {
            nrm_[b] = (uint8_t)v;
            k8_[b] = k;
            b++;
            v = (uint16_t)(v + step);
        } while (v < 256);
    }
    RC_ASSERT(b == 0);

    // lsec16_: lsec_, then from logs (see above), for which log2_ needs
    // logf_, nrm_ and k8_
    for (uint32_t i = 0; i < 128; i++)
        lsec16_[i] = lsec_[i];
    for (uint32_t j = 0; j < 128; j++)
        lsec16_[255 - j] = (uint16_t)(PT_LOG_STEP - log2_((uint16_t)(2 * j + 1)) + lsx_[j]);

    inited_ = true;
}

// ---- angles and logs ----

uint16_t pt_sinq(uint8_t j)
{
    RC_ASSERT(inited_);
    return j >= PT_SIN_ONE ? 256 : sinq_[j];
}

uint16_t pt_log2(uint16_t x)
{
    RC_ASSERT(inited_);
    return log2_(x);
}

uint16_t pt_exp2(uint16_t l)
{
    RC_ASSERT(inited_);

    // 1.f in 9 bits, shifted by the whole part less 8: left exactly, or
    // right with the last shift's carry rounding
    uint8_t n = (uint8_t)(l >> 8);
    uint16_t m = (uint16_t)(256 + expf_[l & 0xFF]);
    if (n >= 16)
        return 0xFFFF;
    if (n >= 8)
        return (uint16_t)(m << (n - 8));
    return (uint16_t)(((m >> (7 - n)) + 1) >> 1);
}

uint8_t pt_quadrant(int16_t dx, int16_t dy)
{
    // the signs, as LDA dy+1 : ASL A : EOR dx+1 : ROL A : PHP : ROL A :
    // AND #3 has them (bit 1 dy's, bit 0 dx's EOR dy's), except that a zero
    // counts as negative where that puts an axis at its quadrant's start: dx
    // when dy is positive, dy when dx is negative (a zero test each)
    bool nx = dx < 0 || (dx == 0 && dy > 0);
    bool ny = dy < 0 || (dy == 0 && dx < 0);
    return (uint8_t)(ny << 1 | (nx != ny));
}

pt_polar pt_polar_of(int16_t dx, int16_t dy)
{
    RC_ASSERT(inited_);
    RC_ASSERT((dx != 0 || dy != 0) && dx != INT16_MIN && dy != INT16_MIN);

    uint8_t q = pt_quadrant(dx, dy);
    bool odd = q & 1;
    uint16_t ux = (uint16_t)(dx < 0 ? -dx : dx);
    uint16_t uy = (uint16_t)(dy < 0 ? -dy : dy);

    // the minor component over the major as a byte, from the logs'
    // difference: 2^8 * minor / major, 256 meaning equal. The major's log
    // is the length's too, less a factor
    bool steep = uy > ux;
    uint16_t maj = steep ? uy : ux;
    uint16_t mnr = steep ? ux : uy;
    uint16_t lmaj = pt_log2(maj);
    uint16_t r = 0;
    if (mnr != 0) {
        int32_t l = 8 * 256 + pt_log2(mnr) - lmaj;
        r = l < 0 ? 0 : pt_exp2((uint16_t)l);
    }
    uint8_t phi = r == 256 ? 128 : atan_[r];
    uint8_t lh = r == 256 ? 128 : lhyp_[r];

    // phi runs from the major axis: within the quadrant, from its first
    // axis or its second, as steep and odd differ. 255 - phi rather than
    // 256 - phi: atan_ floors, so both octants hold the step the angle is in
    uint8_t a = (steep != odd) ? (uint8_t)~phi : phi;
    return (pt_polar) {.angle = (uint16_t)(q << 8 | a), .lr = (uint16_t)(lmaj + lh)};
}

uint8_t pt_lsec(int8_t v)
{
    RC_ASSERT(inited_);
    // a negative step's middle is |v| - 0.5 from straight ahead: ~v
    return lsec_[v < 0 ? (uint8_t)~v : (uint8_t)v];
}

uint16_t pt_lsec16(uint8_t i)
{
    RC_ASSERT(inited_);
    return lsec16_[i];
}

int16_t pt_logsin(uint16_t a)
{
    RC_ASSERT(inited_);
    // sin over the half turn is symmetric about the right angle: step j's
    // middle mirrors step 511 - j's, and sin of step j is cos of 255 - j
    uint16_t j = a & 511;
    if (j >= 256)
        j = 511 - j;
    return (int16_t)-lsec16_[255 - j];
}

uint8_t pt_view_col(int8_t v)
{
    RC_ASSERT(inited_);
    return a2c_[(uint8_t)v ^ 0x80];
}

// ---- directions ----

static uint16_t mulu16_frac_round(uint16_t a, uint8_t f);

// The sine of a 10-bit angle at scale 256, as a magnitude (0..256) and a
// sign. The quadrant picks the index, j or (where the sine runs down from
// 1, in quadrants 1 and 3) 256 - j, and a sign for the lower half turn.
// The indices that are 1.0 are compared, never looked up.
static uint16_t sin_mag(uint16_t a, bool *neg)
{
    uint8_t q = (uint8_t)((a >> 8) & 3);
    uint8_t j = (uint8_t)a;
    *neg = q >= 2;
    if (q & 1) {
        if (j < 256 - PT_SIN_ONE + 1)
            return 256;
        return sinq_[(uint8_t)(~j + 1)];    // (j EOR $FF) + 1 = 256 - j
    }
    return j >= PT_SIN_ONE ? 256 : sinq_[j];
}

int16_t pt_mul_sin(int16_t v, uint16_t a)
{
    RC_ASSERT(inited_);
    RC_ASSERT(v != INT16_MIN);
    bool neg;
    uint16_t s = sin_mag(a, &neg);
    uint16_t uv = (uint16_t)(v < 0 ? -v : v);
    uint16_t p = s == 256 ? uv : s == 0 ? 0 : mulu16_frac_round(uv, (uint8_t)s);
    return (v < 0) != neg ? (int16_t)-p : (int16_t)p;
}

int16_t pt_mul_cos(int16_t v, uint16_t a)
{
    return pt_mul_sin(v, (uint16_t)(a + 256));
}

int16_t pt_dot_dir(int16_t ux, int16_t uy, uint16_t a)
{
    return (int16_t)(pt_mul_cos(ux, a) + pt_mul_sin(uy, a));
}

pt_xy pt_dir(int16_t mag, uint16_t a)
{
    return (pt_xy) {.x = pt_mul_cos(mag, a), .y = pt_mul_sin(mag, a)};
}

uint16_t pt_mulu8(uint8_t a, uint8_t b)
{
    RC_ASSERT(inited_);
    return (uint16_t)(sq1_[(uint32_t)a + b] - sq2_[(uint32_t)a - b + 255]);
}

uint16_t pt_mulu16_frac(uint16_t a, uint8_t f)
{
    // (al*f)>>8 discards only bits the true >>8 also discards, so this is
    // exact, and the sum cannot carry out of 16 bits
    uint16_t lo = (uint16_t)(pt_mulu8((uint8_t)a, f) >> 8);
    uint16_t hi = pt_mulu8((uint8_t)(a >> 8), f);
    return (uint16_t)(lo + hi);
}

uint16_t pt_mulg_u16(uint16_t g, uint8_t dx)
{
    uint16_t hi = pt_mulu8((uint8_t)(g >> 8), dx);
    if (hi > 0xFF)
        return 0xFFFF;
    uint16_t base = (uint16_t)(hi << 8);
    uint16_t s = (uint16_t)(base + pt_mulu8((uint8_t)g, dx));
    return s < base ? 0xFFFF : s;    // carry out: saturate
}

// As pt_mulu16_frac, but rounded to nearest: add 0x80 to the low partial
// before its shift (one add-with-carry on the 6502).
static uint16_t mulu16_frac_round(uint16_t a, uint8_t f)
{
    uint16_t lo = (uint16_t)((uint16_t)(pt_mulu8((uint8_t)a, f) + 128) >> 8);
    uint16_t hi = pt_mulu8((uint8_t)(a >> 8), f);
    return (uint16_t)(lo + hi);
}

