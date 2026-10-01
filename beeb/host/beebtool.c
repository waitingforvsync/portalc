// beebtool.c - the Beeb port's C side (PLAN.md sections 3.4, 3.5 and 5.4).
// The whole engine in one translation unit, so its tables and static
// helpers are all in reach without a second copy of any of them.
//
//   beebtool export DIR               Baron source for the port: the maps
//                                     (demo, seven-sprite, stress), shades, sine
//                                     table and constants
//   beebtool tables FILE              the fixed tables as the Beeb holds
//                                     them, &300-&EFF
//   beebtool expect MAP FILE          the map data the Beeb should hold,
//                                     as JSON arrays keyed by label
//   beebtool goldens DIR              the pose and input streams
//   beebtool render MAP POSES OUT [poison]
//                                     render a pose stream to frames (4K
//                                     each); "poison" puts an RTI (&40)
//                                     in nrm_[0], as the Beeb does
//   beebtool s2 FN RANDOM START COUNT check S2's expected CRC (test/s2.6502):
//                                     crc, cases used, the last outputs, the
//                                     final counter or generator state
//   beebtool camera MAP INPUTS OUT [poison]
//                                     run an input stream through the
//                                     camera from the map's start: x, y
//                                     (u16.8), angle, cell per tick
//   beebtool s6|s7 MAP POSES OUT [N DUMP]
//                                     check S6's (sprites off) or S7's (on)
//                                     expected records (test/s6.6502); pose
//                                     N's capture and image into DUMP
//   beebtool search DIR N             (the search build) N random poses on
//                                     each map; those that reach code the
//                                     goldens in DIR miss go to
//                                     DIR/searchM_poses.bin
//   beebtool record DIR               (the trace build) unit-replay records
//                                     from the golden poses, to DIR/replay
//   beebtool synthclose DIR SEED N    (the trace build) N synthetic close_columns
//                                     records (rare splits), to DIR/replay
//   beebtool replaycheck DIR          (the trace build) replay every record
//                                     on the host: each must reproduce itself
//   beebtool cost DIR                 (the trace build) the frame-cost model
//                                     over the golden poses
//
// MAP is 0 (the demo scene), 7 (the demo scene with seven sprites) or 9
// (the stress map, below).
// A pose record is 6 bytes: x (u16 LE), y (u16 LE), angle, cell.

#ifdef BEEBTOOL_TRACE
#include "arith_traced.c"    // host/trace.py's copies, counters and wrappers in
#else
#include "../../src/arith.c"
#endif
#include "../../src/palette.c"
#include "../../src/mode5.c"
#include "../../src/sprite.c"
#include "../../src/map.c"
#include "../../src/camera.c"
#ifdef BEEBTOOL_TRACE
#include "render_traced.c"
#else
#include "../../src/render.c"
#endif
#include "../../src/test/testmaps.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "richc/random.h"

static pt_map map_;
static pt_render render_;

static FILE *open_out(const char *dir, const char *name)
{
    char path[1024];
    if (dir)
        snprintf(path, sizeof path, "%s/%s", dir, name);
    else
        snprintf(path, sizeof path, "%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "beebtool: cannot write %s\n", path);
        exit(1);
    }
    return f;
}

// ---- the stress map (MAP 9) ----
//
// For the paths the demo scene never takes (PLAN.md 3.4, 5.5). A squat
// corridor of 17 soft-joined cells runs up +x into a tall, deep room Q,
// whose far side is three portals to little cells R1..R3 (x up the page,
// y right, as the demo scene):
//
//   +            +------------+           +
//   | R1  \      |    R3      |     / R2  |     R1 behind a-b, R3 behind
//   |            b------------c           |     b-c, R2 behind c-d
//   |         /                 \         |
//   |     /           Q             \     |
//   |  /                               \  |
//   a                                     d
//   +-------------------------------------+
//   |              corridor               |     cells 16..0, 0 the wedge
//
// - From corridor cell 0 a line of sight crosses more portals than
//   PT_MAX_DEPTH: the depth cap closes the rest.
// - R1's floor, 48 above the corridor's eye, hides behind the corridor's
//   lintel (32 above) where a-b is close enough behind it: closed columns
//   from the floor line, at a-b's near end, the left.
// - R2's ceiling, 160 below the eye, hides under the corridor's floor edge
//   (80 below) the same way: closed columns from the ceiling line, at c-d's
//   near end, the right.
// - R3 is a slot 8 units tall: far off, a sliver for rounding to pinch.
// - The corridor's near end is a wedge (65 degrees at the tip), where the
//   camera's slide wedges and the move is blocked.
// - An obelisk in Q, taller than the corridor's lintel lets through, and a
//   one-unit one down the corridor, too small to scale from far off.
//
// Built with map.c's own builder (this file includes it), so it passes
// pt_map_validate like the demo scene.

#define STRESS_X0 256
#define STRESS_Y0 1024
#define STRESS_LEN 48
#define STRESS_WIDTH 192
#define STRESS_CELLS 17
#define STRESS_START_X (STRESS_X0 + 24)
#define STRESS_START_Y (STRESS_Y0 + STRESS_WIDTH / 2)
#define STRESS_START_ANGLE 128    // facing the wedge

static void stress_scene_init(pt_map *m)
{
    builder b = begin_map(m);
    uint8_t lv[STRESS_CELLS + 1];
    uint8_t rv[STRESS_CELLS + 1];
    for (uint32_t i = 0; i <= STRESS_CELLS; i++) {
        uint16_t x = (uint16_t)(STRESS_X0 + i * STRESS_LEN);
        lv[i] = add_vert(&b, x, STRESS_Y0);
        rv[i] = add_vert(&b, x, STRESS_Y0 + STRESS_WIDTH);
    }
    uint16_t xq = STRESS_X0 + STRESS_CELLS * STRESS_LEN;    // the room's near wall
    uint8_t qa = add_vert(&b, xq + 24, STRESS_Y0);
    uint8_t qb = add_vert(&b, xq + 120, STRESS_Y0 + 64);
    uint8_t qc = add_vert(&b, xq + 120, STRESS_Y0 + 128);
    uint8_t qd = add_vert(&b, xq + 24, STRESS_Y0 + STRESS_WIDTH);
    uint8_t r1 = add_vert(&b, xq + 168, STRESS_Y0);
    uint8_t r3l = add_vert(&b, xq + 168, STRESS_Y0 + 64);
    uint8_t r3r = add_vert(&b, xq + 168, STRESS_Y0 + 128);
    uint8_t r2 = add_vert(&b, xq + 168, STRESS_Y0 + STRESS_WIDTH);
    uint8_t tip = add_vert(&b, STRESS_X0 - 150, STRESS_Y0 + STRESS_WIDTH / 2);

    static const char *const obelisk_rows[] = {
        "0110", "0110", "1221", "1221", "1221", "1221", "1221", "1221",
        "1221", "1221", "1221", "1221", "1221", "1221", "1221", "1221",
        "1331", "1331", "1331", "1331", "1331", "1331", "1331", "1331",
        "1331", "1331", "1331", "1331", "1331", "1331", "3333", "3333",
        NULL,
    };
    static const uint8_t obelisk_pal[7] = {PT_SH_KKKK, PT_SH_BBRR, PT_SH_RRGG, PT_SH_KKKK,
                                           PT_SH_KKKK, PT_SH_KKKK, PT_SH_KKKK};
    uint8_t obelisk = add_art(&b, obelisk_rows, obelisk_pal);

    uint8_t wall_l = PT_EDGE_COLOUR(PT_SH_BBRR);
    uint8_t wall_r = PT_EDGE_COLOUR(PT_SH_KBBB);
    uint8_t lit = PT_EDGE_COLOUR(PT_SH_RRGG);
    uint8_t dim = PT_EDGE_COLOUR(PT_SH_KKBB);

    // the corridor: floor 480, eye 400, ceiling 368; clockwise from above:
    // far left, far right, near right, near left
    for (uint32_t i = 0; i < STRESS_CELLS; i++) {
        begin_cell(&b, 480, 368, PT_SH_KRRR, PT_SH_KKKB);
        if (i == 14)
            add_sprite(&b, (uint16_t)(STRESS_X0 + 14 * STRESS_LEN + STRESS_LEN / 2),
                       STRESS_Y0 + STRESS_WIDTH / 2, 1, obelisk);
        add_edge(&b, lv[i + 1], i + 1 < STRESS_CELLS ? PT_EDGE_SOFT : lit);
        add_edge(&b, rv[i + 1], wall_r);
        if (i == 0) {
            add_edge(&b, rv[0], wall_r);    // the wedge
            add_edge(&b, tip, wall_l);
        } else {
            add_edge(&b, rv[i], PT_EDGE_SOFT);
        }
        add_edge(&b, lv[i], wall_l);
    }

    // Q: a lofty ceiling, a deep floor
    begin_cell(&b, 600, 336, PT_SH_KKGG, PT_SH_KKKB);
    add_sprite(&b, xq + 60, STRESS_Y0 + STRESS_WIDTH / 2, 256, obelisk);
    add_edge(&b, lv[STRESS_CELLS], wall_l);
    add_edge(&b, qa, lit);    // to R1
    add_edge(&b, qb, dim);    // to R3
    add_edge(&b, qc, lit);    // to R2
    add_edge(&b, qd, wall_r);
    add_edge(&b, rv[STRESS_CELLS], dim);    // back to the corridor

    // R1: a high shelf, 16 tall, floor 48 above the corridor's eye
    begin_cell(&b, 352, 336, PT_SH_RRRR, PT_SH_KKKB);
    add_edge(&b, qa, wall_l);
    add_edge(&b, r1, wall_r);
    add_edge(&b, qb, dim);

    // R3: a slot 8 tall, just above the corridor's eye
    begin_cell(&b, 392, 384, PT_SH_BBBB, PT_SH_KKKB);
    add_edge(&b, qb, wall_l);
    add_edge(&b, r3l, lit);
    add_edge(&b, r3r, wall_r);
    add_edge(&b, qc, dim);

    // R2: a crawlspace in the pit, its ceiling 160 below the corridor's eye
    begin_cell(&b, 600, 560, PT_SH_KKGG, PT_SH_RRRR);
    add_edge(&b, qc, wall_l);
    add_edge(&b, r2, wall_r);
    add_edge(&b, qd, dim);

    end_map(&b);
}

