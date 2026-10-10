// test_arith.c - the table arithmetic against wide-integer references.

#include "arith.h"

#include <math.h>
#include <stdlib.h>

#include "mode5.h"
#include "richc/test.h"

RC_TEST_GROUP_DATA(arith) {
    int unused;
};

RC_TEST_GROUP_INIT(arith, fix)
{
    (void)fix;
    pt_arith_init();
}

RC_TEST_GROUP_DEINIT(arith, fix)
{
    (void)fix;
}

RC_TEST_STEP(arith, mulu8_exhaustive, fix)
{
    (void)fix;
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t b = 0; b < 256; b++)
            RC_CHECK(pt_mulu8((uint8_t)a, (uint8_t)b), ==, (uint16_t)(a * b));
}

RC_TEST_STEP(arith, mulu16_frac, fix)
{
    (void)fix;
    static const uint16_t as[] = {0, 1, 2, 255, 256, 257, 4095, 16384, 32767, 65535};
    for (uint32_t i = 0; i < sizeof(as) / sizeof(as[0]); i++)
        for (uint32_t f = 0; f < 256; f++)
            RC_CHECK(pt_mulu16_frac(as[i], (uint8_t)f), ==, (uint16_t)(((uint32_t)as[i] * f) >> 8));
}

RC_TEST_STEP(arith, mulg_u16, fix)
{
    (void)fix;
    static const uint16_t gs[] = {0, 1, 255, 256, 257, 300, 16384, 32767};
    for (uint32_t i = 0; i < sizeof(gs) / sizeof(gs[0]); i++)
        for (uint32_t dx = 0; dx < 128; dx++) {
            uint32_t exact = (uint32_t)gs[i] * dx;
            uint16_t want = exact > 0xFFFF ? 0xFFFF : (uint16_t)exact;
            RC_CHECK(pt_mulg_u16(gs[i], (uint8_t)dx), ==, want);
        }
}

// The table's sine of t in 256ths of a turn, as a wide reference.
static int32_t ref_sin(uint32_t t)
{
    return (int32_t)lround(sin((double)(t & 255) * (3.14159265358979323846 / 128.0)) * 256.0);
}

// Directions in 1024ths: every angle's sine and cosine products against
// floating point, the facing step exactly, and a frame's two dot products.
RC_TEST_STEP(arith, directions, fix)
{
    (void)fix;
    static const int16_t vs[] = {-30000, -2048, -257, -1, 0, 1, 255, 256, 3000, 30000};
    for (uint32_t i = 0; i < sizeof(vs) / sizeof(vs[0]); i++)
        for (uint32_t a = 0; a < 1024; a++) {
            double th = a * (2 * 3.14159265358979323846 / 1024.0);
            double s = lround(sin(th) * 256.0) / 256.0;    // the table's sine
            double c = lround(cos(th) * 256.0) / 256.0;
            RC_CHECK(fabs(pt_mul_sin(vs[i], (uint16_t)a) - vs[i] * s), <=, 0.5);
            RC_CHECK(fabs(pt_mul_cos(vs[i], (uint16_t)a) - vs[i] * c), <=, 0.5);
        }

    // a whole number of units along a camera angle is exact in s.8
    for (uint32_t t = 0; t < 256; t++)
        for (int16_t mag = 0; mag < 128; mag += 7) {
            pt_xy f = pt_dir((int16_t)(mag << 8), (uint16_t)(t << 2));
            RC_CHECK(f.x, ==, (int16_t)(mag * ref_sin(t + 64)));
            RC_CHECK(f.y, ==, (int16_t)(mag * ref_sin(t)));
        }

    // the frame: a vector along a direction dots to its length along it,
    // and to nothing across it
    for (uint32_t a = 0; a < 1024; a += 3) {
        pt_xy v = pt_dir(1000, (uint16_t)a);
        RC_CHECK(abs(pt_dot_dir(v.x, v.y, (uint16_t)a) - 1000), <=, 8);
        RC_CHECK(abs(pt_dot_dir(v.x, v.y, (uint16_t)(a + 256))), <=, 2);
    }
}

// ---- angles and logs ----

#define PI 3.14159265358979323846

// The packed tables, through their accessors, against the formulas their
// bits were packed from: unpacking is the 6502's loop, so this checks the
// packing and the loop together.
RC_TEST_STEP(arith, packed_tables, fix)
{
    (void)fix;
    for (uint32_t j = 0; j < 256; j++) {
        uint16_t want = (uint16_t)lround(256.0 * sin(j * (PI / 512.0)));
        RC_CHECK(pt_mul_sin(256, (uint16_t)j), ==, (int16_t)want);    // the table, read back
        RC_CHECK(want == 256, ==, j >= PT_SIN_ONE);    // where the sine reaches 1
    }
    for (int32_t v = -128; v < 128; v++) {
        double mid = v < 0 ? -v - 0.5 : v + 0.5;
        RC_CHECK(pt_lsec((int8_t)v), ==, (uint8_t)lround(-256.0 * log2(cos(mid * (PI / 512.0)))));
        RC_CHECK(pt_view_col((uint8_t)(v + 128)), ==,
                 (uint8_t)lround(64.0 + 64.0 * tan((v + 0.5) * (PI / 512.0))));
    }
}

