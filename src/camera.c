// camera.c - movement with sliding collision against solid walls, and cell
// transitions through portals. Positions are unsigned u16.8; distances and
// dot products are two's complement s16.8 sums whose multiplies branch on
// the sign and run as unsigned byte chains.
//
// Walls collide as exactly capped segments, and every solid wall endpoint
// is a point obstacle: when the circle overlaps one and the move approaches
// it, the move loses its radial component, leaving it tangent to the circle
// round the corner, so the camera deflects past a doorway jamb. A wall's
// infinite line or an extended cap would instead wedge movement aimed at
// the corner, since its slide direction is parallel to the wall.
//
// Which cell holds the camera is decided by angles alone: from where it
// stands, a cell's ring vertices run clockwise round it. They're the app's
// vertex cache's angles, which the frame's capture looks with too, so the
// camera's cell and the capture's view of it can never disagree.

#include "camera.h"

#include <stdbool.h>

#include "arith.h"
#include "richc/macros.h"

#define MAX_CAND 32
#define HIT_EPS 2    // whole units: truncation slack for the approach test

typedef struct cand {
    uint8_t v;         // the wall's start vertex
    uint16_t angle;    // its direction
    int16_t len;       // the foot's cap along it, its edge's len8 in units
    uint8_t capped;    // neighbour's wall: only hits in front, foot on segment
} cand;

// A tick's move changes a wall's distance by under 12 units, plus a unit
// for the position's truncation and one for each product's rounding: a wall
// farther than this at the start of the move can't be met by it.
#define FAR_SLACK 16

// Collect a corner vertex, keeping the list free of duplicates.
static void add_post(uint8_t *posts, uint8_t *np, uint8_t v)
{
    for (uint8_t k = 0; k < *np; k++)
        if (posts[k] == v)
            return;
    RC_ASSERT(*np < MAX_CAND);
    posts[(*np)++] = v;
}

// A side's direction: its stored quadrant over its edge's angle byte.
static uint16_t side_angle(const pt_map *m, const pt_cell *c, uint8_t k)
{
    return pt_map_side_angle(m, pt_map_side(m, c, k));
}

// Every wall interaction happens in the side's own frame, in whole world
// units: Q - V0 dotted with the side's direction a gives the foot's place
// along the side, and dotted with the direction a quarter turn on, towards
// the cell's inside, the interior-positive distance from its line. Two
// sine-table products each; the distance comes first, as most sides are
// out of range. The sub-unit position fraction exists only so the move
// vector can accumulate diagonal steps; against a radius of PT_CAM_RADIUS
// units the truncated unit is noise.
static int16_t side_across(const pt_map *m, uint8_t v0, uint16_t a, uint16_t x, uint16_t y)
{
    pt_vertex v = m->verts[v0];
    return pt_dot_dir(pt_i16((int32_t)x - v.x), pt_i16((int32_t)y - v.y), (uint16_t)(a + 256u));
}

// The foot of the perpendicular lies on the segment (capped a few units
// short at the far end: the endpoint regions belong to the posts).
static bool foot_in_range(const pt_map *m, const cand *cd, uint16_t x, uint16_t y)
{
    pt_vertex v = m->verts[cd->v];
    int16_t t = pt_dot_dir(pt_i16((int32_t)x - v.x), pt_i16((int32_t)y - v.y), cd->angle);
    return t >= 0 && t <= cd->len;
}

// The wall at position k of cell c's ring as a candidate: what the tests
// above need of it, looked up once.
static cand make_cand(const pt_map *m, const pt_cell *c, uint8_t k, uint8_t capped)
{
    const pt_side *sd = pt_map_side(m, c, k);
    return (cand) {.v = sd->v, .angle = side_angle(m, c, k),
                   .len = pt_i16((int32_t)m->edges[sd->edge].len8 << 3), .capped = capped};
}

// a * b for a byte b, known to fit 16 bits: two lookups, the high
// partial's own high byte (zero) dropped.
static uint16_t mulu16x8_lo(uint16_t a, uint8_t b)
{
    return pt_u16((int32_t)pt_mulu8((uint8_t)a, b) + ((int32_t)pt_mulu8((uint8_t)(a >> 8), b) << 8));
}