// The camera's start: the demo scene's, or the stress map's own.
static void start_pose(int which, uint16_t *x, uint16_t *y, uint8_t *angle)
{
    *x = which == 9 ? STRESS_START_X : PT_CAM_START_X;
    *y = which == 9 ? STRESS_START_Y : PT_CAM_START_Y;
    *angle = which == 9 ? STRESS_START_ANGLE : PT_CAM_START_ANGLE;
}

static void load_map(int which)
{
    pt_arith_init();
    if (which == 9) {
        stress_scene_init(&map_);
        return;
    }
    pt_scene_init(&map_);
    if (which == 7 && !pt_testmaps_add_sprites(&map_)) {
        fprintf(stderr, "beebtool: the seven-sprite map is invalid\n");
        exit(1);
    }
}

// ---- the map as Baron source ----

static void equb(FILE *f, const char *label, const uint8_t *v, uint32_t n)
{
    fprintf(f, ".%s", label);
    for (uint32_t i = 0; i < n; i++)
        fprintf(f, "%s%3u", i % 16 == 0 ? "\n    EQUB " : ", ", v[i]);
    fprintf(f, "\n");
}

// The bytes of an art block as the Beeb holds it: the palette, then the
// encoded data up to and including its final 0.
static uint32_t art_bytes(const pt_sprite_art *a, uint8_t *out)
{
    // the last column's runs end at the final 0
    uint32_t n = a->data[2 + pt_sprite_width(a) - 1];
    while ((a->data[++n] >> 3) != 0)
        ;
    memcpy(out, a->pal, 7);
    memcpy(out + 7, a->data, n + 1);
    return 7 + n + 1;
}

// Every array of the map, as the Beeb holds it: parallel byte arrays.
typedef struct field {
    const char *label;
    uint8_t v[256];
    uint32_t n;
} field;

static uint32_t map_fields(const pt_map *m, field *fs)
{
    uint32_t k = 0;
#define F(name, count, expr)                     \
    do {                                         \
        fs[k].label = name;                      \
        fs[k].n = (count);                       \
        for (uint32_t i = 0; i < (count); i++)   \
            fs[k].v[i] = (uint8_t)(expr);        \
        k++;                                     \
    } while (0)
    F("vx_lo", m->num_verts, m->verts[i].x & 255);
    F("vx_hi", m->num_verts, m->verts[i].x >> 8);
    F("vy_lo", m->num_verts, m->verts[i].y & 255);
    F("vy_hi", m->num_verts, m->verts[i].y >> 8);
    F("c_first", m->num_cells, m->cells[i].first_edge);
    F("c_num", m->num_cells, m->cells[i].num_edges);
    F("c_floorz", m->num_cells, m->cells[i].floor_z);
    F("c_ceilz", m->num_cells, m->cells[i].ceil_z);
    F("c_floor", m->num_cells, m->cells[i].floor);
    F("c_ceil", m->num_cells, m->cells[i].ceil);
    F("c_sprite", m->num_cells, m->cells[i].sprite);
    F("e_v0", m->num_edges, m->edges[i].v0);
    F("e_adj", m->num_edges, m->edges[i].adj);
    F("e_twin", m->num_edges, m->edges[i].twin);
    F("e_colour", m->num_edges, m->edges[i].colour);
    F("e_angle", m->num_edges, m->edges[i].angle);
    F("e_len8", m->num_edges, m->edges[i].len8);
    F("s_x_lo", m->num_sprites, m->sprites[i].x & 255);
    F("s_x_hi", m->num_sprites, m->sprites[i].x >> 8);
    F("s_y_lo", m->num_sprites, m->sprites[i].y & 255);
    F("s_y_hi", m->num_sprites, m->sprites[i].y >> 8);
    F("s_size_lo", m->num_sprites, m->sprites[i].size & 255);
    F("s_size_hi", m->num_sprites, m->sprites[i].size >> 8);
    F("s_art", m->num_sprites, m->sprites[i].art);
#undef F
    for (uint8_t a = 0; a < m->num_arts; a++) {
        static char names[PT_MAX_ARTS][16];
        snprintf(names[a], sizeof names[a], "art_%u", a);
        fs[k].label = names[a];
        fs[k].n = art_bytes(&m->arts[a], fs[k].v);
        k++;
    }
    return k;
}

static void export_map(const char *dir, const char *file, int which)
{
    load_map(which);
    const pt_map *m = &map_;
    FILE *f = open_out(dir, file);
    fprintf(f, "\\ %s - generated by beebtool export: do not edit.\n", file);
    fprintf(f, "\\ The %s as parallel byte arrays (index 0 of cells and sprites\n",
            which == 9 ? "stress map"
            : which == 7 ? "demo scene with seven sprites" : "demo scene");
    fprintf(f, "\\ unused), and the sprite art: palette (7 shades), then the encoded data.\n\n");
    fprintf(f, "MAP_NUM_VERTS = %u\nMAP_NUM_CELLS = %u\nMAP_NUM_EDGES = %u\n", m->num_verts,
            m->num_cells, m->num_edges);
    fprintf(f, "MAP_NUM_SPRITES = %u\nMAP_NUM_ARTS = %u\n", m->num_sprites, m->num_arts);
    uint16_t sx, sy;
    uint8_t sa;
    start_pose(which, &sx, &sy, &sa);
    fprintf(f, "MAP_START_X = %u\nMAP_START_Y = %u\nMAP_START_ANGLE = %u\n", sx, sy, sa);
    fprintf(f, "MAP_START_CELL = %u\n\n", pt_map_find_cell(m, sx, sy));
    static field fs[40];
    uint32_t n = map_fields(m, fs);
    for (uint32_t i = 0; i < n; i++)
        equb(f, fs[i].label, fs[i].v, fs[i].n);
    fprintf(f, ".art_lo\n    EQUB ");
    for (uint8_t a = 0; a < m->num_arts; a++)
        fprintf(f, "%sLO(art_%u)", a ? ", " : "", a);
    fprintf(f, "\n.art_hi\n    EQUB ");
    for (uint8_t a = 0; a < m->num_arts; a++)
        fprintf(f, "%sHI(art_%u)", a ? ", " : "", a);
    fprintf(f, "\n");
    fclose(f);
}

