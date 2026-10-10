// tables.c - arith.c's tables as JSON, laid out as the Beeb holds them (a
// 16-bit table as its low bytes, then its high bytes, each a table of its
// own), for beeb/'s check that its loader builds the same.
//
//   portalc_tables > tables.json

#include <stdint.h>
#include <stdio.h>

#include "arith.h"

static int first_ = 1;

static void bytes(const char *name, const uint8_t *b, unsigned n)
{
    printf("%s\n  \"%s\": \"", first_ ? "{" : ",", name);
    for (unsigned i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\"");
    first_ = 0;
}

static void words(const char *name, const uint16_t *w, unsigned n)
{
    uint8_t lo[512], hi[512];
    for (unsigned i = 0; i < n; i++) {
        lo[i] = (uint8_t)w[i];
        hi[i] = (uint8_t)(w[i] >> 8);
    }
    char nm[32];
    snprintf(nm, sizeof nm, "%slo", name);
    bytes(nm, lo, n);
    snprintf(nm, sizeof nm, "%shi", name);
    bytes(nm, hi, n);
}

int main(void)
{
    pt_arith_init();
    static pt_arith_tables t;
    pt_arith_tables_get(&t);
    bytes("nrm", t.nrm, 256);
    bytes("k8", t.k8, 256);
    bytes("logf", t.logf, 256);
    bytes("expf", t.expf, 256);
    bytes("atan", t.atan, 256);
    bytes("lhyp", t.lhyp, 256);
    bytes("sinq", t.sinq, 256);
    bytes("a2c", t.a2c, 256);
    bytes("lsec", t.lsec, 128);
    bytes("lsx", t.lsx, 128);
    words("lsec16", t.lsec16, 256);
    words("sq1", t.sq1, 512);
    words("sq2", t.sq2, 512);
    bytes("rpost", t.rpost, PT_RPOST_END / 8);
    printf("\n}\n");
    return 0;
}
