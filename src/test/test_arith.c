// test_arith.c - the table arithmetic against wide-integer references.

#include "arith.h"

#include <math.h>

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
        for (uint32_t b = 0; b < 256; b++) {
            RC_CHECK(pt_mulu8((uint8_t)a, (uint8_t)b), ==, (uint16_t)(a * b));
            RC_CHECK(pt_mulu8_neg((uint8_t)a, (uint8_t)b), ==, (uint16_t)-(uint16_t)(a * b));
        }
}

RC_TEST_STEP(arith, mulu16_frac, fix)
{
    (void)fix;
    static const uint16_t as[] = {0, 1, 2, 255, 256, 257, 4095, 16384, 32767, 65535};
    for (uint32_t i = 0; i < sizeof(as) / sizeof(as[0]); i++)
        for (uint32_t f = 0; f < 256; f++) {
            uint16_t want = (uint16_t)(((uint32_t)as[i] * f) >> 8);
            RC_CHECK(pt_mulu16_frac(as[i], (uint8_t)f), ==, want);
            RC_CHECK(pt_mulu16_frac_neg(as[i], (uint8_t)f), ==, (uint16_t)-want);
        }
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

RC_TEST_STEP(arith, mul_sin, fix)
{
    (void)fix;
    static const int16_t as[] = {-32767, -12345, -256, -1, 0, 1, 255, 256, 30000};
    for (uint32_t i = 0; i < sizeof(as) / sizeof(as[0]); i++) {
        int16_t a = as[i];
        RC_CHECK(pt_mul_sin(a, 0), ==, (int16_t)0);
        RC_CHECK(pt_mul_sin(a, 64), ==, a);
        RC_CHECK(pt_mul_sin(a, 128), ==, (int16_t)0);
        RC_CHECK(pt_mul_sin(a, 192), ==, (int16_t)-a);
        for (uint32_t t = 0; t < 256; t++) {
            // |a| * round(256 sin), rounded half up, with the product's sign
            long s = lround(sin((double)t * (3.14159265358979323846 / 128.0)) * 256.0);
            int32_t ua = a < 0 ? -a : a;
            int32_t m = (int32_t)((ua * (s < 0 ? -s : s) + 128) >> 8);
            int32_t want = (a < 0) != (s < 0) ? -m : m;
            RC_CHECK((int32_t)pt_mul_sin(a, (uint8_t)t), ==, want);
            RC_CHECK(pt_mul_cos(a, (uint8_t)t), ==, pt_mul_sin(a, (uint8_t)(t + 64)));
            // odd symmetry
            RC_CHECK(pt_mul_sin(a, (uint8_t)t), ==, (int16_t)-pt_mul_sin(a, (uint8_t)(256 - t)));
        }
    }
}

RC_TEST_STEP(arith, mac_sc, fix)
{
    (void)fix;
    // the accumulate must agree with the two products summed in two's
    // complement, across every sign quadrant
    static const int16_t us[] = {-30000, -257, -1, 0, 1, 256, 30000};
    for (uint32_t i = 0; i < sizeof(us) / sizeof(us[0]); i++)
        for (uint32_t j = 0; j < sizeof(us) / sizeof(us[0]); j++)
            for (uint32_t ta = 0; ta < 256; ta += 5) {
                pt_sincos a = pt_sin_resolve((uint8_t)ta);
                pt_sincos b = pt_cos_resolve((uint8_t)(ta + 3 * j));
                int16_t want = (int16_t)((int32_t)pt_mul_sc(us[i], a) + pt_mul_sc(us[j], b));
                RC_CHECK(pt_mac_sc(us[i], a, us[j], b), ==, want);
                int16_t wsub = (int16_t)((int32_t)pt_mul_sc(us[i], a) - pt_mul_sc(us[j], b));
                RC_CHECK(pt_msub_sc(us[i], a, us[j], b), ==, wsub);
            }
}

RC_TEST_STEP(arith, lerp16, fix)
{
    (void)fix;
    static const int16_t as[] = {-20000, -100, 0, 50, 20000};
    static const int16_t bs[] = {-20000, -101, 0, 49, 30000};
    for (uint32_t i = 0; i < sizeof(as) / sizeof(as[0]); i++)
        for (uint32_t j = 0; j < sizeof(bs) / sizeof(bs[0]); j++)
            for (uint32_t t = 0; t < 256; t += 17) {
                int32_t a = as[i];
                int32_t b = bs[j];
                int32_t dm = b >= a ? b - a : a - b;
                int32_t off = (int32_t)(((int64_t)dm * t) >> 8);
                int32_t exact = b >= a ? a + off : a - off;
                RC_CHECK((int32_t)pt_lerp16(as[i], bs[j], (uint8_t)t), ==, exact);
            }
}

RC_TEST_STEP(arith, div_t_bounds, fix)
{
    (void)fix;
    static const uint16_t dens[] = {1, 2, 3, 255, 256, 257, 1000, 32767, 65535};
    for (uint32_t i = 0; i < sizeof(dens) / sizeof(dens[0]); i++) {
        uint16_t den = dens[i];
        for (uint32_t k = 0; k <= 8; k++) {
            uint16_t num = (uint16_t)(((uint32_t)den * k) / 8);
            uint32_t got = pt_div_t(num, den);
            uint32_t exact = ((uint32_t)num << 8) / den;
            if (exact > 255)
                exact = 255;
            // reciprocal normalisation loses low bits of large denominators
            int32_t err = (int32_t)got - (int32_t)exact;
            RC_CHECK(err >= -3 && err <= 3, ==, true);
        }
    }
}

RC_TEST_STEP(arith, project_col, fix)
{
    (void)fix;
    static const uint16_t xs[] = {1, 2, 3, 100, 255, 256, 1000, 2500, 8000};
    for (uint32_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        uint16_t x = xs[i];
        for (int32_t k = -8; k <= 8; k++) {
            int16_t y = (int16_t)(((int32_t)x * k) / 8);
            int32_t exact = 64 + (64 * (int32_t)y) / (int32_t)x;
            int32_t got = pt_project_col(y, x);
            RC_CHECK(got >= 0 && got <= 128, ==, true);
            int32_t err = got - exact;
            RC_CHECK(err >= -2 && err <= 2, ==, true);
        }
    }
}

RC_TEST_STEP(arith, project_row, fix)
{
    (void)fix;
    static const int16_t hs[] = {-400, -80, -1, 0, 1, 80, 136, 400};
    for (uint32_t i = 0; i < sizeof(hs) / sizeof(hs[0]); i++) {
        int16_t h = hs[i];
        uint32_t uh = (uint32_t)(h < 0 ? -h : h);
        uint16_t d = (uint16_t)((uh * 128 + PT_SCREEN_HEIGHT - 1) / PT_SCREEN_HEIGHT);
        if (d == 0)
            d = 1;
        for (uint16_t x = d; x < 8000; x = (uint16_t)(x * 2 + 1)) {
            int32_t exact = PT_SCREEN_HEIGHT / 2 + (64 * (int32_t)h) / (int32_t)x;
            int32_t got = pt_project_row(h, x);
            RC_CHECK(got >= 0 && got <= PT_SCREEN_HEIGHT, ==, true);
            int32_t err = got - exact;
            RC_CHECK(err >= -2 && err <= 2, ==, true);
        }
    }
}

RC_TEST_STEP(arith, grad_mag, fix)
{
    (void)fix;
    for (uint32_t dy = 0; dy <= 64; dy += 3)
        for (uint32_t dx = 1; dx <= 128; dx++) {
            uint32_t exact = (dy << 8) / dx;
            uint32_t got = pt_grad_mag((uint8_t)dy, (uint8_t)dx);
            // the byte reciprocal costs up to ~0.2% of the exact value; over
            // a line's own dx-column span that is under a sixth of a row
            int32_t err = (int32_t)got - (int32_t)exact;
            int32_t slack = (int32_t)(exact / 256) + 2;
            RC_CHECK(err >= -slack && err <= slack, ==, true);
        }
}

RC_TEST_STEP(arith, gradq8_mag, fix)
{
    (void)fix;
    static const uint16_t dxs[] = {1, 128, 255, 256, 300, 1000, 16384, 32768};
    for (uint32_t dy = 0; dy <= 64; dy += 7)
        for (uint32_t i = 0; i < sizeof(dxs) / sizeof(dxs[0]); i++) {
            uint32_t exact = (dy << 16) / dxs[i];
            if (exact > 0x7FFF)
                exact = 0x7FFF;
            uint32_t got = pt_gradq8_mag((uint8_t)dy, dxs[i]);
            // normalisation costs up to ~1% of the exact value
            int32_t err = (int32_t)got - (int32_t)exact;
            int32_t slack = (int32_t)(exact / 64) + 2;
            RC_CHECK(err >= -slack && err <= slack, ==, true);
        }
}

RC_TEST_STEP(arith, div_q8, fix)
{
    (void)fix;
    static const uint32_t nums[] = {0, 1, 255, 256, 1600, 65535, 400000, 16777215};
    static const uint16_t dens[] = {1, 3, 64, 255, 256, 900, 1600, 18000, 65535};
    for (uint32_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        for (uint32_t j = 0; j < sizeof(dens) / sizeof(dens[0]); j++) {
            uint64_t exact = ((uint64_t)nums[i] << 8) / dens[j];
            if (exact > 0xFFFF)
                exact = 0xFFFF;
            uint32_t got = pt_div_q8(nums[i], dens[j]);
            // reciprocal normalisation loses low bits of large denominators
            int64_t err = (int64_t)got - (int64_t)exact;
            int64_t slack = (int64_t)(exact / 64) + 2;
            RC_CHECK(err >= -slack && err <= slack, ==, true);
        }
}

RC_TEST_STEP(arith, div_cols, fix)
{
    (void)fix;
    static const uint16_t nums[] = {0, 1, 255, 256, 1000, 16384};
    static const uint16_t dens[] = {1, 3, 255, 256, 300, 16384, 32768};
    for (uint32_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        for (uint32_t j = 0; j < sizeof(dens) / sizeof(dens[0]); j++) {
            uint32_t exact = nums[i] / dens[j];
            if (exact > 127)
                exact = 127;
            uint32_t got = pt_div_cols(nums[i], dens[j]);
            int32_t err = (int32_t)got - (int32_t)exact;
            RC_CHECK(err >= -2 && err <= 2, ==, true);
        }
}