static void export_all(const char *dir)
{
    export_map(dir, "map0.6502", 0);
    export_map(dir, "map7.6502", 7);
    export_map(dir, "map9.6502", 9);

    FILE *f = open_out(dir, "shades.6502");
    fprintf(f, "\\ shades.6502 - generated by beebtool export: do not edit.\n");
    fprintf(f, "\\ The shade catalogue (palette.c): even and odd scanline patterns.\n\n");
    uint8_t e[PT_NUM_SHADES];
    uint8_t o[PT_NUM_SHADES];
    for (uint8_t i = 0; i < PT_NUM_SHADES; i++) {
        e[i] = pt_shade(i).e;
        o[i] = pt_shade(i).o;
    }
    fprintf(f, "NUM_SHADES = %u\n", PT_NUM_SHADES);
    equb(f, "shade_e", e, PT_NUM_SHADES);
    equb(f, "shade_o", o, PT_NUM_SHADES);
    fclose(f);

    f = open_out(dir, "sine.6502");
    fprintf(f, "\\ sine.6502 - generated by beebtool export: do not edit.\n");
    fprintf(f, "\\ sin of [0, pi) in u0.8, 128 entries, a stored 0 meaning 256\n");
    fprintf(f, "\\ (arith.c's sin_, built with lround(sin), so exported, not recomputed).\n");
    fprintf(f, "\\ Included into tables.6502 at &B00, so no label here.\n");
    for (uint32_t i = 0; i < 128; i++)
        fprintf(f, "%s%3u", i % 16 == 0 ? "\n    EQUB " : ", ", sin_[i]);
    fprintf(f, "\n");
    fclose(f);

    f = open_out(dir, "consts.6502");
    fprintf(f, "\\ consts.6502 - generated by beebtool export: do not edit.\n");
    fprintf(f, "\\ The engine's constants, from the C headers.\n\n");
#define C(name) fprintf(f, "%-20s = %d\n", #name, (int)(name))
    C(PT_SCREEN_WIDTH);
    C(PT_SCREEN_HEIGHT);
    C(PT_CAM_HEIGHT);
    C(PT_CAM_RADIUS);
    C(PT_CAM_MOVE_SPEED);
    C(PT_CAM_TURN_SPEED);
    C(PT_CAM_START_X);
    C(PT_CAM_START_Y);
    C(PT_CAM_START_ANGLE);
    C(PT_SPRITE_NEAR);
    C(PT_POOL_SIZE);
    C(PT_MAX_DEPTH);
    C(PT_MAX_CLOSED);
    C(PT_MAX_VISITS);
    C(PT_MAX_INSTS);
    C(PT_SEG_POOL);
    C(PT_MAX_PEND);
    C(PT_MAX_TOGS);
    C(PT_VF_ANEG);
    C(PT_VF_BNEG);
    C(PT_VF_AZERO);
    C(PT_VF_BZERO);
    C(PT_SLOT_CEIL);
    C(PT_SLOT_CEIL_BOUND);
    C(PT_SLOT_FLOOR_BOUND);
    C(PT_SLOT_FLOOR);
    C(PT_NUM_SLOTS);
    C(PT_IN_FWD);
    C(PT_IN_BACK);
    C(PT_IN_LEFT);
    C(PT_IN_RIGHT);
    C(PT_IN_TURNL);
    C(PT_IN_TURNR);
#undef C
    fclose(f);
}

// ---- the tables and the map as the Beeb should hold them ----

// &300-&EFF, as tables.6502 lays it out: four quarter-square tables of 512
// (low and high bytes of sq1_ and sq2_), the sine table and the reciprocal
// fractions sharing a page, then k8, nrm and sbyte a page each. nrm's
// unused entry 0 holds &40, an RTI: the NMI vector enters RAM at &D00.
static void tables(const char *file)
{
    pt_arith_init();
    static uint8_t img[0xC00];
    for (uint32_t i = 0; i < 512; i++) {
        img[0x000 + i] = (uint8_t)sq1_[i];
        img[0x200 + i] = (uint8_t)(sq1_[i] >> 8);
        img[0x400 + i] = (uint8_t)sq2_[i];
        img[0x600 + i] = (uint8_t)(sq2_[i] >> 8);
    }
    memcpy(img + 0x800, sin_, 128);
    memcpy(img + 0x880, recipf_, 128);
    memcpy(img + 0x900, k8_, 256);
    memcpy(img + 0xA00, nrm_, 256);
    img[0xA00] = 0x40;
    memcpy(img + 0xB00, sbyte_, 256);
    FILE *f = open_out(NULL, file);
    fwrite(img, 1, sizeof img, f);
    fclose(f);
}

static void expect(int which, const char *file)
{
    load_map(which);
    static field fs[40];
    uint32_t n = map_fields(&map_, fs);
    FILE *f = open_out(NULL, file);
    fprintf(f, "{\n");
    for (uint32_t i = 0; i < n; i++) {
        fprintf(f, "  \"%s\": [", fs[i].label);
        for (uint32_t k = 0; k < fs[i].n; k++)
            fprintf(f, "%s%u", k ? ", " : "", fs[i].v[k]);
        fprintf(f, "]%s\n", i + 1 < n ? "," : "");
    }
    fprintf(f, "}\n");
    fclose(f);
}

// ---- golden streams ----

static void put_pose(FILE *f, uint16_t x, uint16_t y, uint8_t angle, uint8_t cell)
{
    uint8_t rec[6] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), angle, cell};
    fwrite(rec, 1, 6, f);
}

static void centroid(uint8_t cell, uint16_t *x, uint16_t *y)
{
    const pt_cell *c = &map_.cells[cell];
    uint32_t sx = 0;
    uint32_t sy = 0;
    for (uint8_t k = 0; k < c->num_edges; k++) {
        sx += map_.verts[map_.edges[c->first_edge + k].v0].x;
        sy += map_.verts[map_.edges[c->first_edge + k].v0].y;
    }
    *x = (uint16_t)(sx / c->num_edges);
    *y = (uint16_t)(sy / c->num_edges);
}

// The sweep's input for a tick: its LCG (sweep.c, probe_collision.c).
static uint8_t lcg_input(unsigned *seed, uint8_t input)
{
    *seed = *seed * 1664525u + 1013904223u;
    if (((*seed >> 16) & 15) == 0) {
        unsigned r = *seed >> 20;
        input = (uint8_t)(PT_IN_FWD | ((r & 1) ? PT_IN_TURNL : 0) | ((r & 2) ? PT_IN_TURNR : 0)
                          | ((r & 4) ? PT_IN_LEFT : 0) | ((r & 8) ? PT_IN_BACK : 0));
    }
    return input;
}

// The eye on a cell's rim, where the root portal's rounded search can fail
// (spec 7.4): on every vertex and at every edge's midpoint of cells 1 to
// last, facing along and across the edge each side of it, and at every 16th
// angle as well.
static void rim_poses(FILE *f, uint8_t last)
{
    for (uint8_t cell = 1; cell <= last; cell++) {
        const pt_cell *c = &map_.cells[cell];
        for (uint8_t k = 0; k < c->num_edges; k++) {
            uint8_t e = (uint8_t)(c->first_edge + k);
            pt_vertex v0 = map_.verts[map_.edges[e].v0];
            pt_vertex v1 = map_.verts[map_.edges[pt_map_ring_next(c, e)].v0];
            pt_vertex at[2] = {v0, {(uint16_t)((v0.x + v1.x) / 2), (uint16_t)((v0.y + v1.y) / 2)}};
            for (uint32_t i = 0; i < 2; i++) {
                if (!pt_map_cell_contains(&map_, cell, at[i].x, at[i].y))
                    continue;    // a midpoint rounded outside
                for (int32_t d = -2; d <= 2; d++)
                    for (uint32_t q = 0; q < 4; q++)
                        put_pose(f, at[i].x, at[i].y,
                                 (uint8_t)(map_.edges[e].angle + 64 * q + d), cell);
                for (uint32_t a = 0; a < 256; a += 16)
                    put_pose(f, at[i].x, at[i].y, (uint8_t)a, cell);
            }
        }
    }
}

