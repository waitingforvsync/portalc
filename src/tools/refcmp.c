// refcmp.c - the renderer against an exact reference: every pixel centre's
// ray cast through the cells in floating point. A pixel counts as wrong
// when it matches the reference neither on its own row nor a row up or
// down (rounding at an edge). Sprites are off: the reference has none.
//
//   portalc_refcmp MAP OUT                    every pose of MAP (0 the demo
//                                             scene, 9 the stress map): per
//                                             frame its pose and the pixels
//                                             wrong, for refcmp.py
//   portalc_refcmp MAP OUT.ppm X Y ANGLE CELL one pose: the frame, the
//                                             reference and the wrong pixels
//
// The poses: every cell's centroid at 64 angles, 20,000 at random, and the
// eye 1 and 6 units off every vertex. A record is 8 bytes, little endian:
// x, y (16 bits), angle, cell, wrong pixels (16 bits).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "map.h"
#include "mode5.h"
#include "render.h"
#include "richc/random.h"

static pt_map map;
static pt_render render;
static uint8_t ref[PT_SCREEN_HEIGHT][PT_SCREEN_WIDTH];    // shade + 1, 0 = no hit

// ---- the reference ----

// The shade + 1 a ray meets: from (px, py) at height eye, along (dx, dy)
// with forward depth 1 a unit and lz height a unit, through cell's exits.
static uint8_t cast(double px, double py, uint8_t cell, double eye, double dx, double dy,
                    double lz)
{
    double t = 0;
    int entry = -1;
    for (int steps = 0; steps < 64; steps++) {
        const pt_cell *c = &map.cells[cell];
        double best = 1e30;
        int exit = -1;
        for (uint8_t k = 0; k < c->num_edges; k++) {
            uint8_t e = (uint8_t)(c->first_edge + k);
            if (e == entry)
                continue;
            pt_vertex a = map.verts[map.edges[e].v0];
            pt_vertex b = map.verts[map.edges[pt_map_ring_next(c, e)].v0];
            double ex = (double)b.x - a.x;
            double ey = (double)b.y - a.y;
            double den = dx * ey - dy * ex;
            if (fabs(den) < 1e-12)
                continue;
            double ax = a.x - px;
            double ay = a.y - py;
            double tt = (ax * ey - ay * ex) / den;
            double u = (ax * dy - ay * dx) / den;
            if (tt > t + 1e-9 && u >= -1e-9 && u <= 1 + 1e-9 && tt < best) {
                best = tt;
                exit = e;
            }
        }
        if (exit < 0)
            return 0;

        // the floor or ceiling before the exit, else the exit's wall, else on
        double z = eye + lz * best;
        if (lz > 0 && z > 8.0 * c->floor_z)
            return (uint8_t)(c->floor + 1);
        if (lz < 0 && z < 8.0 * c->ceil_z)
            return (uint8_t)(c->ceil + 1);
        const pt_edge *ed = &map.edges[exit];
        if (ed->adj == 0)
            return ed->colour;
        const pt_cell *n = &map.cells[ed->adj];
        if (z > 8.0 * n->floor_z || z < 8.0 * n->ceil_z)
            return ed->colour;
        t = best;
        entry = ed->twin;
        cell = ed->adj;
    }
    return 0;
}

static void ref_frame(uint16_t x, uint16_t y, uint8_t angle, uint8_t cell)
{
    double th = angle * (2 * M_PI / 256);
    double fx = cos(th), fy = sin(th);
    double rx = -sin(th), ry = cos(th);
    double eye = 8.0 * (map.cells[cell].floor_z - PT_CAM_HEIGHT);
    for (int c = 0; c < PT_SCREEN_WIDTH; c++) {
        double ly = (c + 0.5 - 64) / 64;
        for (int r = 0; r < PT_SCREEN_HEIGHT; r++)
            ref[r][c] = cast(x, y, cell, eye, fx + ly * rx, fy + ly * ry, (r + 0.5 - 32) / 64);
    }
}

// ---- comparing ----

