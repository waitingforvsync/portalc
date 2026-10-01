// test_sprite.c - sprite art encoding, the demo scene's boulder, and the
// capture's sprite bookkeeping: projection, clip segments and draw order.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "render.h"
#include "richc/random.h"
#include "richc/test.h"
#include "sprite.h"
#include "testmaps.h"

static pt_map map_;
static pt_render render_;

// Every pixel decodes back to the code it was encoded from.
static void check_round_trip(const char *const rows[])
{
    static pt_sprite_art a;
    static const uint8_t pal[7] = {1, 2, 3, 4, 5, 6, 7};
    pt_sprite_encode(&a, rows, pal);
    uint8_t h = 0;
    while (h < PT_SPRITE_MAX_ROWS && rows[h] != NULL)
        h++;
    RC_CHECK(pt_sprite_height(&a), ==, h);
    for (uint8_t r = 0; r < h; r++)
        for (uint8_t c = 0; c < pt_sprite_width(&a); c++)
            RC_CHECK(pt_sprite_code(&a, c, r), ==, (uint8_t)(rows[r][c] - '0'));
}

RC_TEST(sprite, round_trip)
{
    // every code, runs of one row, a colour change on every row, and a
    // column that is one run throughout
    static const char *const small[] = {
        "0123",
        "4567",
        "7777",
        "0000",
        "0707",
        NULL,
    };
    check_round_trip(small);

    // the full height: runs starting on every row up to the last
    static const char *tall[PT_SPRITE_MAX_ROWS];
    static char buf[PT_SPRITE_MAX_ROWS][4];
    for (uint8_t r = 0; r < PT_SPRITE_MAX_ROWS; r++) {
        buf[r][0] = (char)('0' + r % 8);
        buf[r][1] = (char)('0' + (r / 4) % 8);
        buf[r][2] = '5';
        buf[r][3] = 0;
        tall[r] = buf[r];
    }
    check_round_trip(tall);
}

RC_TEST(sprite, format)
{
    static const char *const rows[] = {
        "10",
        "12",
        "22",
        NULL,
    };
    static const uint8_t pal[7] = {0};
    static pt_sprite_art a;
    pt_sprite_encode(&a, rows, pal);

    // header, column offsets, then the runs: column 0 is 1 from row 0 and 2
    // from row 2; column 1 is 0 from row 0 and 2 from row 1; a 0 ends it
    static const uint8_t want[] = {2, 3, 4, 6, 0 << 3 | 1, 2 << 3 | 2, 0 << 3 | 0, 1 << 3 | 2, 0};
    for (uint8_t i = 0; i < sizeof want; i++)
        RC_CHECK(a.data[i], ==, want[i]);
}

RC_TEST(sprite, boulder)
{
    static pt_map m;
    pt_scene_init(&m);

    // the pit (cell 1) holds the boulder: 14 x 16, 128 units tall
    const pt_cell *pit = &m.cells[1];
    RC_CHECK(pit->sprite, !=, (uint8_t)0);
    const pt_sprite *sp = &m.sprites[pit->sprite];
    const pt_sprite_art *a = &m.arts[sp->art];
    RC_CHECK(pt_sprite_width(a), ==, (uint8_t)14);
    RC_CHECK(pt_sprite_height(a), ==, (uint8_t)16);
    RC_CHECK(sp->size, ==, (uint16_t)128);
    RC_CHECK_TRUE(pt_map_cell_contains(&m, 1, sp->x, sp->y));

    // no other cell has one
    for (uint8_t c = 2; c < m.num_cells; c++)
        RC_CHECK(m.cells[c].sprite, ==, (uint8_t)0);
}