static void goldens(const char *dir)
{
    // the sweep: every cell's centroid at 32 angles, then every 8th tick of
    // its camera walk (tools/sweep.c)
    load_map(0);
    FILE *f = open_out(dir, "sweep_poses.bin");
    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        uint16_t x;
        uint16_t y;
        centroid(cell, &x, &y);
        for (uint32_t a = 0; a < 256; a += 8)
            put_pose(f, x, y, (uint8_t)a, cell);
    }
    pt_camera cam;
    pt_camera_init(&cam, &map_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);
    unsigned seed = 12345;
    uint8_t input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        input = lcg_input(&seed, input);
        pt_camera_tick(&cam, &map_, input);
        if ((tick & 7) == 0)
            put_pose(f, (uint16_t)(cam.x >> 8), (uint16_t)(cam.y >> 8), cam.angle, cam.cell);
    }
    fclose(f);

    // the seven-sprite map's poses: every cell's centroid at 64 angles, then
    // every 4th tick of test_sprite.c's random walk
    load_map(7);
    f = open_out(dir, "map7_poses.bin");
    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        uint16_t x;
        uint16_t y;
        centroid(cell, &x, &y);
        for (uint32_t a = 0; a < 256; a += 4)
            put_pose(f, x, y, (uint8_t)a, cell);
    }
    pt_camera_init(&cam, &map_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);
    rc_random rng = rc_random_make(4242);
    input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        if ((rc_random_next(&rng) & 15) == 0) {
            uint32_t rr = rc_random_next(&rng);
            input = (uint8_t)(PT_IN_FWD | ((rr & 1) ? PT_IN_TURNL : 0) | ((rr & 2) ? PT_IN_TURNR : 0)
                              | ((rr & 4) ? PT_IN_LEFT : 0) | ((rr & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, &map_, input);
        if ((tick & 3) == 0)
            put_pose(f, (uint16_t)(cam.x >> 8), (uint16_t)(cam.y >> 8), cam.angle, cam.cell);
    }
    fclose(f);

    // the stress map: a grid of points in every corridor cell and in Q, at
    // 32 angles; then the rim poses as for the demo scene below
    load_map(9);
    f = open_out(dir, "stress_poses.bin");
    for (uint8_t cell = 1; cell <= STRESS_CELLS + 1; cell++) {
        const pt_cell *c = &map_.cells[cell];
        uint16_t x0 = 0xFFFF, x1 = 0, y0 = 0xFFFF, y1 = 0;
        for (uint8_t k = 0; k < c->num_edges; k++) {
            pt_vertex v = map_.verts[map_.edges[c->first_edge + k].v0];
            x0 = v.x < x0 ? v.x : x0;
            x1 = v.x > x1 ? v.x : x1;
            y0 = v.y < y0 ? v.y : y0;
            y1 = v.y > y1 ? v.y : y1;
        }
        for (uint32_t i = 1; i <= 3; i++) {
            for (uint32_t j = 1; j <= 3; j++) {
                uint16_t x = (uint16_t)(x0 + (x1 - x0) * i / 4);
                uint16_t y = (uint16_t)(y0 + (y1 - y0) * j / 4);
                if (!pt_map_cell_contains(&map_, cell, x, y))
                    continue;
                for (uint32_t a = 0; a < 256; a += 8)
                    put_pose(f, x, y, (uint8_t)a, cell);
            }
        }
    }
    rim_poses(f, STRESS_CELLS + 1);
    fclose(f);

    // the demo scene's rims
    load_map(0);
    f = open_out(dir, "rim_poses.bin");
    rim_poses(f, map_.num_cells - 1);
    fclose(f);

    // input streams, one byte a tick from the start pose: the sweep's LCG
    // for 40000 ticks (the sweep's walk is its first 4000, the collision
    // probe all of them), and every one of the 64 key combinations
    f = open_out(dir, "lcg_inputs.bin");
    seed = 12345;
    input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 40000; tick++) {
        input = lcg_input(&seed, input);
        fputc(input, f);
    }
    fclose(f);
    f = open_out(dir, "combo_inputs.bin");
    uint32_t s = 2463534242u;    // xorshift32: portable, and the Beeb could run it too
    uint8_t held = 0;
    for (uint32_t tick = 0; tick < 40000; tick++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if ((s & 7) == 0)
            held = (uint8_t)((s >> 8) & 63);    // a new combination now and then
        fputc(held, f);
    }
    fclose(f);
}

// ---- rendering and the camera ----

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "beebtool: cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*n ? *n : 1);
    if (fread(buf, 1, *n, f) != *n)
        exit(1);
    fclose(f);
    return buf;
}

static void poison(int on)
{
    pt_arith_init();
    nrm_[0] = on ? 0x40 : 0;
}

static void render(int which, const char *poses, const char *out, int poisoned)
{
    load_map(which);
    poison(poisoned);
    pt_render_init(&render_, &map_);
    size_t n;
    uint8_t *p = read_file(poses, &n);
    FILE *f = open_out(NULL, out);
    for (size_t i = 0; i + 6 <= n; i += 6) {
        pt_render_frame(&render_, (uint16_t)(p[i] | p[i + 1] << 8),
                        (uint16_t)(p[i + 2] | p[i + 3] << 8), p[i + 4], p[i + 5]);
        fwrite(render_.fb, 1, PT_FB_BYTES, f);
    }
    fclose(f);
    free(p);
}

static void camera(int which, const char *inputs, const char *out, int poisoned)
{
    load_map(which);
    poison(poisoned);
    size_t n;
    uint8_t *in = read_file(inputs, &n);
    FILE *f = open_out(NULL, out);
    uint16_t sx, sy;
    uint8_t sa;
    start_pose(which, &sx, &sy, &sa);
    pt_camera cam;
    pt_camera_init(&cam, &map_, sx, sy, sa);
    for (size_t i = 0; i < n; i++) {
        pt_camera_tick(&cam, &map_, in[i]);
        uint8_t rec[8] = {(uint8_t)cam.x, (uint8_t)(cam.x >> 8), (uint8_t)(cam.x >> 16),
                          (uint8_t)cam.y, (uint8_t)(cam.y >> 8), (uint8_t)(cam.y >> 16),
                          cam.angle, cam.cell};
        fwrite(rec, 1, 8, f);
    }
    fclose(f);
    free(in);
}

// ---- the pose search (PLAN.md 5.5) ----
//
// Built with clang's -fsanitize-coverage=trace-pc-guard: every basic block
// the compiler instruments has a guard, and the first time one runs it
// flags the frame as fresh. The golden streams run first, so only what
// they miss counts; then random poses, biased to rims and vertices where
// the rounding cases live, and every pose that reaches a new block is kept.

#ifdef BEEBTOOL_SEARCH

static bool fresh_;
static uint32_t blocks_;
static void *fresh_pc_[64];    // the new blocks' addresses, for a report
static uint32_t num_fresh_pc_;

__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard_init(uint32_t *start,
                                                                                 uint32_t *stop)
{
    for (uint32_t *g = start; g < stop; g++)
        *g = 1;
}

__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    if (!*guard)
        return;
    *guard = 0;
    fresh_ = true;
    blocks_++;
    if (num_fresh_pc_ < 64)
        fresh_pc_[num_fresh_pc_++] = __builtin_return_address(0);
}

static void search_seed(const char *dir)
{
    static const struct {
        int map;
        const char *file;
    } streams[] = {{0, "sweep_poses.bin"}, {7, "map7_poses.bin"}, {0, "rim_poses.bin"},
                   {9, "stress_poses.bin"}};
    char path[1024];
    for (uint32_t i = 0; i < sizeof streams / sizeof streams[0]; i++) {
        load_map(streams[i].map);
        poison(0);
        pt_render_init(&render_, &map_);
        snprintf(path, sizeof path, "%s/%s", dir, streams[i].file);
        size_t n;
        uint8_t *p = read_file(path, &n);
        for (size_t k = 0; k + 6 <= n; k += 6)
            pt_render_frame(&render_, (uint16_t)(p[k] | p[k + 1] << 8),
                            (uint16_t)(p[k + 2] | p[k + 3] << 8), p[k + 4], p[k + 5]);
        free(p);
    }
}

