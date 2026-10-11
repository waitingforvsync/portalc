// arithref.c - arith.c's logs and exps as JSON: every answer make test checks the Beeb's
// macros against (beeb/test/arith.6502). Run as portalc_arithref > arith.json.

#include <stdint.h>
#include <stdio.h>

#include "arith.h"

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

int main(void)
{
    pt_arith_init();
    printf("{\n");
    list("log2_8", 256, log2_8, 0);
    list("log2", 65536, log2_16, 0);
    list("exp2", 8 * 256, exp2_8, 1);
    printf("}\n");
    return 0;
}
