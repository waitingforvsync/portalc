// captureref.c - the capture's answers as JSON, for make test's check of the Beeb's
// (beeb/test/capture.6502). Run as portalc_captureref > capture.json.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "map.h"
#include "places.h"
#include "render.h"
#include "vcache.h"

#define LINES_HEIGHT (17 * 256 + 31)    // the made-up lines' height's log
#define INST_GROUPS 64                  // groups of made-up instance cases, a pair of sums each
#define INST_CASES 16                   // the cases in a group

static pt_map m;
static pt_vcache vc;
static pt_render r;

// A 16-bit Galois LFSR, stepped before each made-up instance value, as test/capture.6502 steps it.
static uint16_t random = 0xACE1;

static uint8_t step(void)
{
    random = (uint16_t)((random >> 1) ^ ((random & 1) ? 0xB400 : 0));
    return (uint8_t)random;
}

// 0 to 64: 0 to 127 made up, less 64 past 64.
static uint8_t up_to_64(void)
{
    uint8_t v = step() & 127;
    return v > 64 ? (uint8_t)(v - 64) : v;
}

// A byte into a pair of Fletcher sums, mod 255 as FLETCHER makes them: a carry out goes back in.
static void fletcher(uint8_t *s1, uint8_t *s2, uint8_t b)
{
    unsigned t = (unsigned)*s1 + b;
    *s1 = (uint8_t)(t > 255 ? t - 255 : t);
    t = (unsigned)*s2 + *s1;
    *s2 = (uint8_t)(t > 255 ? t - 255 : t);
}

static void hex(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        printf("%02X", b[i]);
}

// The frame's instances, as walkref has them: each column's top, and its bottom where it shows.
static void insts(void)
{
    uint8_t head[2] = {r.num_insts, r.num_order};
    hex(head, sizeof head);
    hex(r.order, r.num_order);
    for (unsigned i = 1; i < r.num_insts; i++) {
        const pt_inst *in = &r.insts[i];
        uint8_t f[17] = {in->sprite,
                         (uint8_t)in->place.scale,
                         (uint8_t)(in->place.scale >> 8),
                         (uint8_t)in->place.x,
                         (uint8_t)((uint16_t)in->place.x >> 8),
                         (uint8_t)in->place.y,
                         (uint8_t)((uint16_t)in->place.y >> 8),
                         (uint8_t)in->ld,
                         (uint8_t)((uint16_t)in->ld >> 8),
                         in->cx0,
                         in->cx1,
                         in->p0,
                         in->p1,
                         (uint8_t)in->clip,
                         (uint8_t)(in->clip >> 8),
                         in->cols,
                         in->whole};
        hex(f, sizeof f);
        unsigned w = (unsigned)(in->cx1 - in->cx0);
        for (unsigned c = 0; c < w; c++) {
            const uint8_t *row = &r.clipmem[in->clip + c];
            hex(&row[w], 1);
            if (row[w] != 0)
                hex(row, 1);    // its bottom, only where it shows something
        }
    }
}

static void comma(int *first)
{
    if (!*first)
        printf(",");
    *first = 0;
}