RC_TEST(sprite, boulder_projection)
{
    // from the start position the boulder stands 220 units ahead, its base
    // 104 below the eye: a little over half the screen's height, centred
    pt_arith_init();
    pt_scene_init(&map_);
    pt_render_init(&render_, &map_);
    uint8_t cell = pt_map_find_cell(&map_, PT_CAM_START_X, PT_CAM_START_Y);
    pt_render_frame(&render_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE, cell);

    RC_CHECK(render_.num_insts, ==, (uint8_t)2);
    const pt_inst *in = &render_.insts[1];
    double scale = 64.0 * 128 / (16 * 220);             // 2.33 pixels a source pixel
    double bottom = 32 + 64.0 * 104 / 220;              // row 62.3
    double top = bottom - scale * 16;                   // row 25.0
    RC_CHECK(fabs(in->scale / 256.0 - scale) < 0.02, ==, true);
    RC_CHECK(fabs(in->top / 256.0 - top) < 0.5, ==, true);
    RC_CHECK(fabs(in->left / 256.0 - (64 - scale * 7)) < 0.5, ==, true);
    RC_CHECK(in->cx0, ==, (uint8_t)48);
    RC_CHECK(in->cx1, ==, (uint8_t)80);

    // seen whole, through the pit's portal: one draw
    RC_CHECK(render_.order_head, ==, (uint8_t)1);
    RC_CHECK(render_.order_tail, ==, (uint8_t)1);
    RC_CHECK(in->seg, !=, (uint8_t)0);
}

RC_TEST(sprite, projection_all_angles)
{
    // turning on the spot at the start position sweeps the boulder across
    // the screen and off it: whenever any of it should be on screen (its
    // centre inside the view cone or not), it is projected, and its columns
    // agree with floating point to a pixel
    pt_arith_init();
    pt_scene_init(&map_);
    pt_render_init(&render_, &map_);
    uint8_t cell = pt_map_find_cell(&map_, PT_CAM_START_X, PT_CAM_START_Y);
    const pt_sprite *sp = &map_.sprites[map_.cells[1].sprite];
    uint32_t outside = 0;
    for (uint32_t a = 0; a < 256; a++) {
        pt_render_frame(&render_, PT_CAM_START_X, PT_CAM_START_Y, (uint8_t)a, cell);
        double th = a * (2 * 3.14159265358979323846 / 256);
        double wx = (double)sp->x - PT_CAM_START_X;
        double wy = (double)sp->y - PT_CAM_START_Y;
        double x = wx * cos(th) + wy * sin(th);
        double y = wy * cos(th) - wx * sin(th);
        if (x < PT_SPRITE_NEAR + 2)
            continue;
        double half = 64.0 * 0.5 * sp->size * 14 / 16 / x;
        double centre = 64 + 64 * y / x;
        double c0 = floor(centre - half + 0.5);
        double c1 = floor(centre + half + 0.5);
        if (c0 < 0)
            c0 = 0;
        if (c1 > PT_SCREEN_WIDTH)
            c1 = PT_SCREEN_WIDTH;
        if (c1 - c0 < 2)
            continue;    // too close to call at the screen's sides
        if (fabs(y) > x)
            outside++;
        RC_CHECK(render_.num_insts, ==, (uint8_t)2);
        if (render_.num_insts == 2) {
            RC_CHECK(fabs(render_.insts[1].cx0 - c0) <= 1, ==, true);
            RC_CHECK(fabs(render_.insts[1].cx1 - c1) <= 1, ==, true);
        }
    }
    RC_CHECK(outside, >, (uint32_t)0);    // some with the centre outside the view cone
}

// The seven-sprite map (testmaps.c), checked valid.
static void add_test_sprites(pt_map *m)
{
    RC_CHECK_TRUE(pt_testmaps_add_sprites(m));
}

// Where the ray through column c's centre enters a cell, as a distance along
// the ray: 0 for the camera's own, -1 when it misses (Cyrus-Beck against
// the convex ring, whose interior lies on each edge's positive side).
static double ray_enter(const pt_map *m, uint8_t cell, double px, double py, double dx, double dy)
{
    const pt_cell *c = &m->cells[cell];
    double t0 = 0;
    double t1 = 1e9;
    for (uint8_t k = 0; k < c->num_edges; k++) {
        uint8_t e = (uint8_t)(c->first_edge + k);
        pt_vertex a = m->verts[m->edges[e].v0];
        pt_vertex b = m->verts[m->edges[pt_map_ring_next(c, e)].v0];
        double ex = (double)b.x - a.x;
        double ey = (double)b.y - a.y;
        // side(p) = ex * (p.y - a.y) - ey * (p.x - a.x), positive inside
        double s0 = ex * (py - a.y) - ey * (px - a.x);
        double ds = ex * dy - ey * dx;
        if (fabs(ds) < 1e-12) {
            if (s0 < 0)
                return -1;
            continue;
        }
        double t = -s0 / ds;
        if (ds > 0) {
            if (t > t0)
                t0 = t;
        } else if (t < t1) {
            t1 = t;
        }
    }
    return t0 < t1 ? t0 : -1;
}

