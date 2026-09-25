// camera.c - movement with sliding collision against solid walls, and cell
// transitions through portals (spec section 10). Positions are unsigned
// u16.8; distances and dot products are two's complement s16.8 sums whose
// multiplies branch on the sign and run as unsigned byte chains.
//
// Walls collide as exactly capped segments, and every solid wall endpoint
// is a point obstacle: when the circle overlaps one and the move approaches
// it, the move loses its radial component, leaving it tangent to the circle
// round the corner, so the camera deflects past a doorway jamb. A wall's
// infinite line (spec 10.2) or an extended cap would instead wedge movement
// aimed at the corner, since its slide direction is parallel to the wall.

#include "camera.h"

#include <stdbool.h>

#include "arith.h"
#include "richc/macros.h"

#define MAX_CAND 32
#define RADIUS_Q8 ((int32_t)PT_CAM_RADIUS << 8)
#define HIT_EPS 4    // s16.8: rounding slack for the approach test

typedef struct cand {
    uint8_t edge;
    uint8_t capped;    // neighbour's wall: only hits in front, foot on segment
} cand;

// (v * f) >> 8 for an unsigned 24-bit value and a u0.8 factor (256 meaning
// one): the 6502's u24 x u8 keep-high byte chain.
static uint32_t mul24u_frac(uint32_t v, uint16_t f)
{
    RC_ASSERT(f <= 256);
    RC_ASSERT(v < (1u << 24));

    if (f == 256)
        return v;
    uint32_t m = (uint32_t)(pt_mulu8((uint8_t)v, (uint8_t)f) >> 8);
    m += pt_mulu8((uint8_t)(v >> 8), (uint8_t)f);
    m += (uint32_t)pt_mulu8((uint8_t)(v >> 16), (uint8_t)f) << 8;
    return m;
}

// (q * s) >> 8 for an unsigned u16.8 position and a signed s0.8 component:
// the sign of s picks the branch.
static int32_t mulq_s8(uint32_t q, int16_t s)
{
    if (s < 0)
        return -(int32_t)mul24u_frac(q, (uint16_t)-s);
    return (int32_t)mul24u_frac(q, (uint16_t)s);
}

// (v * s) >> 8 for a signed s16.8 value and a signed s0.8 component: the
// sign pair picks the branch, the multiply is unsigned.
static int32_t muls24_s8(int32_t v, int16_t s)
{
    uint32_t uv = (uint32_t)(v < 0 ? -v : v);
    uint16_t f = (uint16_t)(s < 0 ? -s : s);
    uint32_t m = mul24u_frac(uv, f);
    return ((v < 0) != (s < 0)) ? -(int32_t)m : (int32_t)m;
}

// Signed distance (s16.8) from Q to the edge's line; positive on the
// interior side.
static int32_t edge_dist(const pt_map *m, uint8_t e, uint32_t qx, uint32_t qy)
{
    const pt_edge *ed = &m->edges[e];
    return mulq_s8(qx, ed->nx) + mulq_s8(qy, ed->ny) - ed->d0;
}

// Position (s16.8) of the foot of the perpendicular along the edge from V0.
static int32_t edge_t(const pt_map *m, uint8_t e, uint32_t qx, uint32_t qy)
{
    const pt_edge *ed = &m->edges[e];
    int32_t rx = (int32_t)(qx - ((uint32_t)m->verts[ed->v0].x << 8));
    int32_t ry = (int32_t)(qy - ((uint32_t)m->verts[ed->v0].y << 8));
    return muls24_s8(rx, ed->ex) + muls24_s8(ry, ed->ey);
}

// The foot of the perpendicular lies on the segment (exactly capped: the
// endpoint regions belong to the posts).
static bool foot_in_range(const pt_map *m, uint8_t e, int32_t t)
{
    return t >= 0 && t <= ((int32_t)m->edges[e].len << 8);
}

// The full signed product of a s16.8 value and a small whole-unit count.
static int32_t mul_su(int32_t a, int32_t b)
{
    uint32_t ua = (uint32_t)(a < 0 ? -a : a);
    uint16_t ub = (uint16_t)(b < 0 ? -b : b);
    RC_ASSERT(ua < (1u << 16) && ub <= 256);
    uint32_t p = mul24u_frac(ua << 8, ub);
    return ((a < 0) != (b < 0)) ? -(int32_t)p : (int32_t)p;
}