// lsec over the whole right angle, its second half made from logs, and log
// sine, which is it read backwards.
RC_TEST_STEP(arith, lsec16_logsin, fix)
{
    (void)fix;
    for (uint32_t i = 0; i < 128; i++)
        RC_CHECK(pt_lsec16((uint8_t)i), ==, pt_lsec((int8_t)i));
    double worst = 0;
    for (uint32_t i = 0; i < 256; i++) {
        double want = -256.0 * log2(cos((i + 0.5) * (PI / 512.0)));
        worst = fmax(worst, fabs(pt_lsec16((uint8_t)i) - want));
    }
    RC_CHECK(worst, <, 1.2);    // three roundings: the log's, lsx's, the constant's
    for (uint32_t a = 0; a < 1024; a++) {
        double want = 256.0 * log2(fabs(sin((a + 0.5) * (PI / 512.0))));
        RC_CHECK(fabs(pt_logsin((uint16_t)a) - want), <, 1.2);
    }
}

RC_TEST_STEP(arith, log2_exhaustive, fix)
{
    (void)fix;
    double worst = 0;
    for (uint32_t x = 1; x < 65536; x++) {
        double e = fabs(pt_log2((uint16_t)x) - 256.0 * log2(x));
        worst = e > worst ? e : worst;
    }
    RC_CHECK(worst, <, 1.2);    // a 9-bit mantissa's rounding, and the table's
    RC_CHECK(pt_log2(1), ==, 0);
    RC_CHECK(pt_log2(256), ==, 8 * 256);
    RC_CHECK(pt_log2(65535), ==, 16 * 256);    // rounds into the next octave
}

RC_TEST_STEP(arith, log2_8, fix)
{
    (void)fix;
    for (uint32_t b = 1; b < 256; b++)
        RC_CHECK(pt_log2_8((uint8_t)b), ==, pt_log2((uint16_t)b));
}

RC_TEST_STEP(arith, exp2, fix)
{
    (void)fix;
    double worst = 0;
    for (uint32_t l = 0; l < 4096; l++) {
        double t = exp2(l / 256.0);
        if (t >= 512)
            worst = fmax(worst, fabs(pt_exp2((uint16_t)l) - t) / t);
        RC_CHECK(fabs(pt_exp2((uint16_t)l) - t), <=, 0.5 + t * 0.0025);    // rounding to a whole number too
    }
    RC_CHECK(worst, <, 0.0025);
    RC_CHECK(pt_exp2(0), ==, 1);
    RC_CHECK(pt_exp2(8 * 256), ==, 256);
    RC_CHECK(pt_exp2(16 * 256), ==, 0xFFFF);
    // a round trip lands within its errors
    for (uint32_t x = 512; x < 65536; x += 97)
        RC_CHECK(fabs((double)pt_exp2(pt_log2((uint16_t)x)) - x), <=, x * 0.005);
}

// The angle's step middle against atan2, and the length's log against
// hypot, over a grid out to the map's size and close round the eye.
static void polar_check(int32_t lim, int32_t sx, int32_t sy, double max_angle, double max_lr)
{
    double wa = 0;
    double wl = 0;
    for (int32_t dx = -lim; dx <= lim; dx += sx)
        for (int32_t dy = -lim; dy <= lim; dy += sy) {
            if (dx == 0 && dy == 0)
                continue;
            pt_polar p = pt_polar_of((int16_t)dx, (int16_t)dy);
            double t = atan2(dy, dx) * (512.0 / PI);
            double e = fmod(p.angle + 0.5 - t + 1024.0 + 512.0, 1024.0) - 512.0;
            wa = fmax(wa, fabs(e));
            wl = fmax(wl, fabs(p.lr - 256.0 * log2(hypot(dx, dy))));
        }
    RC_CHECK(wa, <, max_angle);
    RC_CHECK(wl, <, max_lr);
}

RC_TEST_STEP(arith, polar, fix)
{
    (void)fix;
    polar_check(2000, 3, 7, 1.4, 2.5);
    polar_check(40, 1, 1, 1.4, 2.5);

    // the axes start their quadrants, and the diagonals are their middles
    // (atan_ floors, so 255 - phi is the step below a quadrant's end)
    RC_CHECK(pt_polar_of(100, 0).angle, ==, 0);
    RC_CHECK(pt_polar_of(0, 100).angle, ==, 256);
    RC_CHECK(pt_polar_of(-100, 0).angle, ==, 512);
    RC_CHECK(pt_polar_of(0, -100).angle, ==, 768);
    RC_CHECK(pt_polar_of(100, 100).angle, ==, 128);
    RC_CHECK(pt_polar_of(-100, 100).angle, ==, 256 + 127);
    RC_CHECK(pt_polar_of(-100, -100).angle, ==, 512 + 128);
    RC_CHECK(pt_polar_of(100, -100).angle, ==, 768 + 127);

    // a direction and its reverse are two quadrants apart, axes included
    for (int32_t dx = -3; dx <= 3; dx++)
        for (int32_t dy = -3; dy <= 3; dy++)
            if (dx != 0 || dy != 0)
                RC_CHECK((pt_quadrant((int16_t)dx, (int16_t)dy) + 2) & 3, ==,
                         pt_quadrant((int16_t)-dx, (int16_t)-dy));
}