// Every frame's bookkeeping: segments in x order within the instance's
// columns, each bound chain's pieces in x order across its columns; the
// draw order holding each instance with a segment exactly once; and, where
// two instances share a column, the one whose cell the column's ray enters
// later (farther) drawn first.
static uint32_t compared_;    // column pairs the order check decided on

static void check_frame(const pt_map *m, const pt_render *r, uint16_t camx, uint16_t camy,
                        uint8_t angle)
{
    uint8_t order_pos[PT_MAX_INSTS] = {0};
    uint8_t n = 0;
    uint8_t prev = 0;
    for (uint8_t i = r->order_head; i != 0; i = r->insts[i].next) {
        RC_CHECK(r->insts[i].prev, ==, prev);
        RC_CHECK(order_pos[i], ==, (uint8_t)0);    // once only
        order_pos[i] = ++n;
        prev = i;
    }
    RC_CHECK(r->order_tail, ==, prev);

    // each instance's covered columns, and which of them a segment reaches
    uint8_t covers[PT_MAX_INSTS][PT_SCREEN_WIDTH] = {{0}};
    for (uint8_t i = 1; i < r->num_insts; i++) {
        const pt_inst *in = &r->insts[i];
        RC_CHECK((in->seg != 0), ==, (order_pos[i] != 0));
        uint8_t last_x1 = in->cx0;
        for (uint8_t sg = in->seg; sg != 0; sg = r->segs[sg + PT_SEG_NEXT]) {
            uint8_t x0 = r->segs[sg + PT_SEG_X0];
            uint8_t x1 = r->segs[sg + PT_SEG_X1];
            RC_CHECK(x0, >=, last_x1);
            RC_CHECK(x0, <, x1);
            RC_CHECK(x1, <=, in->cx1);
            last_x1 = x1;
            const uint8_t *pc = &r->segs[sg + PT_SEG_PIECES];
            for (uint8_t side = 0; side < 2; side++) {
                uint8_t np = r->segs[sg + (side ? PT_SEG_NB : PT_SEG_NT)];
                RC_CHECK(np, >=, (uint8_t)1);
                RC_CHECK(r->pool[pc[0]].x0, <=, x0);
                for (uint8_t k = 1; k < np; k++) {
                    RC_CHECK(r->pool[pc[k]].x0, >, x0);
                    RC_CHECK(r->pool[pc[k]].x0, <, x1);
                    RC_CHECK(r->pool[pc[k]].x0, >, r->pool[pc[k - 1]].x0);
                }
                pc += np;
            }
            for (uint8_t c = x0; c < x1; c++)
                covers[i][c] = 1;
        }
    }

    double th = angle * (2 * 3.14159265358979323846 / 256);
    for (uint8_t c = 0; c < PT_SCREEN_WIDTH; c++) {
        double k = (c + 0.5 - 64) / 64;
        double dx = cos(th) - k * sin(th);
        double dy = sin(th) + k * cos(th);
        for (uint8_t i = 1; i < r->num_insts; i++) {
            for (uint8_t j = (uint8_t)(i + 1); j < r->num_insts; j++) {
                if (!covers[i][c] || !covers[j][c])
                    continue;
                uint8_t celli = 0;
                uint8_t cellj = 0;
                for (uint8_t cc = 1; cc < m->num_cells; cc++) {
                    if (m->cells[cc].sprite == r->insts[i].sprite)
                        celli = cc;
                    if (m->cells[cc].sprite == r->insts[j].sprite)
                        cellj = cc;
                }
                double ti = ray_enter(m, celli, camx, camy, dx, dy);
                double tj = ray_enter(m, cellj, camx, camy, dx, dy);
                if (ti < 0 || tj < 0 || fabs(ti - tj) < 1)
                    continue;    // the column's centre misses one: a boundary column
                // the farther is drawn first
                RC_CHECK((ti > tj), ==, (order_pos[i] < order_pos[j]));
                compared_++;
            }
        }
    }
}