// A solid wall endpoint as a point obstacle. When the circle overlaps the
// vertex and the move approaches it, remove the move's radial component
// (overcorrected by a quarter, so a grazing arc drifts back out instead of
// grinding in chord by chord): the rest is tangent to the circle round the
// corner. False when the vertex leaves the move alone.
static bool corner_deflect(const pt_map *m, uint8_t v, uint32_t px, uint32_t py,
                           int32_t *mx, int32_t *my)
{
    // whole units are plenty here: the offset only matters within the radius
    int32_t ux = (int32_t)(px >> 8) - m->verts[v].x;
    int32_t uy = (int32_t)(py >> 8) - m->verts[v].y;
    int32_t ax = ux < 0 ? -ux : ux;
    int32_t ay = uy < 0 ? -uy : uy;
    if (ax >= PT_CAM_RADIUS || ay >= PT_CAM_RADIUS)
        return false;
    uint16_t d2 = (uint16_t)(pt_mulu8((uint8_t)ax, (uint8_t)ax)
                             + pt_mulu8((uint8_t)ay, (uint8_t)ay));
    if (d2 >= (uint16_t)(PT_CAM_RADIUS * PT_CAM_RADIUS) || d2 < 256)
        return false;    // clear of the vertex, or degenerately on top of it

    int32_t dot = mul_su(*mx, ux) + mul_su(*my, uy);
    if (dot >= 0)
        return false;    // moving away or tangent

    // f = (M . v) / (v . v) in q16, then a q8 correction along v
    uint32_t f = pt_div_q8((uint32_t)-dot, d2);
    f += f >> 2;
    if (f > 0xFFFF)
        f = 0xFFFF;
    int32_t cx = (int32_t)mul24u_frac(f, (uint16_t)ax);
    int32_t cy = (int32_t)mul24u_frac(f, (uint16_t)ay);
    *mx += ux < 0 ? -cx : cx;
    *my += uy < 0 ? -cy : cy;
    return true;
}

// Move an unsigned position by a signed displacement.
static uint32_t apply(uint32_t p, int32_t d)
{
    if (d < 0) {
        RC_ASSERT(p >= (uint32_t)-d);
        return p - (uint32_t)-d;
    }
    return p + (uint32_t)d;
}

void pt_camera_init(pt_camera *c, const pt_map *m, uint16_t x, uint16_t y, uint8_t angle)
{
    uint8_t cell = pt_map_find_cell(m, x, y);
    RC_ASSERT(cell != 0);

    *c = (pt_camera) {
        .x = (uint32_t)x << 8,
        .y = (uint32_t)y << 8,
        .angle = angle,
        .cell = cell,
    };
}