// A pixel's two scanlines' 2-bit values, from a framebuffer or a pattern.
static uint8_t fb_px(const uint8_t *fb, int c, int r)
{
    uint8_t v = 0;
    for (int k = 0; k < 2; k++) {
        uint8_t b = fb[pt_mode5_offset((uint8_t)(c >> 2), (uint8_t)(2 * r + k))];
        int p = c & 3;
        v = (uint8_t)((v << 2) | (((b >> (7 - p)) & 1) << 1) | ((b >> (3 - p)) & 1));
    }
    return v;
}

static uint8_t shade_px(uint8_t s1, int c)
{
    if (s1 == 0)
        return 0xFF;
    pt_pattern pat = pt_shade((uint8_t)(s1 - 1));
    uint8_t v = 0;
    for (int k = 0; k < 2; k++) {
        uint8_t b = k ? pat.o : pat.e;
        int p = c & 3;
        v = (uint8_t)((v << 2) | (((b >> (7 - p)) & 1) << 1) | ((b >> (3 - p)) & 1));
    }
    return v;
}

static bool wrong(const uint8_t *fb, int c, int r)
{
    uint8_t v = fb_px(fb, c, r);
    if (v == shade_px(ref[r][c], c))
        return false;
    if (r > 0 && v == shade_px(ref[r - 1][c], c))
        return false;
    if (r + 1 < PT_SCREEN_HEIGHT && v == shade_px(ref[r + 1][c], c))
        return false;
    return true;
}

static uint16_t errors(const uint8_t *fb)
{
    uint16_t n = 0;
    for (int r = 0; r < PT_SCREEN_HEIGHT; r++)
        for (int c = 0; c < PT_SCREEN_WIDTH; c++)
            n = (uint16_t)(n + wrong(fb, c, r));
    return n;
}

// ---- the pose set ----

static FILE *out;
static unsigned frames;

static void frame(uint16_t x, uint16_t y, uint8_t angle, uint8_t cell)
{
    pt_render_frame(&render, x, y, angle, cell);
    ref_frame(x, y, angle, cell);
    uint16_t er = errors(render.fb);
    uint8_t rec[8] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8),
                      angle,      cell,             (uint8_t)er, (uint8_t)(er >> 8)};
    fwrite(rec, 1, sizeof rec, out);
    frames++;
}

static void all_poses(int which)
{
    // every cell's centroid at 64 angles
    for (uint8_t c = 1; c < map.num_cells; c++) {
        const pt_cell *cl = &map.cells[c];
        uint32_t sx = 0, sy = 0;
        for (uint8_t k = 0; k < cl->num_edges; k++) {
            pt_vertex v = map.verts[map.edges[cl->first_edge + k].v0];
            sx += v.x;
            sy += v.y;
        }
        uint16_t x = (uint16_t)(sx / cl->num_edges);
        uint16_t y = (uint16_t)(sy / cl->num_edges);
        for (unsigned a = 0; a < 256; a += 4)
            frame(x, y, (uint8_t)a, c);
    }

    // at random in the map's bounding box, wherever a cell holds the point
    uint16_t minx = 0xFFFF, maxx = 0, miny = 0xFFFF, maxy = 0;
    for (uint8_t i = 0; i < map.num_verts; i++) {
        pt_vertex v = map.verts[i];
        minx = v.x < minx ? v.x : minx;
        maxx = v.x > maxx ? v.x : maxx;
        miny = v.y < miny ? v.y : miny;
        maxy = v.y > maxy ? v.y : maxy;
    }
    rc_random rng = rc_random_make(12345u + (uint32_t)which);
    for (unsigned n = 0; n < 20000;) {
        uint16_t x = (uint16_t)(minx + rc_random_next(&rng) % (uint32_t)(maxx - minx));
        uint16_t y = (uint16_t)(miny + rc_random_next(&rng) % (uint32_t)(maxy - miny));
        uint8_t cell = pt_map_find_cell(&map, x, y);
        if (cell == 0)
            continue;
        frame(x, y, (uint8_t)rc_random_next(&rng), cell);
        n++;
    }

    // the eye 1 and 6 units off every vertex, diagonally, at 16 angles
    for (uint8_t i = 0; i < map.num_verts; i++)
        for (int d = 1; d <= 6; d += 5)
            for (unsigned a = 0; a < 256; a += 16) {
                uint16_t x = (uint16_t)(map.verts[i].x + ((a & 32) ? d : -d));
                uint16_t y = (uint16_t)(map.verts[i].y + ((a & 64) ? d : -d));
                uint8_t cell = pt_map_find_cell(&map, x, y);
                if (cell != 0)
                    frame(x, y, (uint8_t)(a * 7 + i), cell);
            }
}