// A solid wall endpoint as a point obstacle. When the circle overlaps the
// vertex and the move approaches it, remove the move's radial component
// (overcorrected by a quarter, so a grazing arc drifts back out instead of
// grinding in chord by chord): the rest is tangent to the circle round the
// corner. False when the vertex leaves the move alone.
static bool corner_deflect(const pt_map *m, uint8_t v, uint32_t px, uint32_t py,
                           int16_t *mx, int16_t *my)
{
    // whole units are plenty here: the offset only matters within the radius
    int16_t ux = pt_i16((int32_t)(px >> 8) - m->verts[v].x);
    int16_t uy = pt_i16((int32_t)(py >> 8) - m->verts[v].y);
    uint16_t ax = pt_u16(ux < 0 ? -(int32_t)ux : ux);
    uint16_t ay = pt_u16(uy < 0 ? -(int32_t)uy : uy);
    if (ax >= PT_CAM_RADIUS || ay >= PT_CAM_RADIUS)
        return false;
    uint16_t d2 = pt_u16((int32_t)pt_mulu8(pt_u8(ax), pt_u8(ax)) + pt_mulu8(pt_u8(ay), pt_u8(ay)));
    if (d2 >= (uint16_t)(PT_CAM_RADIUS * PT_CAM_RADIUS) || d2 < 256)
        return false;    // clear of the vertex, or degenerately on top of it

    // M . v as a sign and a magnitude: only approach gates, and the division
    // below wants the magnitude anyway, so nothing here is ever negated. The
    // move drops to 1/64-unit steps so each product fits u16 (|M| < 16
    // units makes it 10 bits, times a byte), and so does the sum: by
    // Cauchy-Schwarz it is at most |M| * |v| / 4 < 1024 * 40
    RC_ASSERT(*mx > -4096 && *mx < 4096 && *my > -4096 && *my < 4096);
    uint16_t wx = mulu16x8_lo(pt_u16((*mx < 0 ? -(int32_t)*mx : *mx) >> 2), pt_u8(ax));
    uint16_t wy = mulu16x8_lo(pt_u16((*my < 0 ? -(int32_t)*my : *my) >> 2), pt_u8(ay));
    bool nx = (*mx < 0) != (ux < 0);
    bool ny = (*my < 0) != (uy < 0);
    uint16_t mag;
    bool toward;
    if (nx == ny) {
        mag = pt_u16((int32_t)wx + wy);
        toward = nx;
    } else if (wx >= wy) {
        mag = pt_u16((int32_t)wx - wy);
        toward = nx;
    } else {
        mag = pt_u16((int32_t)wy - wx);
        toward = ny;
    }
    if (!toward || mag == 0)
        return false;    // moving away or tangent

    // f = |M . v| / (v . v) in q14, overcorrected by a quarter: under 0.71 *
    // 1.25 of 2^14 for any move this slow. The division and the quarter are
    // one byte, pt_rpost, and a fractional multiply. The correction along v
    // is f * a / 64 in s.8, one fractional multiply by 4a (a < 40, so a
    // byte)
    uint16_t f = pt_u16((int32_t)pt_mulu16_frac(mag, pt_rpost(d2)) << 1);
    int16_t cx = pt_i16(pt_mulu16_frac(f, pt_u8((int32_t)ax << 2)));
    int16_t cy = pt_i16(pt_mulu16_frac(f, pt_u8((int32_t)ay << 2)));
    *mx = pt_i16(ux < 0 ? (int32_t)*mx - cx : (int32_t)*mx + cx);
    *my = pt_i16(uy < 0 ? (int32_t)*my - cy : (int32_t)*my + cy);
    return true;
}

// Move an unsigned position by a signed displacement.
static uint32_t apply(uint32_t p, int16_t d)
{
    if (d < 0) {
        uint16_t u = pt_u16(-(int32_t)d);
        RC_ASSERT(p >= u);
        return p - u;
    }
    return p + pt_u16(d);
}