void pt_camera_tick(pt_camera *c, const pt_map *m, uint8_t input)
{
    if (input & PT_IN_TURNL)
        c->angle = (uint8_t)(c->angle - PT_CAM_TURN_SPEED);
    if (input & PT_IN_TURNR)
        c->angle = (uint8_t)(c->angle + PT_CAM_TURN_SPEED);

    const pt_cell *cc = &m->cells[c->cell];

    // 0. candidate walls (ours, plus every overlapped neighbour's) and the
    // corner vertices at their endpoints
    cand cands[MAX_CAND];
    uint8_t nc = 0;
    uint8_t posts[MAX_CAND];
    uint8_t np = 0;
    for (uint8_t k = 0; k < cc->num_edges; k++) {
        uint8_t ei = (uint8_t)(cc->first_edge + k);
        const pt_edge *ed = &m->edges[ei];
        if (ed->adj == 0) {
            RC_ASSERT(nc < MAX_CAND);
            cands[nc++] = (cand) {.edge = ei, .capped = 0};
        } else if (edge_dist(m, ei, c->x, c->y) < RADIUS_Q8) {
            const pt_cell *nb = &m->cells[ed->adj];
            for (uint8_t j = 0; j < nb->num_edges; j++) {
                uint8_t nj = (uint8_t)(nb->first_edge + j);
                if (m->edges[nj].adj == 0) {
                    RC_ASSERT(nc < MAX_CAND);
                    cands[nc++] = (cand) {.edge = nj, .capped = 1};
                }
            }
        }
    }
    for (uint8_t i = 0; i < nc; i++) {
        uint8_t vs[2];
        vs[0] = m->edges[cands[i].edge].v0;
        vs[1] = m->edges[pt_map_ring_next(m, cands[i].edge)].v0;
        for (uint8_t j = 0; j < 2; j++) {
            uint8_t v = vs[j];
            uint8_t k = 0;
            while (k < np && posts[k] != v)
                k++;
            if (k == np) {
                RC_ASSERT(np < MAX_CAND);
                posts[np++] = v;
            }
        }
    }

    // 1. resolve: restore the invariant if a slide or transition left us too
    // close; neighbour walls and posts count too, or a corner shared with
    // the next cell wedges the slide before its own resolve can see it
    for (uint8_t i = 0; i < nc; i++) {
        uint8_t ei = cands[i].edge;
        const pt_edge *ed = &m->edges[ei];
        int32_t d = edge_dist(m, ei, c->x, c->y);
        if (d >= RADIUS_Q8)
            continue;
        if (cands[i].capped && d < 0)
            continue;
        if (!foot_in_range(m, ei, edge_t(m, ei, c->x, c->y)))
            continue;
        int32_t push = RADIUS_Q8 - d;
        c->x = apply(c->x, muls24_s8(push, ed->nx));
        c->y = apply(c->y, muls24_s8(push, ed->ny));
    }
    // desired displacement from the input
    int16_t dc = pt_cos(c->angle);
    int16_t ds = pt_sin(c->angle);
    int32_t mx = 0;
    int32_t my = 0;
    if (input & PT_IN_FWD) {
        mx += mulq_s8(PT_CAM_MOVE_SPEED, dc);
        my += mulq_s8(PT_CAM_MOVE_SPEED, ds);
    }
    if (input & PT_IN_BACK) {
        mx -= mulq_s8(PT_CAM_MOVE_SPEED, dc);
        my -= mulq_s8(PT_CAM_MOVE_SPEED, ds);
    }
    if (input & PT_IN_RIGHT) {
        // right of facing is the direction at angle + 64: (-sin, cos)
        mx -= mulq_s8(PT_CAM_MOVE_SPEED, ds);
        my += mulq_s8(PT_CAM_MOVE_SPEED, dc);
    }
    if (input & PT_IN_LEFT) {
        mx += mulq_s8(PT_CAM_MOVE_SPEED, ds);
        my -= mulq_s8(PT_CAM_MOVE_SPEED, dc);
    }
    if (mx == 0 && my == 0)
        return;

    // 2. move with sliding: the worst violated wall claims the move and
    // keeps only its tangent component. A wall counts only if the move
    // brings the centre closer than it already is, so a tangent graze never
    // wedges. With no wall in the way, a corner vertex may still deflect
    // the move; the next iteration then re-checks the walls.
    bool blocked = false;
    for (uint8_t iter = 0; iter < 3; iter++) {
        uint32_t qx = apply(c->x, mx);
        uint32_t qy = apply(c->y, my);
        int32_t worst = RADIUS_Q8;
        uint8_t hit = 0;
        bool have_hit = false;
        for (uint8_t i = 0; i < nc; i++) {
            uint8_t ei = cands[i].edge;
            int32_t d = edge_dist(m, ei, qx, qy);
            if (cands[i].capped && d < 0)
                continue;
            if (!foot_in_range(m, ei, edge_t(m, ei, qx, qy)))
                continue;
            if (d + HIT_EPS >= edge_dist(m, ei, c->x, c->y))
                continue;
            if (d < worst) {
                worst = d;
                hit = ei;
                have_hit = true;
            }
        }
        if (have_hit) {
            blocked = true;

            // slide: keep only the component along the wall
            const pt_edge *ed = &m->edges[hit];
            int32_t along = muls24_s8(mx, ed->ex) + muls24_s8(my, ed->ey);
            mx = muls24_s8(along, ed->ex);
            my = muls24_s8(along, ed->ey);
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

    // 3. transition: the centre has crossed a portal line
    for (uint8_t pass = 0; pass < 4; pass++) {
        const pt_cell *cur = &m->cells[c->cell];
        bool crossed = false;
        for (uint8_t k = 0; k < cur->num_edges; k++) {
            uint8_t ei = (uint8_t)(cur->first_edge + k);
            if (m->edges[ei].adj == 0)
                continue;
            if (edge_dist(m, ei, c->x, c->y) < 0) {
                c->cell = m->edges[ei].adj;
                crossed = true;
                break;
            }
        }
        if (!crossed)
            break;
    }
}
