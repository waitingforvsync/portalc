// arithref.c - arith.c's logs, exps and polar forms as JSON: every answer make test checks the
// Beeb's macros against (beeb/test/arith.6502). Run as portalc_arithref > arith.json.

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
    printf("  \"polar_bits\": %u\n", bits);
    printf("}\n");
    return 0;
}
