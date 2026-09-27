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
#define HIT_EPS 2    // whole units: truncation slack for the approach test

typedef struct cand {
    uint8_t edge;
    uint8_t capped;    // neighbour's wall: only hits in front, foot on segment
} cand;

// Collect a corner vertex, keeping the list free of duplicates.
static void add_post(uint8_t *posts, uint8_t *np, uint8_t v)
{
    for (uint8_t k = 0; k < *np; k++)
        if (posts[k] == v)
            return;
    RC_ASSERT(*np < MAX_CAND);
    posts[(*np)++] = v;
}

// (v * f) >> 8 for an unsigned 24-bit value and a u0.8 factor (256 meaning
// one, forked here; the chain itself is arith's).
static uint32_t mul24u_frac(uint32_t v, uint16_t f)
{
    RC_ASSERT(f <= 256);
    RC_ASSERT(v < (1u << 24));

    if (f == 256)
        return v;
    return pt_mulu24_frac(v, (uint8_t)f);
}

// Every wall interaction happens in the edge's own frame, in whole world
// units: rotating Q - V0 by the edge angle (the renderer's own primitive)
// gives the foot position along the edge as x and the interior-positive
// distance as y in one operation. The sub-unit position fraction exists only
// so the move vector can accumulate diagonal steps; against a radius of
// PT_CAM_RADIUS units the truncated unit is noise.
static pt_xy edge_frame(const pt_map *m, uint8_t e, uint16_t x, uint16_t y)
{
    const pt_edge *ed = &m->edges[e];
    int16_t rx = (int16_t)(x - m->verts[ed->v0].x);
    int16_t ry = (int16_t)(y - m->verts[ed->v0].y);
    return pt_rot(rx, ry, ed->angle);
}

// The foot of the perpendicular lies on the segment (capped a few units
// short at the far end: the endpoint regions belong to the posts).
static bool foot_in_range(const pt_map *m, uint8_t e, int16_t t)
{
    return t >= 0 && t <= (int16_t)((uint16_t)m->edges[e].len8 << 3);
}

// |a| * |b| mod 2^16: three lookups, the high partial dropped. Exact by
// wraparound whenever the accumulated result is known to fit s16.
static uint16_t mul16_lo(uint16_t a, uint16_t b)
{
    uint16_t lo = pt_mulu8((uint8_t)a, (uint8_t)b);
    uint16_t mid = (uint16_t)(pt_mulu8((uint8_t)a, (uint8_t)(b >> 8))
                              + pt_mulu8((uint8_t)(a >> 8), (uint8_t)b));
    return (uint16_t)(lo + (uint16_t)(mid << 8));
}

// The side of Q against the exact line through the edge's two vertices (the
// same sign convention as the frame distance), valid only within a few units
// of the line: there the true cross product (dx qy - dy qx, perpendicular
// distance times edge length) fits s16, so every product can be kept mod
// 2^16 and the wraparound is exact. The twin's value is the exact negation
// about the same two vertices, so both cells always agree where the boundary
// lies.
static int16_t edge_cross_band(const pt_map *m, const pt_cell *c, uint8_t e, uint16_t x,
                               uint16_t y)
{
    const pt_edge *ed = &m->edges[e];
    pt_vertex a = m->verts[ed->v0];
    pt_vertex b = m->verts[m->edges[pt_map_ring_next(c, e)].v0];
    int16_t dx = (int16_t)(b.x - a.x);
    int16_t dy = (int16_t)(b.y - a.y);
    int16_t qx = (int16_t)(x - a.x);
    int16_t qy = (int16_t)(y - a.y);

    uint16_t p1 = mul16_lo((uint16_t)(dx < 0 ? -dx : dx), (uint16_t)(qy < 0 ? -qy : qy));
    uint16_t p2 = mul16_lo((uint16_t)(dy < 0 ? -dy : dy), (uint16_t)(qx < 0 ? -qx : qx));
    bool n1 = (dx < 0) != (qy < 0);
    bool n2 = !((dy < 0) != (qx < 0));    // subtracted: its sign enters flipped
    if (n1 != n2)
        return n1 ? (int16_t)(p2 - p1) : (int16_t)(p1 - p2);
    uint16_t m2 = (uint16_t)(p1 + p2);
    return n1 ? (int16_t)-m2 : (int16_t)m2;
}