int main(void)
{
    pt_arith_init();
    pt_scene_init(&m);
    pt_vcache_init(&vc);
    pt_render_init(&r, &m, &vc);

    // from each place, at every angle: the frame's setup and root walk, then the eye's cell
    // walked as the walk walks it, from the root side until a side ends at or past the portal's
    // right column, with the ends of each side that has columns and its lines at the cell's
    // ceiling and floor and the eye's height: as test/capture.6502 sums them
    static place at[PT_MAX_PLACES];
    unsigned n = places(&m, at);
    unsigned last = 0;    // frames whose walk reaches the eye's cell's last side, a turn on
    printf("{\n  \"frames\": [");
    int first = 1;
    for (unsigned p = 0; p < n; p++) {
        pt_vcache_reset(&vc, at[p].x, at[p].y);
        for (unsigned a = 0; a < 256; a++) {
            pt_walk w = pt_render_root(&r, (uint8_t)a, at[p].cell);
            const pt_visit *vs = &r.visits[w.vi];
            comma(&first);
            printf("[%u,%u,%u,%u,%u,%d,%d,%u,%u,", w.vi, w.x, w.tmax, w.bmin, w.split_angle, w.u0,
                   w.u1, w.c0, w.c1);
            printf("%u,%u,%u,%u,%u,%d,%u,%d,", vs->cell, vs->side, vs->xr, vs->kept,
                   vs->hc.below, vs->hc.l, vs->hf.below, vs->hf.l);
            printf("%u,%u,%u,%u", r.eye_z, r.num_visits, r.num_kept, r.num_recs);
            for (unsigned i = 0; i < sizeof r.toggle_end; i++)
                printf(",%u", r.toggle_end[i]);
            unsigned turned = 0;
            for (;;) {
                printf(",%u,%d,%d,%u,%u", vs->side, w.u0, w.u1, w.c0, w.c1);
                if (w.c0 < w.c1) {
                    pt_ends e = pt_render_ends(&r, &w);
                    printf(",%d,%d,%d", e.logdepth0, e.logdepth1, e.logcolumns);
                    pt_height hs[3] = {vs->hc, vs->hf, {.below = 1, .l = 0}};
                    for (unsigned k = 0; k < 3; k++) {
                        pt_line L = pt_render_line(&w, &e, hs[k]);
                        printf(",%u,%u,%u", L.x0, L.x1, L.below);
                        if (L.x0 < L.x1)
                            printf(",%u,%u,%u", L.y, L.g, L.rl);
                    }
                }
                turned |= w.u1 >= (int16_t)w.split_angle;
                if (w.c1 >= vs->xr)
                    break;
                pt_render_side_next(&r, &w);
            }
            last += turned;
            printf("]");
            // the visit cache emptied, as a frame does after its walk
            memset(r.cell_visit, 0, sizeof r.cell_visit);
        }
    }
    printf("],\n  \"places\": %u,\n", n);

    fprintf(stderr, "captureref: %u places, %u of %u frames reach the eye's cell's last side\n", n,
            last, n * 256);

    // made-up lines, for the paths the walk never takes: the ends' depths' logs on a grid from
    // -512 in steps of 32, the start's by row and the end's by column, at a height whose log is 31
    // on from a multiple of 32, so the grid meets 13 * 256 - 1, the last offset on the screen; the
    // column also picks the side's columns, 1 to 128, and whether the height's below the eye
    printf("  \"lines\": [");
    first = 1;
    for (unsigned i = 0; i < 256; i++)
        for (unsigned j = 0; j < 256; j++) {
            pt_walk w = {.c0 = 0, .c1 = (uint8_t)(1 + (j >> 1))};
            pt_ends e = {.logdepth0 = (int16_t)(-512 + 32 * (int)i),
                         .logdepth1 = (int16_t)(-512 + 32 * (int)j),
                         .logcolumns = (int16_t)pt_log2_8(w.c1)};
            pt_height h = {.below = (uint8_t)(j & 1), .l = LINES_HEIGHT};
            pt_line L = pt_render_line(&w, &e, h);
            comma(&first);
            printf("%u,%u,%u", L.x0, L.x1, L.below);
            if (L.x0 < L.x1)
                printf(",%u,%u,%u", L.y, L.g, L.rl);
        }
    printf("],\n");

    // every height from every eye height: whether it's below, and the log
    printf("  \"height\": [");
    first = 1;
    for (unsigned eye = 0; eye < 256; eye++)
        for (unsigned z = 0; z < 256; z++) {
            pt_height h = pt_render_height((uint8_t)z, (uint8_t)eye);
            comma(&first);
            printf("%u,%d", h.below, h.l);
        }
    printf("],\n");

    // made-up instances of the first cell's sprite that has one, for the paths the walk never
    // takes, and the sprite pass's draw order. A group's cases share made-up bounds, each column's top then bottom, 0 to 64. A case
    // starts with 0, 192, 384 or 576 of the clip pool's bytes taken, so it fills, and makes 3 to 6
    // instances (6 when they're near), each from a made-up camera: an offset from the sprite, each
    // part a signed byte times the case's 1, 2, 4 or 8; its facing, made up 1 in 4 times, else the
    // sprite's angle from it, as inst_new sees it, give or take a signed byte over 8, 4, 2 or 1
    // (the nearer, the closer to it); and the cell's floor's height as it's seen from an eye a
    // signed byte over 4 (shifted, so rounded down) from it. Then each instance clipped over a
    // made-up span, [x, x + 0 to 127) cut at 128, with made-up extents, and the first hidden over
    // another: each instance made (or 0), then the instances. And the case's sprites drawn into a
    // screen made up from the case's number (each byte its offset's low byte EOR its high EOR the
    // number's low byte), the screens summed here, by group: they'd be 4K a case
    uint8_t cell = 1;
    while (m.cells[cell].sprite == 0)
        cell++;
    const pt_sprite *sp = &m.sprites[m.cells[cell].sprite];
    pt_vertex base = m.verts[sp->v];
    unsigned made = 0, order = 0;
    static uint8_t drawn1[INST_GROUPS], drawn2[INST_GROUPS];
    printf("  \"insts\": [");
    for (unsigned g = 0; g < INST_GROUPS; g++) {
        for (unsigned c = 0; c < PT_SCREEN_WIDTH; c++) {
            r.top[c] = up_to_64();
            r.bot[c] = up_to_64();
        }
        printf("%s\n    \"", g ? "," : "");
        for (unsigned k = 0; k < INST_CASES; k++) {
            r.num_insts = 1;
            r.num_order = 0;
            r.clip_used = (uint16_t)((step() & 3u) * 192);
            uint8_t s = step() & 3;
            unsigned calls = s == 0 ? 6 : (step() & 3u) + 3;
            for (unsigned j = 0; j < calls; j++) {
                int16_t dx = (int16_t)((int8_t)step() * (1 << s));
                int16_t dy = (int16_t)((int8_t)step() * (1 << s));
                pt_vcache_reset(&vc, pt_u16((int32_t)base.x + dx), pt_u16((int32_t)base.y + dy));
                if ((step() & 3) == 0) {
                    uint8_t lo = step();
                    r.cam4 = (uint16_t)(lo | (step() & 3u) << 8);
                } else {
                    uint16_t a = pt_vcache_vert(&vc, &m, sp->v)->angle;
                    int8_t off = (int8_t)step();
                    for (uint8_t i = s; i < 3; i++)
                        off = (int8_t)(off >> 1);    // (CMP #&80 : ROR A)
                    r.cam4 = (uint16_t)((uint16_t)(a + (uint16_t)(int16_t)off) & 1023u);
                }
                uint8_t z = m.cells[cell].floor_z;
                int8_t up = (int8_t)((int8_t)step() >> 2);    // (CMP #&80 : ROR A, twice)
                pt_height hf = pt_render_height(z, (uint8_t)(z + up));
                uint8_t ii = pt_render_inst_new(&r, cell, hf);
                hex(&ii, 1);
                made += ii != 0;
                order += ii != 0 && r.num_order > 1;
            }
            for (uint8_t i = 1; i < r.num_insts; i++) {
                uint8_t xl = step() & 127;
                unsigned xr = xl + (step() & 127u);
                uint8_t tmax = up_to_64();
                pt_render_inst_clip(&r, i, xl, xr > 128 ? (uint8_t)128 : (uint8_t)xr, tmax,
                                    up_to_64());
            }
            if (r.num_insts > 1) {
                uint8_t xl = step() & 127;
                unsigned xr = xl + (step() & 127u);
                pt_render_inst_hide(&r, 1, xl, xr > 128 ? (uint8_t)128 : (uint8_t)xr);
            }
            insts();
            uint8_t number = (uint8_t)(g * INST_CASES + k);
            for (unsigned i = 0; i < sizeof r.fb; i++)
                r.fb[i] = (uint8_t)(i ^ (i >> 8) ^ number);
            pt_render_sprites(&r);
            for (unsigned i = 0; i < sizeof r.fb; i++)
                fletcher(&drawn1[g], &drawn2[g], r.fb[i]);
        }
        printf("\"");
    }
    printf("\n  ],\n  \"drawn1\": \"");
    hex(drawn1, sizeof drawn1);
    printf("\",\n  \"drawn2\": \"");
    hex(drawn2, sizeof drawn2);
    printf("\",\n");

    // a near sprite on a floor far below the eye, the camera 33 to 40 units straight ahead of it
    // (west), facing it, the eye 14 to 21 steps over the floor: an offset past 255 rows, in whole
    // rows, that leaves a sprite this tall on the screen, which the made-up cases all but miss
    printf("  \"deep\": \"");
    for (uint8_t d = 33; d <= 40; d++)
        for (uint8_t dz = 14; dz <= 21; dz++) {
            r.num_insts = 1;
            r.num_order = 0;
            r.clip_used = 0;
            pt_vcache_reset(&vc, (uint16_t)(base.x - d), base.y);
            r.cam4 = pt_vcache_vert(&vc, &m, sp->v)->angle;
            uint8_t z = m.cells[cell].floor_z;
            uint8_t ii = pt_render_inst_new(&r, cell, pt_render_height(z, (uint8_t)(z - dz)));
            hex(&ii, 1);
            insts();
        }
    printf("\"\n}\n");
    fprintf(stderr, "captureref: %u made-up instances made, %u into an order of more\n", made,
            order);
    return 0;
}