// A random pose in a random cell: anywhere, near a rim, or near a vertex.
static bool search_pose(rc_random *rng, uint16_t *x, uint16_t *y, uint8_t *angle, uint8_t *cell)
{
    uint32_t r = rc_random_next(rng);
    uint8_t c = (uint8_t)(1 + rc_random_next(rng) % (map_.num_cells - 1));
    const pt_cell *pc = &map_.cells[c];
    if (pc->floor_z - PT_CAM_HEIGHT <= pc->ceil_z)
        return false;    // no headroom (the stress map's R cells): the eye would be above the ceiling
    uint8_t k = (uint8_t)(rc_random_next(rng) % pc->num_edges);
    uint8_t e = (uint8_t)(pc->first_edge + k);
    pt_vertex v0 = map_.verts[map_.edges[e].v0];
    pt_vertex v1 = map_.verts[map_.edges[pt_map_ring_next(pc, e)].v0];
    int32_t px;
    int32_t py;
    switch (r % 5) {
    case 0: {    // anywhere: a random blend of two ring vertices and a third
        pt_vertex w = map_.verts[map_.edges[pc->first_edge
                                            + rc_random_next(rng) % pc->num_edges].v0];
        uint32_t t = rc_random_next(rng) % 257;
        uint32_t u = rc_random_next(rng) % 257;
        px = (int32_t)(v0.x + ((int32_t)(v1.x - v0.x) * (int32_t)t) / 256);
        py = (int32_t)(v0.y + ((int32_t)(v1.y - v0.y) * (int32_t)t) / 256);
        px += ((int32_t)(w.x - px) * (int32_t)u) / 256;
        py += ((int32_t)(w.y - py) * (int32_t)u) / 256;
        break;
    }
    case 1: {    // on or just inside an edge
        uint32_t t = rc_random_next(rng) % 257;
        px = (int32_t)(v0.x + ((int32_t)(v1.x - v0.x) * (int32_t)t) / 256);
        py = (int32_t)(v0.y + ((int32_t)(v1.y - v0.y) * (int32_t)t) / 256);
        px += (int32_t)(rc_random_next(rng) % 7) - 3;
        py += (int32_t)(rc_random_next(rng) % 7) - 3;
        break;
    }
    case 2: {    // exactly on an axis-aligned edge's line, inside or beyond it
        if (v0.x != v1.x && v0.y != v1.y)
            return false;
        int32_t t = (int32_t)(rc_random_next(rng) % 512) - 128;
        px = v0.x + ((int32_t)(v1.x - v0.x) * t) / 256;
        py = v0.y + ((int32_t)(v1.y - v0.y) * t) / 256;
        break;
    }
    case 3:    // on or by a vertex
        px = v0.x + (int32_t)(rc_random_next(rng) % 5) - 2;
        py = v0.y + (int32_t)(rc_random_next(rng) % 5) - 2;
        break;
    default: {    // a vertex just inside the near-clip depth, looked at
        uint8_t dir = (uint8_t)rc_random_next(rng);
        int32_t dist = 4 + (int32_t)(rc_random_next(rng) % 160);
        pt_xy o = pt_rot((int16_t)dist, 0, (uint8_t)-dir);
        px = v0.x - o.x;
        py = v0.y - o.y;
        if (px < 0 || py < 0 || px > 0xFFFF || py > 0xFFFF)
            return false;
        if (!pt_map_cell_contains(&map_, c, (uint16_t)px, (uint16_t)py))
            return false;
        *x = (uint16_t)px;
        *y = (uint16_t)py;
        *cell = c;
        *angle = (uint8_t)(dir + (int32_t)(rc_random_next(rng) % 65) - 32);
        return true;
    }
    }
    if (px < 0 || py < 0 || px > 0xFFFF || py > 0xFFFF)
        return false;
    if (!pt_map_cell_contains(&map_, c, (uint16_t)px, (uint16_t)py))
        return false;
    *x = (uint16_t)px;
    *y = (uint16_t)py;
    *cell = c;
    // half the time a facing aligned with the edge or a ray along it
    uint32_t a = rc_random_next(rng);
    *angle = (a & 1) ? (uint8_t)(map_.edges[e].angle + 32 * ((a >> 1) & 7)
                                 + (int32_t)((a >> 4) % 5) - 2)
                     : (uint8_t)(a >> 8);
    return true;
}

static void search(const char *dir, uint32_t n)
{
    search_seed(dir);
    num_fresh_pc_ = 0;
    printf("seeded: %u blocks\n", blocks_);
    static const int maps[] = {0, 7, 9};
    for (uint32_t i = 0; i < 3; i++) {
        load_map(maps[i]);
        poison(0);
        pt_render_init(&render_, &map_);
        char name[32];
        snprintf(name, sizeof name, "search%d_poses.bin", maps[i]);
        FILE *f = open_out(dir, name);
        rc_random rng = rc_random_make(1000 + (uint32_t)maps[i]);
        uint32_t kept = 0;
        for (uint32_t tries = 0; tries < n; tries++) {
            uint16_t x, y;
            uint8_t angle, cell;
            if (!search_pose(&rng, &x, &y, &angle, &cell))
                continue;
            fresh_ = false;
            num_fresh_pc_ = 0;
            pt_render_frame(&render_, x, y, angle, cell);
            if (fresh_) {
                uint32_t npc = num_fresh_pc_;
                put_pose(f, x, y, angle, cell);
                kept++;
                fprintf(stderr, "map %d pose %u %u %u %u:", maps[i], x, y, angle, cell);
                for (uint32_t k = 0; k < npc; k++)
                    fprintf(stderr, " %p", fresh_pc_[k]);
                fprintf(stderr, "\n");
            }
        }
        fclose(f);
        printf("map %d: kept %u poses, %u blocks\n", maps[i], kept, blocks_);
    }
}

#endif

// ---- check S2: the arithmetic, as test/s2.6502 drives it ----
//
// The same input bits (a counter, or xorshift32), the same drivers turning
// them into inputs, the same output bytes, the same CRC-16/XMODEM: the Beeb's
// CRC must match this one.

enum {
    S2_MULU8, S2_MULU16_FRAC, S2_MULU16_FRAC_ROUND, S2_MULG_U16, S2_RECIP8, S2_RECIP16,
    S2_MUL_RECIP8, S2_MUL_RECIP16, S2_DIV_T, S2_DIV_COLS, S2_DIV_Q8, S2_GRAD_MAG, S2_GRADQ8_MAG,
    S2_PROJECT_COL, S2_PROJECT_COL_Q8, S2_PROJECT_Q8, S2_PROJECT_ROWD, S2_LERP16, S2_ROT, S2_DIR8,
    S2_RAY_BASIS, S2_N,
};

typedef struct s2_run {
    int random;
    uint32_t x;           // the counter, or the generator's state
    uint16_t crc;
    uint32_t calls;
    uint8_t last[4];
    uint8_t n;            // output bytes this case
} s2_run;

static uint16_t crc16_tab_[256];