RC_TEST(sprite, segments_and_order)
{
    pt_arith_init();
    pt_scene_init(&map_);
    add_test_sprites(&map_);
    pt_render_init(&render_, &map_);

    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        const pt_cell *c = &map_.cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t k = 0; k < c->num_edges; k++) {
            sx += map_.verts[map_.edges[c->first_edge + k].v0].x;
            sy += map_.verts[map_.edges[c->first_edge + k].v0].y;
        }
        uint16_t x = (uint16_t)(sx / c->num_edges);
        uint16_t y = (uint16_t)(sy / c->num_edges);
        for (uint32_t a = 0; a < 256; a += 4) {
            pt_render_frame(&render_, x, y, (uint8_t)a, cell);
            check_frame(&map_, &render_, x, y, (uint8_t)a);
        }
    }

    pt_camera cam;
    pt_camera_init(&cam, &map_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);
    rc_random rng = rc_random_make(777);
    uint8_t input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        if ((rc_random_next(&rng) & 15) == 0) {
            uint32_t rr = rc_random_next(&rng);
            input = (uint8_t)(PT_IN_FWD | ((rr & 1) ? PT_IN_TURNL : 0)
                              | ((rr & 2) ? PT_IN_TURNR : 0) | ((rr & 4) ? PT_IN_LEFT : 0)
                              | ((rr & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, &map_, input);
        if ((tick & 3) == 0) {
            uint16_t x = (uint16_t)(cam.x >> 8);
            uint16_t y = (uint16_t)(cam.y >> 8);
            pt_render_frame(&render_, x, y, cam.angle, cam.cell);
            check_frame(&map_, &render_, x, y, cam.angle);
        }
    }
    printf("order checked on %u column pairs\n", (unsigned)compared_);
    RC_CHECK(compared_, >, (uint32_t)1000);    // not vacuous (2045 as written)
}

// ---- the sprite pass against a direct reference ----

// A recorded bound's row at column c, evaluated exactly and clamped to the
// screen: the plotted row, as the pass's clip rows are.
static int32_t bound_row(const pt_render *r, const uint8_t *list, uint8_t n, uint8_t c)
{
    uint8_t k = 0;
    while (k + 1 < n && r->pool[list[k + 1]].x0 <= c)
        k++;
    const pt_piece *p = &r->pool[list[k]];
    int32_t y = p->y0 * 256 + 128;
    int32_t d = (int32_t)pt_g_mag(p->g) * (c - p->x0);
    y = pt_g_isneg(p->g) ? y - d : y + d;
    int32_t row = y < 0 ? 0 : y >> 8;
    return row > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : row;
}

// Paint the frame's sprites straight into a filled image, far to near: every
// opaque source pixel whose screen pixels' centres it covers, inside its
// segments' bounds, takes its palette pattern.
static void reference_sprites(const pt_map *m, const pt_render *r, uint8_t *fb)
{
    for (uint8_t ii = r->order_head; ii != 0; ii = r->insts[ii].next) {
        const pt_inst *in = &r->insts[ii];
        const pt_sprite_art *a = &m->arts[m->sprites[in->sprite].art];
        uint8_t w = pt_sprite_width(a);
        uint8_t h = pt_sprite_height(a);
        for (uint8_t sg = in->seg; sg != 0; sg = r->segs[sg + PT_SEG_NEXT]) {
            const uint8_t *tl = &r->segs[sg + PT_SEG_PIECES];
            const uint8_t *bl = tl + r->segs[sg + PT_SEG_NT];
            for (uint8_t c = r->segs[sg + PT_SEG_X0]; c < r->segs[sg + PT_SEG_X1]; c++) {
                int32_t top = bound_row(r, tl, r->segs[sg + PT_SEG_NT], c);
                int32_t bot = bound_row(r, bl, r->segs[sg + PT_SEG_NB], c);
                int32_t j = -1;
                for (int32_t k = 0; k <= w; k++)
                    if (((in->left + k * (int32_t)in->scale + 128) >> 8) <= c)
                        j = k;
                if (j < 0 || j >= w)
                    continue;
                uint8_t pm = pt_mode5_pixmask(c);
                for (int32_t row = top; row < bot; row++) {
                    int32_t i = -1;
                    for (int32_t k = 0; k <= h; k++)
                        if (((in->top + k * (int32_t)in->scale + 128) >> 8) <= row)
                            i = k;
                    if (i < 0 || i >= h)
                        continue;
                    uint8_t code = pt_sprite_code(a, (uint8_t)j, (uint8_t)i);
                    if (code == 0)
                        continue;
                    pt_pattern pat = pt_shade(a->pal[code - 1]);
                    uint16_t off = pt_mode5_offset(c >> 2, (uint8_t)(row << 1));
                    fb[off] = (uint8_t)((fb[off] & ~pm) | (pat.e & pm));
                    fb[off + 1] = (uint8_t)((fb[off + 1] & ~pm) | (pat.o & pm));
                }
            }
        }
    }
}

static uint32_t ref_frames_;      // frames compared
static uint32_t ref_sprites_;     // of which with a sprite drawn
static uint32_t ref_mismatch_;

static void check_against_reference(uint16_t x, uint16_t y, uint8_t angle, uint8_t cell)
{
    static uint8_t want[PT_FB_BYTES];
    render_.sprites = false;
    pt_render_frame(&render_, x, y, angle, cell);
    memcpy(want, render_.fb, PT_FB_BYTES);
    render_.sprites = true;
    pt_render_frame(&render_, x, y, angle, cell);
    reference_sprites(&map_, &render_, want);
    ref_frames_++;
    if (render_.order_head != 0)
        ref_sprites_++;
    if (memcmp(want, render_.fb, PT_FB_BYTES) != 0) {
        if (ref_mismatch_ < 5)
            printf("sprite pass differs from the reference at (%u, %u) angle %u cell %u\n",
                   (unsigned)x, (unsigned)y, (unsigned)angle, (unsigned)cell);
        ref_mismatch_++;
    }
}

static void reference_poses(void)
{
    for (uint8_t cell = 1; cell < map_.num_cells; cell++) {
        const pt_cell *c = &map_.cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t k = 0; k < c->num_edges; k++) {
            sx += map_.verts[map_.edges[c->first_edge + k].v0].x;
            sy += map_.verts[map_.edges[c->first_edge + k].v0].y;
        }
        for (uint32_t a = 0; a < 256; a += 4)
            check_against_reference((uint16_t)(sx / c->num_edges), (uint16_t)(sy / c->num_edges),
                                    (uint8_t)a, cell);
    }
    pt_camera cam;
    pt_camera_init(&cam, &map_, PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE);
    rc_random rng = rc_random_make(4242);
    uint8_t input = PT_IN_FWD;
    for (uint32_t tick = 0; tick < 4000; tick++) {
        if ((rc_random_next(&rng) & 15) == 0) {
            uint32_t rr = rc_random_next(&rng);
            input = (uint8_t)(PT_IN_FWD | ((rr & 1) ? PT_IN_TURNL : 0)
                              | ((rr & 2) ? PT_IN_TURNR : 0) | ((rr & 4) ? PT_IN_LEFT : 0)
                              | ((rr & 8) ? PT_IN_BACK : 0));
        }
        pt_camera_tick(&cam, &map_, input);
        if ((tick & 3) == 0)
            check_against_reference((uint16_t)(cam.x >> 8), (uint16_t)(cam.y >> 8), cam.angle,
                                    cam.cell);
    }
}

RC_TEST(sprite, pass_matches_reference)
{
    pt_arith_init();
    ref_frames_ = ref_sprites_ = ref_mismatch_ = 0;

    // the demo scene's boulder alone, then with six more sprites overlapping
    pt_scene_init(&map_);
    pt_render_init(&render_, &map_);
    reference_poses();
    pt_scene_init(&map_);
    add_test_sprites(&map_);
    pt_render_init(&render_, &map_);
    reference_poses();

    printf("sprite pass: %u frames, %u with sprites, %u differ\n", (unsigned)ref_frames_,
           (unsigned)ref_sprites_, (unsigned)ref_mismatch_);
    RC_CHECK(ref_mismatch_, ==, (uint32_t)0);
    RC_CHECK(ref_sprites_, >, (uint32_t)500);    // not vacuous
}