// Has the centre crossed this portal? Away from the line the frame distance
// is decisive: the margin covers its rounding plus the quantised angle's
// tilt about V0, which grows with the distance |t| along the line in BOTH
// directions (t/64 dominates the tilt's pi/256 per unit) - on the backward
// extension as much as on the edge, or a pose out there could hop, or fail
// to hop, against the exact truth. Within the band the exact vertex-line
// side decides, identically from both cells, so every point has a definite
// owner and the hop walk can neither cycle nor stick; fan lines concur
// exactly at their shared vertex for the same reason.
static bool portal_crossed(const pt_map *m, const pt_cell *c, uint8_t e, uint16_t x, uint16_t y)
{
    pt_xy f = edge_frame(m, e, x, y);
    int16_t ax = f.x < 0 ? (int16_t)-f.x : f.x;
    int16_t margin = (int16_t)(2 + (ax >> 6));
    if (f.y >= margin)
        return false;
    if (f.y < -margin)
        return true;
    return edge_cross_band(m, c, e, x, y) < 0;
}

// The full signed product of a s16.8 value and a small whole-unit count.
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

    // M . v as a sign and a magnitude: only approach gates, and the division
    // below wants the magnitude anyway, so nothing here is ever negated
    uint32_t wx = mul24u_frac((uint32_t)(*mx < 0 ? -*mx : *mx) << 8, (uint16_t)ax);
    uint32_t wy = mul24u_frac((uint32_t)(*my < 0 ? -*my : *my) << 8, (uint16_t)ay);
    RC_ASSERT(wx < (1u << 24) && wy < (1u << 24));
    bool nx = (*mx < 0) != (ux < 0);
    bool ny = (*my < 0) != (uy < 0);
    uint32_t mag;
    bool toward;
    if (nx == ny) {
        mag = wx + wy;
        toward = nx;
    } else if (wx >= wy) {
        mag = wx - wy;
        toward = nx;
    } else {
        mag = wy - wx;
        toward = ny;
    }
    if (!toward || mag == 0)
        return false;    // moving away or tangent

    // f = |M . v| / (v . v) in q16, then a q8 correction along v
    uint32_t f = pt_div_q8(mag, d2);
    f += f >> 2;
    if (f > 0xFFFF)
        f = 0xFFFF;
    uint32_t cx = mul24u_frac(f, (uint16_t)ax);
    uint32_t cy = mul24u_frac(f, (uint16_t)ay);
    if (ux < 0)
        *mx -= (int32_t)cx;
    else
        *mx += (int32_t)cx;
    if (uy < 0)
        *my -= (int32_t)cy;
    else
        *my += (int32_t)cy;
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
    // corner vertices at their endpoints, collected while the ring (and so
    // the wall's end vertex) is in hand
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
            add_post(posts, &np, ed->v0);
            add_post(posts, &np, m->edges[pt_map_ring_next(cc, ei)].v0);
        } else if (edge_frame(m, ei, (uint16_t)(c->x >> 8), (uint16_t)(c->y >> 8)).y
                   < PT_CAM_RADIUS) {
            const pt_cell *nb = &m->cells[ed->adj];
            for (uint8_t j = 0; j < nb->num_edges; j++) {
                uint8_t nj = (uint8_t)(nb->first_edge + j);
                if (m->edges[nj].adj == 0) {
                    RC_ASSERT(nc < MAX_CAND);
                    cands[nc++] = (cand) {.edge = nj, .capped = 1};
                    add_post(posts, &np, m->edges[nj].v0);
                    add_post(posts, &np, m->edges[pt_map_ring_next(nb, nj)].v0);
                }
            }
        }
    }

    // 1. resolve: restore the invariant if a slide or transition left us too
    // close; neighbour walls and posts count too, or a corner shared with
    // the next cell wedges the slide before its own resolve can see it
    for (uint8_t i = 0; i < nc; i++) {
        uint8_t ei = cands[i].edge;
        const pt_edge *ed = &m->edges[ei];
        pt_xy f = edge_frame(m, ei, (uint16_t)(c->x >> 8), (uint16_t)(c->y >> 8));
        if (f.y >= PT_CAM_RADIUS)
            continue;
        if (cands[i].capped && f.y < 0)
            continue;
        if (!foot_in_range(m, ei, f.x))
            continue;
        // the push is a positive byte stepped along the inward normal: the
        // direction a quarter turn on from the edge
        RC_ASSERT(PT_CAM_RADIUS - f.y <= 127);
        pt_xy pv = pt_dir8((uint8_t)(PT_CAM_RADIUS - f.y), (uint8_t)(ed->angle + 64));
        c->x = apply(c->x, pv.x);
        c->y = apply(c->y, pv.y);
    }
    // desired displacement from the input: the facing step, one exact 8-bit
    // product per component
    pt_xy f = pt_dir8(PT_CAM_MOVE_SPEED, c->angle);
    int16_t fx = f.x;
    int16_t fy = f.y;
    int32_t mx = 0;
    int32_t my = 0;
    if (input & PT_IN_FWD) {
        mx += fx;
        my += fy;
    }
    if (input & PT_IN_BACK) {
        mx -= fx;
        my -= fy;
    }
    if (input & PT_IN_RIGHT) {
        // right of facing is the direction at angle + 64: (-sin, cos)
        mx -= fy;
        my += fx;
    }
    if (input & PT_IN_LEFT) {
        mx += fy;
        my -= fx;
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
        uint16_t qx = (uint16_t)(apply(c->x, mx) >> 8);
        uint16_t qy = (uint16_t)(apply(c->y, my) >> 8);
        int16_t worst = PT_CAM_RADIUS;
        uint8_t hit = 0;
        bool have_hit = false;
        for (uint8_t i = 0; i < nc; i++) {
            uint8_t ei = cands[i].edge;
            pt_xy f = edge_frame(m, ei, qx, qy);
            if (cands[i].capped && f.y < 0)
                continue;
            if (!foot_in_range(m, ei, f.x))
                continue;
            if (f.y + HIT_EPS >= edge_frame(m, ei, (uint16_t)(c->x >> 8), (uint16_t)(c->y >> 8)).y)
                continue;
            if (f.y < worst) {
                worst = f.y;
                hit = ei;
                have_hit = true;
            }
        }
        if (have_hit) {
            blocked = true;

            // slide: the move in the wall's frame keeps only its along
            // component, rotated back out (the reverse angle)
            const pt_edge *ed = &m->edges[hit];
            RC_ASSERT(mx > INT16_MIN && mx <= INT16_MAX);
            RC_ASSERT(my > INT16_MIN && my <= INT16_MAX);
            int16_t along = pt_rot((int16_t)mx, (int16_t)my, ed->angle).x;
            pt_xy mv = pt_rot(along, 0, (uint8_t)-ed->angle);
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

    // 3. transition: the centre has crossed a portal line. The walk hops
    // through any violated portal and repeats; a move skimming a fan vertex
    // may cross several cells, and can legally walk the long way round the
    // fan, so the cap allows a full ring of hops.
    for (uint8_t pass = 0; pass < 8; pass++) {
        const pt_cell *cur = &m->cells[c->cell];
        bool crossed = false;
        for (uint8_t k = 0; k < cur->num_edges; k++) {
            uint8_t ei = (uint8_t)(cur->first_edge + k);
            if (m->edges[ei].adj == 0)
                continue;
            if (portal_crossed(m, cur, ei, (uint16_t)(c->x >> 8), (uint16_t)(c->y >> 8))) {
                c->cell = m->edges[ei].adj;
                crossed = true;
                break;
            }
        }
        if (!crossed)
            break;
    }
}