// CRC-16/XMODEM, as the Beeb's crc_lo and crc_hi tables compute it.
static void crc16_init(void)
{
    for (uint32_t v = 0; v < 256; v++) {
        uint32_t c = v << 8;
        for (int k = 0; k < 8; k++)
            c = ((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
        crc16_tab_[v] = (uint16_t)c;
    }
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = (uint16_t)((crc << 8) ^ crc16_tab_[((crc >> 8) ^ p[i]) & 0xFF]);
    return crc;
}

static uint32_t s2_xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static uint32_t s2_draw(s2_run *r)
{
    if (r->random) {
        r->x = s2_xorshift(r->x);
        return r->x;
    }
    return r->x++;
}

static void s2_out(s2_run *r, uint8_t b)
{
    r->last[r->n++] = b;
    r->crc = (uint16_t)((r->crc << 8) ^ crc16_tab_[((r->crc >> 8) ^ b) & 0xFF]);
}

static void s2_out16(s2_run *r, uint16_t v)
{
    s2_out(r, (uint8_t)v);
    s2_out(r, (uint8_t)(v >> 8));
}

static void s2_case(int fn, s2_run *r)
{
    uint32_t in = s2_draw(r);
    uint8_t b0 = (uint8_t)in, b1 = (uint8_t)(in >> 8), b2 = (uint8_t)(in >> 16), b3 = (uint8_t)(in >> 24);
    uint16_t lo = (uint16_t)(in & 0xFFFF), hi = (uint16_t)(in >> 16);
    r->n = 0;
    switch (fn) {
    case S2_MULU8:
        r->calls++;
        s2_out16(r, pt_mulu8(b0, b1));
        break;
    case S2_MULU16_FRAC:
        r->calls++;
        s2_out16(r, pt_mulu16_frac(lo, b2));
        break;
    case S2_MULU16_FRAC_ROUND:
        r->calls++;
        s2_out16(r, mulu16_frac_round(lo, b2));
        break;
    case S2_MULG_U16:
        r->calls++;
        s2_out16(r, pt_mulg_u16(lo, b2));
        break;
    case S2_RECIP8:
        if (b0 == 0)
            break;
        r->calls++;
        {
            pt_recip rc = pt_recip8(b0);
            s2_out(r, rc.f);
            s2_out(r, rc.e);
        }
        break;
    case S2_RECIP16:
        if (lo == 0)
            break;
        r->calls++;
        {
            pt_recip rc = pt_recip16(lo);
            s2_out(r, rc.f);
            s2_out(r, rc.e);
        }
        break;
    case S2_MUL_RECIP8: {
        r->calls++;
        prod17 v = mul_recip8(b0, (pt_recip) {.f = b1});
        s2_out(r, (uint8_t)v.v);
        s2_out(r, (uint8_t)(v.v >> 8));
        s2_out(r, v.c);    // the 17th bit, as the Beeb's driver makes it a byte
        break;
    }
    case S2_MUL_RECIP16: {
        r->calls++;
        prod17 v = mul_recip16(lo, (pt_recip) {.f = b2});
        s2_out(r, (uint8_t)v.v);
        s2_out(r, (uint8_t)(v.v >> 8));
        s2_out(r, v.c);
        break;
    }
    case S2_DIV_T: {
        r->calls++;
        uint16_t num = lo < hi ? lo : hi;
        uint16_t den = lo < hi ? hi : lo;
        if (den == 0)
            den = 1;
        s2_out(r, pt_div_t(num, den));
        break;
    }
    case S2_DIV_COLS:
        r->calls++;
        s2_out(r, pt_div_cols(lo, hi ? hi : 1));
        break;
    case S2_DIV_Q8:
        r->calls++;
        s2_out16(r, pt_div_q8(lo, (uint16_t)(b2 | (b3 ? b3 : 1) << 8)));
        break;
    case S2_GRAD_MAG:
        if (b0 > 64 || b1 == 0)
            break;
        r->calls++;
        s2_out16(r, pt_grad_mag(b0, b1));
        break;
    case S2_GRADQ8_MAG:
        if (b2 > 64 || lo == 0)
            break;
        r->calls++;
        s2_out16(r, pt_gradq8_mag(b2, lo));
        break;
    case S2_PROJECT_COL: {
        r->calls++;
        uint16_t x = lo ? lo : 1;
        int16_t y = (int16_t)hi;
        uint16_t uy = (uint16_t)(y < 0 ? -(int32_t)y : y);
        if (uy > x)
            y = y < 0 ? (int16_t)-(int32_t)x : (int16_t)x;
        s2_out(r, pt_project_col(y, x, pt_recip16(x)));
        break;
    }
    case S2_PROJECT_COL_Q8: {
        r->calls++;
        uint16_t d = lo & 0x3FFF;
        s2_out16(r, pt_project_col_q8((int16_t)hi, d ? d : 1));
        break;
    }
    case S2_PROJECT_Q8: {
        r->calls++;
        uint16_t x = hi ? hi : 1;
        s2_out16(r, pt_project_q8(lo, x, pt_recip16(x)));
        break;
    }
    case S2_PROJECT_ROWD: {
        r->calls++;
        uint16_t lim = (uint16_t)(b0 * 16);
        uint16_t x = (uint16_t)(b1 | b2 << 8);
        if (x < lim)
            x = (uint16_t)(lim + b3);
        if (x == 0)
            x = 1;
        s2_out(r, pt_project_rowd(b0, x, pt_recip16(x)));
        break;
    }
    case S2_LERP16: {
        r->calls++;
        uint8_t t = (uint8_t)s2_draw(r);
        s2_out16(r, (uint16_t)pt_lerp16((int16_t)lo, (int16_t)hi, t));
        break;
    }
    case S2_ROT: {
        r->calls++;
        int16_t x, y;
        uint8_t angle;
        if (r->random) {
            x = (int16_t)lo;
            y = (int16_t)hi;
            if (x == INT16_MIN)
                x++;
            if (y == INT16_MIN)
                y++;
            angle = (uint8_t)s2_draw(r);
        } else {
            uint32_t gx = (in >> 8) & 0x3F, gy = (in >> 14) & 0x3F;
            x = (int16_t)(uint16_t)(((uint16_t)(gx - 32) << 9) | (gx << 2));
            y = (int16_t)(uint16_t)(((uint16_t)(gy - 32) << 9) | (gy << 2));
            angle = b0;
        }
        pt_xy v = pt_rot(x, y, angle);
        s2_out16(r, (uint16_t)v.x);
        s2_out16(r, (uint16_t)v.y);
        break;
    }
    case S2_DIR8: {
        if (b0 > 127)
            break;
        r->calls++;
        pt_xy v = pt_dir8(b0, b1);
        s2_out16(r, (uint16_t)v.x);
        s2_out16(r, (uint16_t)v.y);
        break;
    }
    case S2_RAY_BASIS: {
        r->calls++;
        int16_t k1, k2;
        pt_ray_basis(b0, &k1, &k2);
        s2_out16(r, (uint16_t)k1);
        s2_out16(r, (uint16_t)k2);
        break;
    }
    }
}

// The CRC of count cases from start (a counter, or a seed), the cases used,
// the last case's outputs, and where the counter or generator ended up.
static void s2(int fn, int random, uint32_t start, uint32_t count)
{
    pt_arith_init();
    crc16_init();
    s2_run r = {.random = random, .x = start};
    uint8_t last[4] = {0};
    for (uint32_t i = 0; i < count; i++) {
        s2_case(fn, &r);
        if (r.n)
            memcpy(last, r.last, r.n);
    }
    printf("%u %u %u %u %u %u %u\n", r.crc, r.calls, last[0], last[1], last[2], last[3], r.x);
}

// ---- check S3: the mode 5 buffer (PLAN.md 5.7) ----
//
// An image is a buffer with its junk row either side, as the Beeb lays it
// out. Each operation does what test/s3.6502's does; the plots take their
// patterns from a 256-byte table, two a plot, and between them reach every
// pixel pair of the image once.

enum { S3_FILL, S3_CLEAR, S3_PLOT, S3_DOWN, S3_UP, S3_RIGHT, S3_NONE };

static pt_pattern s3_next(const uint8_t *pats, uint8_t *k)
{
    pt_pattern p = {.e = pats[*k], .o = pats[(uint8_t)(*k + 1)]};
    *k = (uint8_t)(*k + 2);
    return p;
}

static void s3(int op, const char *in, const char *pats_path, const char *out)
{
    size_t n;
    uint8_t *img = read_file(in, &n);
    size_t np;
    uint8_t *pats = read_file(pats_path, &np);
    if (n != PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD || np != 256) {
        fprintf(stderr, "beebtool: s3 wants a %d-byte image and 256 patterns\n",
                PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD);
        exit(1);
    }
    uint8_t *fb = img + PT_FB_GUARD;
    uint8_t k = 0;
    switch (op) {
    case S3_FILL:
        pt_mode5_fill(fb);
        break;
    case S3_CLEAR:
        pt_mode5_clear(fb);
        break;
    case S3_PLOT:
        for (int row = -PT_FB_GUARD_ROWS; row < PT_SCREEN_HEIGHT + PT_FB_GUARD_ROWS; row++)
            for (uint8_t col = 0; col < PT_SCREEN_WIDTH; col++)
                pt_mode5_plot(fb, col, (uint8_t)row, s3_next(pats, &k));
        break;
    case S3_DOWN:
    case S3_UP:
        for (uint8_t col = 0; col < PT_SCREEN_WIDTH; col++) {
            int start = op == S3_DOWN ? -PT_FB_GUARD_ROWS : PT_SCREEN_HEIGHT + PT_FB_GUARD_ROWS - 1;
            pt_mode5_cur cur = pt_mode5_at(col >> 2, (uint8_t)start);
            for (int i = 0; i < PT_SCREEN_HEIGHT + 2 * PT_FB_GUARD_ROWS; i++) {
                pt_mode5_eor(fb, cur, pt_mode5_pixmask(col), s3_next(pats, &k));
                if (op == S3_DOWN)
                    pt_mode5_down(&cur);
                else
                    pt_mode5_up(&cur);
            }
        }
        break;
    case S3_RIGHT:
        for (int row = -PT_FB_GUARD_ROWS; row < PT_SCREEN_HEIGHT + PT_FB_GUARD_ROWS; row++) {
            pt_mode5_cur cur = pt_mode5_at(0, (uint8_t)row);
            uint8_t mask = 0x88;
            for (uint8_t col = 0; col < PT_SCREEN_WIDTH; col++) {
                pt_mode5_eor(fb, cur, mask, s3_next(pats, &k));
                pt_mode5_right(&cur, &mask);
            }
        }
        break;
    default:
        break;
    }
    FILE *f = open_out(NULL, out);
    fwrite(img, 1, n, f);
    fclose(f);
    free(img);
    free(pats);
}

// ---- check S4: the plotters (PLAN.md 5.7) ----
//
// A record is a pool as test/s4.6502 unpacks it - a piece count n, then
// the pieces' x0, x1, y0, gradient low and high bytes, ce and co, n bytes
// each - followed by two CRC-16s of the image plot() leaves in a zeroed
// buffer: over its junk row above, the buffer and its junk row below (as
// buffer A is checked), and without the junk row below (B's is the ROM).

static void s4_record(FILE *f)
{
    uint8_t n = (uint8_t)(render_.pool_num - 1);
    uint8_t rec[1 + 7 * 255];
    rec[0] = n;
    for (uint8_t i = 1; i <= n && i != 0; i++) {
        const pt_piece *p = &render_.pool[i];
        uint8_t v[7] = {p->x0, p->x1, p->y0, (uint8_t)p->g, (uint8_t)(p->g >> 8), p->ce, p->co};
        for (int k = 0; k < 7; k++)
            rec[1 + k * n + (i - 1)] = v[k];
    }
    fwrite(rec, 1, 1 + 7u * n, f);
    memset(render_.fbmem, 0, sizeof(render_.fbmem));
    plot(&render_);
    uint16_t a = crc16(render_.fbmem, sizeof(render_.fbmem));
    uint16_t b = crc16(render_.fbmem, sizeof(render_.fbmem) - PT_FB_GUARD);
    uint8_t crcs[4] = {(uint8_t)a, (uint8_t)(a >> 8), (uint8_t)b, (uint8_t)(b >> 8)};
    fwrite(crcs, 1, 4, f);
}

// Every pose's pool, rendered as the goldens render it.
static void s4(int which, const char *poses, const char *out)
{
    crc16_init();
    load_map(which);
    pt_render_init(&render_, &map_);
    size_t n;
    uint8_t *p = read_file(poses, &n);
    FILE *f = open_out(NULL, out);
    for (size_t i = 0; i + 6 <= n; i += 6) {
        pt_render_frame(&render_, (uint16_t)(p[i] | p[i + 1] << 8),
                        (uint16_t)(p[i + 2] | p[i + 3] << 8), p[i + 4], p[i + 5]);
        s4_record(f);
    }
    fclose(f);
    free(p);
}

// count random pools of pieces pieces each: lines between two points on
// the screen (or rising from its bottom edge), every gradient, some
// uncoloured and some starting on the bottom edge without rising, which
// plot skips.
static void s4_random(uint32_t seed, uint32_t count, uint32_t pieces, const char *out)
{
    crc16_init();
    load_map(0);    // (for a render state, fb and all; the map itself plays no part)
    pt_render_init(&render_, &map_);
    rc_random rng = rc_random_make(seed);
    FILE *f = open_out(NULL, out);
    for (uint32_t k = 0; k < count; k++) {
        render_.pool_num = (uint16_t)(pieces + 1);
        for (uint32_t i = 1; i <= pieces; i++) {
            pt_piece *p = &render_.pool[i];
            uint32_t r = rc_random_next(&rng);
            uint8_t x0 = (uint8_t)(r & 127);
            uint8_t len = (uint8_t)(1 + (r >> 7) % (uint32_t)(128 - x0));
            if ((r >> 14) & 1)
                len = (uint8_t)(1 + (r >> 15) % (len < 12 ? len : 12));    // short ones too
            uint32_t s = rc_random_next(&rng);
            uint8_t y0 = (uint8_t)(s % 65);
            uint8_t y1 = (uint8_t)((s >> 8) % 64);
            if ((s >> 16) % 4 == 0)
                y1 = y0 == 64 ? 63 : y0;    // flat lines, the commonest kind
            uint32_t dy = y1 > y0 ? (uint32_t)(y1 - y0) : (uint32_t)(y0 - y1);
            uint32_t mag = len > 1 ? dy * 256 / (uint32_t)(len - 1) : 0;
            if (mag > 0x7FFF)
                mag = 0x7FFF;
            p->x0 = x0;
            p->x1 = (uint8_t)(x0 + len);
            p->y0 = y0;
            p->g = pt_g_make((uint16_t)mag, y1 < y0);
            uint32_t c = rc_random_next(&rng);
            p->ce = (uint8_t)c;
            p->co = (uint8_t)(c >> 8);
            if (((c >> 16) & 15) == 0)
                p->ce = p->co = 0;    // a bound only
            p->next = 0;
        }
        s4_record(f);
    }
    fclose(f);
}

// ---- check S5: every vertex's transform at every pose (PLAN.md 5.7) ----
//
// Per pose, its camera (x, y, angle) and a CRC-16 over every map vertex's
// view vertex, 8 bytes each as REPLAY.md's VV: what test/s5.6502 computes.
static void s5t(int which, const char *poses, const char *out)
{
    crc16_init();
    load_map(which);
    pt_render_init(&render_, &map_);
    size_t n;
    uint8_t *p = read_file(poses, &n);
    FILE *f = open_out(NULL, out);
    for (size_t i = 0; i + 6 <= n; i += 6) {
        render_.cam_x = (uint16_t)(p[i] | p[i + 1] << 8);
        render_.cam_y = (uint16_t)(p[i + 2] | p[i + 3] << 8);
        render_.angle = p[i + 4];
        uint8_t vv[8 * PT_MAX_VERTS];
        for (uint32_t k = 0; k < map_.num_verts; k++) {
            pt_vv v = transform(&render_, map_.verts[k]);
            uint8_t *q = &vv[8 * k];
            q[0] = (uint8_t)v.x;
            q[1] = (uint8_t)((uint16_t)v.x >> 8);
            q[2] = (uint8_t)v.y;
            q[3] = (uint8_t)((uint16_t)v.y >> 8);
            q[4] = v.sx;
            q[5] = v.flags;
            q[6] = v.rx.f;
            q[7] = v.rx.e;
        }
        uint16_t crc = crc16(vv, 8u * map_.num_verts);
        uint8_t rec[7] = {p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], (uint8_t)crc, (uint8_t)(crc >> 8)};
        fwrite(rec, 1, sizeof rec, f);
    }
    fclose(f);
    free(p);
}

// ---- check S5: make_line on synthetic endpoints (PLAN.md 5.7) ----
//
// The golden poses rarely clip a line at the near depth with the clip
// column rounding left of the near endpoint's, or the start row
// overflowing u8.8 (host/uncovered.txt): random endpoint pairs, often one
// near and one far, cover them. A record is make_line's arguments as
// REPLAY.md's ARGS (18 bytes), then its LINE (7).

// An endpoint: a depth (half the time within twice the clip depth d) and a
// lateral position inside the frustum, its column projected as transform's.
static pt_ep s5_ep(rc_random *rng, uint16_t d)
{
    uint32_t r = rc_random_next(rng);
    uint16_t x = d && (r & 1) ? (uint16_t)(1 + (r >> 1) % (2u * d + 1)) : (uint16_t)(1 + (r >> 1) % 4096);
    uint32_t s = rc_random_next(rng);
    int16_t y = (int16_t)((int32_t)(s % (2u * x + 1)) - (int32_t)x);
    pt_recip rx = pt_recip16(x);
    return (pt_ep) {.x = x, .y = y, .sx = pt_project_col(y, x, rx), .rx = rx};
}

static void put_ep(uint8_t *q, const pt_ep *e)
{
    q[0] = (uint8_t)e->x;
    q[1] = (uint8_t)(e->x >> 8);
    q[2] = (uint8_t)e->y;
    q[3] = (uint8_t)((uint16_t)e->y >> 8);
    q[4] = e->sx;
    q[5] = e->rx.f;
    q[6] = e->rx.e;
}

static void s5make(uint32_t seed, uint32_t count, const char *out)
{
    pt_arith_init();
    rc_random rng = rc_random_make(seed);
    FILE *f = open_out(NULL, out);
    for (uint32_t k = 0; k < count;) {
        uint32_t r = rc_random_next(&rng);
        pt_relh h = {.m = (uint8_t)(r % 81), .below = (uint8_t)((r >> 8) & 1)};
        pt_ep a = s5_ep(&rng, (uint16_t)(h.m * 16));
        pt_ep b = s5_ep(&rng, (uint16_t)(h.m * 16));
        if (a.sx == b.sx)
            continue;
        pt_ep e0 = a.sx < b.sx ? a : b;
        pt_ep e1 = a.sx < b.sx ? b : a;
        pt_pattern col = {.e = (uint8_t)(r >> 16), .o = (uint8_t)(r >> 24)};
        pt_line L = make_line(e0, e1, h, col);
        uint8_t rec[25];
        put_ep(rec, &e0);
        put_ep(rec + 7, &e1);
        rec[14] = h.m;
        rec[15] = h.below;
        rec[16] = col.e;
        rec[17] = col.o;
        rec[18] = L.x0;
        rec[19] = L.x1;
        rec[20] = L.y0;
        rec[21] = (uint8_t)L.g;
        rec[22] = (uint8_t)(L.g >> 8);
        rec[23] = L.ce;
        rec[24] = L.co;
        fwrite(rec, 1, sizeof rec, f);
        k++;
    }
    fclose(f);
}

// Pools given as records without their CRCs (the host's, splitting a
// pool that failed into single pieces), with them added.
static void s4_crc(const char *in, const char *out)
{
    crc16_init();
    load_map(0);
    pt_render_init(&render_, &map_);
    size_t n;
    uint8_t *p = read_file(in, &n);
    FILE *f = open_out(NULL, out);
    for (size_t i = 0; i < n;) {
        uint8_t np = p[i];
        render_.pool_num = (uint16_t)(np + 1);
        for (uint8_t k = 0; k < np; k++) {
            const uint8_t *v = &p[i + 1 + k];
            render_.pool[k + 1] = (pt_piece) {.x0 = v[0], .x1 = v[np], .y0 = v[2 * np],
                                              .g = (uint16_t)(v[3 * np] | v[4 * np] << 8),
                                              .ce = v[5 * np], .co = v[6 * np]};
        }
        s4_record(f);
        i += 1 + 7u * np;
    }
    fclose(f);
    free(p);
}

// ---- checks S6 and S7: whole captures and frames (PLAN.md 5.7) ----
//
// Per pose, what test/s6.6502 forms: a CRC-16 of the capture - the pieces
// 1.. as the Beeb's pages hold them (x0 x1 y0 g ce co next), the visits
// 1.. (cell k xr, v0, v1, inst, active[4]), the instances 1.. (sprite cx0
// cx1 seg seg_tail prev next, scale, left and top in 24 bits), the order's
// head and tail, seg_num (a byte: 256 is 0) and the segments 1.., num_pend
// - and two of the image, as S4's. The sprites on (S7) or off (S6). A
// record is the pose (6), then those three CRCs, pool_last and num_visits:
// 14 bytes.

// The capture as s6.6502 CRCs it, into out; its length.
static size_t s6_state(uint8_t *out)
{
    size_t n = 0;
    for (uint32_t i = 1; i < render_.pool_num; i++) {
        const pt_piece *p = &render_.pool[i];
        uint8_t v[8] = {p->x0, p->x1, p->y0, (uint8_t)p->g, (uint8_t)(p->g >> 8), p->ce, p->co, p->next};
        memcpy(out + n, v, 8);
        n += 8;
    }
    for (uint32_t i = 1; i < render_.num_visits; i++) {
        const pt_visit *vs = &render_.visits[i];
        const pt_vv *vv[2] = {&vs->v0, &vs->v1};
        out[n++] = vs->cell;
        out[n++] = vs->k;
        out[n++] = vs->xr;
        for (int k = 0; k < 2; k++) {
            out[n++] = (uint8_t)vv[k]->x;
            out[n++] = (uint8_t)((uint16_t)vv[k]->x >> 8);
            out[n++] = (uint8_t)vv[k]->y;
            out[n++] = (uint8_t)((uint16_t)vv[k]->y >> 8);
            out[n++] = vv[k]->sx;
            out[n++] = vv[k]->flags;
            out[n++] = vv[k]->rx.f;
            out[n++] = vv[k]->rx.e;
        }
        out[n++] = vs->inst;
        for (int s = 0; s < PT_NUM_SLOTS; s++)
            out[n++] = vs->active[s];
    }
    for (uint32_t i = 1; i < render_.num_insts; i++) {
        const pt_inst *in = &render_.insts[i];
        uint8_t v[15] = {in->sprite, in->cx0, in->cx1, in->seg, in->seg_tail, in->prev, in->next,
                         (uint8_t)in->scale, (uint8_t)(in->scale >> 8),
                         (uint8_t)in->left, (uint8_t)(in->left >> 8), (uint8_t)(in->left >> 16),
                         (uint8_t)in->top, (uint8_t)(in->top >> 8), (uint8_t)(in->top >> 16)};
        memcpy(out + n, v, 15);
        n += 15;
    }
    out[n++] = render_.order_head;
    out[n++] = render_.order_tail;
    out[n++] = (uint8_t)render_.seg_num;
    for (uint32_t i = 1; i < render_.seg_num; i++)
        out[n++] = render_.segs[i];
    out[n++] = render_.num_pend;
    return n;
}

// Every pose's record; and, for pose number dump (if any), the capture's
// bytes as s6_state lays them out, then the image, into dumpfile.
static void s6(int which, const char *poses, const char *out, long dump, const char *dumpfile, bool sprites)
{
    crc16_init();
    load_map(which);
    pt_render_init(&render_, &map_);
    render_.sprites = sprites;
    size_t n;
    uint8_t *p = read_file(poses, &n);
    FILE *f = open_out(NULL, out);
    static uint8_t state[8 * 256 + 24 * PT_MAX_VISITS + 15 * PT_MAX_INSTS + PT_SEG_POOL + 8];
    for (size_t i = 0; i + 6 <= n; i += 6) {
        memset(render_.fbmem, 0, sizeof(render_.fbmem));
        pt_render_frame(&render_, (uint16_t)(p[i] | p[i + 1] << 8),
                        (uint16_t)(p[i + 2] | p[i + 3] << 8), p[i + 4], p[i + 5]);
        size_t sn = s6_state(state);
        uint16_t c = crc16(state, sn);
        uint16_t a = crc16(render_.fbmem, sizeof(render_.fbmem));
        uint16_t b = crc16(render_.fbmem, sizeof(render_.fbmem) - PT_FB_GUARD);
        uint8_t rec[14] = {p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], p[i + 5],
                           (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)a, (uint8_t)(a >> 8),
                           (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(render_.pool_num - 1),
                           render_.num_visits};
        fwrite(rec, 1, sizeof rec, f);
        if (dump >= 0 && (size_t)dump == i / 6) {
            FILE *d = open_out(NULL, dumpfile);
            fwrite(state, 1, sn, d);
            fwrite(render_.fbmem, 1, sizeof(render_.fbmem), d);
            fclose(d);
        }
    }
    fclose(f);
    free(p);
}

#ifdef BEEBTOOL_TRACE
#include "trace_hooks.c"
#endif

int main(int argc, char **argv)
{
#ifdef BEEBTOOL_TRACE
    if (argc >= 3 && !strcmp(argv[1], "record")) {
        record(argv[2]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "replaycost")) {
        replaycost(argv[2], argv[3]);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "synthclose")) {
        synth_close(argv[2], (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 0));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "replaycheck")) {
        replaycheck(argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "cost")) {
        cost(argv[2], argc >= 4 ? argv[3] : NULL);
        return 0;
    }