// Wherever the tick leaves the camera: the vertex cache looks from there
// (emptied if that's a new whole-unit position), and the cell is the one
// its angles put the centre in, hopping across any portal side it has
// crossed. The frame's capture then looks with the same cache.
static void transition(pt_camera *c, const pt_map *m, pt_vcache *vc)
{
    pt_vcache_move(vc, pt_u16((int32_t)(c->x >> 8)), pt_u16((int32_t)(c->y >> 8)));
    c->cell = pt_vcache_locate(vc, m, c->cell);
}

void pt_camera_init(pt_camera *c, const pt_map *m, pt_vcache *vc, uint16_t x, uint16_t y,
                    uint8_t angle)
{
    uint8_t cell = pt_map_find_cell(m, x, y);
    RC_ASSERT(cell != 0);

    *c = (pt_camera) {
        .x = (uint32_t)x << 8,
        .y = (uint32_t)y << 8,
        .angle = angle,
        .cell = cell,
    };
    pt_vcache_move(vc, x, y);
}

void pt_camera_tick(pt_camera *c, const pt_map *m, pt_vcache *vc, uint8_t input)
{
    if (input & PT_IN_TURNL)
        c->angle = (uint8_t)(c->angle - PT_CAM_TURN_SPEED);
    if (input & PT_IN_TURNR)
        c->angle = (uint8_t)(c->angle + PT_CAM_TURN_SPEED);

    // desired displacement from the input: the facing step, exact (a whole
    // number of units along the camera's angle, four 1024ths a step)
    pt_xy f = pt_dir((int16_t)(PT_CAM_MOVE_SPEED << 8), (uint16_t)(c->angle << 2));
    int16_t fx = f.x;
    int16_t fy = f.y;
    int16_t mx = 0;    // s.8, under 12 units in any direction
    int16_t my = 0;
    if (input & PT_IN_FWD) {
        mx = pt_i16((int32_t)mx + fx);
        my = pt_i16((int32_t)my + fy);
    }
    if (input & PT_IN_BACK) {
        mx = pt_i16((int32_t)mx - fx);
        my = pt_i16((int32_t)my - fy);
    }
    if (input & PT_IN_RIGHT) {
        // right of facing is the direction a quarter turn on: (-sin, cos)
        mx = pt_i16((int32_t)mx - fy);
        my = pt_i16((int32_t)my + fx);
    }
    if (input & PT_IN_LEFT) {
        mx = pt_i16((int32_t)mx + fy);
        my = pt_i16((int32_t)my - fx);
    }

    // standing still where the last tick did, its resolve pushing nothing:
    // this one's would find nothing either, and the cell's the same
    if (mx == 0 && my == 0 && c->settled)
        return;

    const pt_cell *cc = &m->cells[c->cell];
    uint16_t px = pt_u16((int32_t)(c->x >> 8));
    uint16_t py = pt_u16((int32_t)(c->y >> 8));

    // 0. candidate walls (ours, plus every overlapped neighbour's) and the
    // corner vertices at their endpoints, collected while the ring (and so
    // the wall's end vertex) is in hand
    cand cands[MAX_CAND];
    uint8_t nc = 0;
    uint8_t posts[MAX_CAND];
    uint8_t np = 0;
    for (uint8_t k = 0; k <= cc->num; k++) {
        const pt_side *sd = pt_map_side(m, cc, k);
        uint16_t a = side_angle(m, cc, k);
        if (sd->adj == 0) {
            RC_ASSERT(nc < MAX_CAND);
            cands[nc++] = make_cand(m, cc, k, 0);
            add_post(posts, &np, sd->v);
            add_post(posts, &np, pt_map_side_v1(m, cc, k));
        } else if (side_across(m, sd->v, a, px, py) < PT_CAM_RADIUS) {
            const pt_cell *nb = &m->cells[sd->adj];
            for (uint8_t j = 0; j <= nb->num; j++) {
                const pt_side *nd = pt_map_side(m, nb, j);
                if (nd->adj == 0) {
                    RC_ASSERT(nc < MAX_CAND);
                    cands[nc++] = make_cand(m, nb, j, 1);
                    add_post(posts, &np, nd->v);
                    add_post(posts, &np, pt_map_side_v1(m, nb, j));
                }
            }
        }
    }

    // 1. resolve: restore the invariant if a slide or transition left us too
    // close; neighbour walls and posts count too, or a corner shared with
    // the next cell wedges the slide before its own resolve can see it. The
    // slide below wants each candidate's distance from where it starts:
    // these, if nothing pushes, else made again on first need (INT16_MIN
    // marks not yet made; no distance comes near it)
    int16_t cur_y[MAX_CAND];
    bool pushed = false;
    for (uint8_t i = 0; i < nc; i++) {
        uint16_t a = cands[i].angle;
        uint16_t qx = pt_u16((int32_t)(c->x >> 8));
        uint16_t qy = pt_u16((int32_t)(c->y >> 8));
        int16_t across = side_across(m, cands[i].v, a, qx, qy);
        cur_y[i] = across;
        if (across >= PT_CAM_RADIUS)
            continue;
        if (cands[i].capped && across < 0)
            continue;
        if (!foot_in_range(m, &cands[i], qx, qy))
            continue;
        // the push: whole units shifted to s.8 along the inward normal, the
        // direction a quarter turn on from the side's
        RC_ASSERT(PT_CAM_RADIUS - across <= 127);
        pt_xy pv = pt_dir(pt_i16((PT_CAM_RADIUS - (int32_t)across) << 8), (uint16_t)(a + 256u));
        c->x = apply(c->x, pv.x);
        c->y = apply(c->y, pv.y);
        pushed = true;
    }
    if (pushed)
        for (uint8_t i = 0; i < nc; i++)
            cur_y[i] = INT16_MIN;
    c->settled = !pushed && mx == 0 && my == 0;
    if (mx == 0 && my == 0) {
        transition(c, m, vc);    // the resolve may still have pushed it
        return;
    }

    // 2. move with sliding: the worst violated wall claims the move and
    // keeps only its tangent component. A wall counts only if the move
    // brings the centre closer than it already is, so a tangent graze never
    // wedges. With no wall in the way, a corner vertex may still deflect
    // the move; the next iteration then re-checks the walls.
    bool blocked = false;
    uint16_t sx = pt_u16((int32_t)(c->x >> 8));    // where the move starts
    uint16_t sy = pt_u16((int32_t)(c->y >> 8));
    for (uint8_t iter = 0; iter < 3; iter++) {
        uint16_t qx = pt_u16((int32_t)(apply(c->x, mx) >> 8));
        uint16_t qy = pt_u16((int32_t)(apply(c->y, my) >> 8));
        int16_t worst = PT_CAM_RADIUS;
        uint16_t hit = 0;
        bool have_hit = false;
        for (uint8_t i = 0; i < nc; i++) {
            if (cur_y[i] != INT16_MIN && cur_y[i] >= PT_CAM_RADIUS + FAR_SLACK)
                continue;    // too far to reach this tick
            uint16_t a = cands[i].angle;
            int16_t across = side_across(m, cands[i].v, a, qx, qy);
            if (cands[i].capped && across < 0)
                continue;
            if (across >= worst)
                continue;
            if (!foot_in_range(m, &cands[i], qx, qy))
                continue;
            if (cur_y[i] == INT16_MIN)
                cur_y[i] = side_across(m, cands[i].v, a, sx, sy);
            if (across + HIT_EPS >= cur_y[i])
                continue;
            worst = across;
            hit = a;
            have_hit = true;
        }
        if (have_hit) {
            blocked = true;

            // slide: the move keeps only its component along the wall
            int16_t along = pt_dot_dir(mx, my, hit);
            pt_xy mv = pt_dir(along, hit);
            mx = mv.x;
            my = mv.y;
            continue;
        }
        blocked = false;
        bool defl = false;
        for (uint8_t i = 0; i < np; i++)
            defl |= corner_deflect(m, posts[i], c->x, c->y, &mx, &my);
        if (!defl)
            break;
    }
    if (blocked) {
        mx = 0;
        my = 0;
    }
    c->x = apply(c->x, mx);
    c->y = apply(c->y, my);

    // 3. transition
    transition(c, m, vc);
}
