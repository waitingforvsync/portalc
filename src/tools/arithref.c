// arithref.c - arith.c's logs, exps, polar forms, sines' multiplies and fractions as JSON: every
// answer make test checks the Beeb's macros against (beeb/test/arith.6502). Run as
// portalc_arithref > arith.json.

#include <stdint.h>
#include <stdio.h>

#include "arith.h"
#include "map.h"

static void list(const char *name, unsigned n, uint16_t (*f)(unsigned), int last)
{
    printf("  \"%s\": [", name);
    for (unsigned i = 0; i < n; i++)
        printf(i == 0 ? "%u" : ",%u", (unsigned)f(i));
    printf("]%s\n", last ? "" : ",");
}

static uint16_t log2_8(unsigned b) { return b == 0 ? 0 : pt_log2_8((uint8_t)b); }
static uint16_t log2_16(unsigned x) { return x == 0 ? 0 : pt_log2((uint16_t)x); }
static uint16_t exp2_8(unsigned l) { return pt_exp2((uint16_t)l); }

// The bits of the longest half-unit offset across the map, as beeb/src/map.6502 has them.
static unsigned offset_bits(const pt_map *m)
{
    int32_t x0 = 0xFFFF, x1 = 0, y0 = 0xFFFF, y1 = 0;
    for (unsigned i = 0; i < m->num_verts; i++) {
        pt_vertex v = m->verts[i];
        x0 = v.x < x0 ? v.x : x0;
        x1 = v.x > x1 ? v.x : x1;
        y0 = v.y < y0 ? v.y : y0;
        y1 = v.y > y1 ? v.y : y1;
    }
    int32_t span = x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0;
    unsigned bits = 0;
    while ((1 << bits) <= 2 * span + 1)
        bits++;
    return bits;
}

// POLAR's answers over a square of points, offset first + step * i from the camera both ways, a
// row of x a y: pt_polar_of of the half-unit offset, its lr less 256, as pt_vcache_vert has it.
static void polar(const char *name, int32_t first, int32_t step)
{
    printf("  \"%s\": [", name);
    for (int32_t j = 0; j < 256; j++)
        for (int32_t i = 0; i < 256; i++) {
            int16_t dx = pt_i16(2 * (first + step * i) - 1);
            int16_t dy = pt_i16(2 * (first + step * j) - 1);
            pt_polar p = pt_polar_of(dx, dy);
            printf(i == 0 && j == 0 ? "%u,%d" : ",%u,%d", (unsigned)p.angle, p.lr - 256);
        }
    printf("],\n");
}

// The sines' multiplies' made-up values, as test/arith_macros.6502 makes them: a * 0x79B9 + i *
// 0x9E37, wrapping 16 bits, 0x8000 taken as 0x7FFF (pt_mul_sin's v is never -32768).
static int16_t made_up(unsigned a, unsigned i)
{
    uint16_t v = (uint16_t)(a * 0x79B9u + i * 0x9E37u);
    return (int16_t)(v == 0x8000 ? 0x7FFF : v);
}

// At every 10-bit angle, 128 answers: of n << 8 for each n under 128 (UNITS_SINE), of the made-up
// values (MULTIPLY_SINE), or the made-up values quartered and quartered again EOR 0x5555, shifted
// signed, along it (DOT_DIRECTION); each as 16 bits.
static void sines(const char *name, int kind, int cosine)
{
    printf("  \"%s\": [", name);
    for (unsigned a = 0; a < 1024; a++)
        for (unsigned i = 0; i < 128; i++) {
            int16_t r;
            if (kind == 0) {
                int16_t v = (int16_t)(i << 8);
                r = cosine ? pt_mul_cos(v, (uint16_t)a) : pt_mul_sin(v, (uint16_t)a);
            } else if (kind == 1) {
                int16_t v = made_up(a, i);
                r = cosine ? pt_mul_cos(v, (uint16_t)a) : pt_mul_sin(v, (uint16_t)a);
            } else {
                int16_t v = made_up(a, i);
                int16_t ux = (int16_t)(v >> 2);
                int16_t uy = (int16_t)((int16_t)(v ^ 0x5555) >> 2);
                r = pt_dot_dir(ux, uy, (uint16_t)a);
            }
            printf(a == 0 && i == 0 ? "%u" : ",%u", (unsigned)(uint16_t)r);
        }
    printf("],\n");
}

// At every fraction f, 128 answers of pt_mulu16_frac: of f * 0x3C6F + i * 0x5BD1, wrapping.
static void fractions(void)
{
    printf("  \"mulu16_frac\": [");
    for (unsigned f = 0; f < 256; f++)
        for (unsigned i = 0; i < 128; i++) {
            uint16_t a = (uint16_t)(f * 0x3C6Fu + i * 0x5BD1u);
            printf(f == 0 && i == 0 ? "%u" : ",%u", (unsigned)pt_mulu16_frac(a, (uint8_t)f));
        }
    printf("],\n");
}

int main(void)
{
    static pt_map m;
    pt_arith_init();
    pt_scene_init(&m);
    unsigned bits = offset_bits(&m);

    printf("{\n");
    list("log2_8", 256, log2_8, 0);
    list("log2", 65536, log2_16, 0);
    list("exp2", 8 * 256, exp2_8, 0);
    polar("polar_near", -128, 1);
    polar("polar_far", -(1 << (bits - 1)) + 1, 1 << (bits - 8));
    sines("units_sin", 0, 0);
    sines("units_cos", 0, 1);
    sines("mul_sin", 1, 0);
    sines("mul_cos", 1, 1);
    sines("dot_dir", 2, 0);
    fractions();
    printf("  \"polar_bits\": %u\n", bits);
    printf("}\n");
    return 0;
}