#endif
#ifdef BEEBTOOL_SEARCH
    if (argc >= 4 && !strcmp(argv[1], "search")) {
        search(argv[2], (uint32_t)strtoul(argv[3], NULL, 0));
        return 0;
    }
#endif
    if (argc >= 3 && !strcmp(argv[1], "export")) {
        export_all(argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "tables")) {
        tables(argv[2]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "expect")) {
        expect(atoi(argv[2]), argv[3]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "goldens")) {
        goldens(argv[2]);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "s2")) {
        s2(atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 0),
           (uint32_t)strtoul(argv[5], NULL, 0));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "s4")) {
        s4(atoi(argv[2]), argv[3], argv[4]);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "s5make")) {
        s5make((uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0), argv[4]);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "s5t")) {
        s5t(atoi(argv[2]), argv[3], argv[4]);
        return 0;
    }
    if (argc >= 5 && (!strcmp(argv[1], "s6") || !strcmp(argv[1], "s7"))) {
        s6(atoi(argv[2]), argv[3], argv[4], argc >= 7 ? atol(argv[5]) : -1, argc >= 7 ? argv[6] : NULL,
           argv[1][1] == '7');
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "s4crc")) {
        s4_crc(argv[2], argv[3]);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "s4random")) {
        s4_random((uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0),
                  (uint32_t)strtoul(argv[4], NULL, 0), argv[5]);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "s3")) {
        s3(atoi(argv[2]), argv[3], argv[4], argv[5]);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "render")) {
        render(atoi(argv[2]), argv[3], argv[4], argc >= 6 && !strcmp(argv[5], "poison"));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "camera")) {
        camera(atoi(argv[2]), argv[3], argv[4], argc >= 6 && !strcmp(argv[5], "poison"));
        return 0;
    }
    fprintf(stderr, "usage: beebtool export|tables|expect|goldens|s2|s3|s4|s4random|render|camera ...\n");
    return 2;
}