// ---- one pose, as a picture ----

static uint8_t ref_fb[PT_FB_BYTES];
static uint8_t rgba_eng[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];
static uint8_t rgba_ref[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];

static void write_px(FILE *f, const uint8_t *p)
{
    fwrite(p, 1, 3, f);
    fwrite(p, 1, 3, f);    // double width pixels
}

static void picture(const char *path)
{
    for (int r = 0; r < PT_SCREEN_HEIGHT; r++)
        for (int c = 0; c < PT_SCREEN_WIDTH; c++) {
            if (ref[r][c] == 0)
                continue;
            pt_pattern pat = pt_shade((uint8_t)(ref[r][c] - 1));
            uint8_t m = (uint8_t)(0x88 >> (c & 3));
            ref_fb[pt_mode5_offset((uint8_t)(c >> 2), (uint8_t)(2 * r))] |= pat.e & m;
            ref_fb[pt_mode5_offset((uint8_t)(c >> 2), (uint8_t)(2 * r + 1))] |= pat.o & m;
        }
    pt_mode5_to_rgba(render.fb, rgba_eng);
    pt_mode5_to_rgba(ref_fb, rgba_ref);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    int gap = 4;
    fprintf(f, "P6\n%d %d\n255\n", 3 * 2 * PT_SCREEN_WIDTH + 2 * gap, PT_SCREEN_SCANLINES);
    static const uint8_t grey[3] = {128, 128, 128};
    static const uint8_t hit[3] = {255, 255, 255};
    for (int y = 0; y < PT_SCREEN_SCANLINES; y++) {
        for (int x = 0; x < PT_SCREEN_WIDTH; x++)
            write_px(f, &rgba_eng[(y * PT_SCREEN_WIDTH + x) * 4]);
        for (int k = 0; k < gap; k++)
            fwrite(grey, 1, 3, f);
        for (int x = 0; x < PT_SCREEN_WIDTH; x++)
            write_px(f, &rgba_ref[(y * PT_SCREEN_WIDTH + x) * 4]);
        for (int k = 0; k < gap; k++)
            fwrite(grey, 1, 3, f);
        // the wrong pixels in white over the frame, darkened
        for (int x = 0; x < PT_SCREEN_WIDTH; x++) {
            if (wrong(render.fb, x, y / 2)) {
                write_px(f, hit);
                continue;
            }
            const uint8_t *p = &rgba_eng[(y * PT_SCREEN_WIDTH + x) * 4];
            uint8_t d[3] = {(uint8_t)(p[0] / 4), (uint8_t)(p[1] / 4), (uint8_t)(p[2] / 4)};
            write_px(f, d);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 7) {
        fprintf(stderr, "usage: portalc_refcmp MAP OUT\n"
                        "       portalc_refcmp MAP OUT.ppm X Y ANGLE CELL\n");
        return 1;
    }
    int which = atoi(argv[1]);
    if (which != 0 && which != 9) {
        fprintf(stderr, "refcmp: MAP is 0 (the demo scene) or 9 (the stress map)\n");
        return 1;
    }

    pt_arith_init();
    if (which == 9)
        pt_stress_scene_init(&map);
    else
        pt_scene_init(&map);
    pt_render_init(&render, &map);
    render.sprites = false;

    if (argc == 7) {
        uint16_t x = (uint16_t)atoi(argv[3]);
        uint16_t y = (uint16_t)atoi(argv[4]);
        uint8_t angle = (uint8_t)atoi(argv[5]);
        uint8_t cell = (uint8_t)atoi(argv[6]);
        pt_render_frame(&render, x, y, angle, cell);
        ref_frame(x, y, angle, cell);
        picture(argv[2]);
        printf("%u pixels wrong\n", errors(render.fb));
        return 0;
    }

    out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    all_poses(which);
    fclose(out);
    printf("map %d: %u frames\n", which, frames);
    return 0;
}
