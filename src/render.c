// render.c - the capture walk (spec section 7), line generation and bound
// clipping, EOR plotting and the fill. All per-frame arithmetic goes through
// the table helpers in arith.h; sums and differences are two's complement,
// while every multiply and divide runs on unsigned magnitudes.

#include "render.h"

#include <stdbool.h>

// clip statuses: where a line range sits relative to the portal bounds
enum {
    ST_ABOVE,      // hidden above the top bound: merge into the top piece
    ST_BELOW,      // hidden below the bottom bound: merge into the bottom piece
    ST_VISIBLE,    // between the bounds: allocate a fresh piece
    ST_OPEN,       // resolved against the top only, bottom still to do
};

// clip roles: which bound chain the line being clipped is forming
enum {
    ROLE_NONE,      // wall or recolour-only line
    ROLE_BOTTOM,    // floor-forming line, building the child's bottom chain
    ROLE_TOP,       // ceiling-forming line, building the child's top chain
};

// A candidate line from make_line, before clipping against the bounds.
typedef struct pt_line {
    uint8_t x0;
    uint8_t x1;
    uint8_t y0;
    uint16_t g;    // sign bit + u7.8 magnitude
    uint8_t ce;
    uint8_t co;
} pt_line;

// A side-clipped edge endpoint: depth, lateral, screen column, and the
// depth's reciprocal for the row projections.
typedef struct pt_ep {
    uint16_t x;
    int16_t y;
    uint8_t sx;
    pt_recip rx;
} pt_ep;

// A chain under construction (childT or childB) with its row extents.
typedef struct pt_chainb {
    uint8_t head;
    uint8_t tail;
    uint8_t tmax;    // ceil of the largest row over any piece
    uint8_t bmin;    // floor of the smallest row over any piece
} pt_chainb;

// Closed column ranges of the current edge (nothing of the child visible).
typedef struct pt_closedl {
    uint8_t l[PT_MAX_CLOSED];
    uint8_t r[PT_MAX_CLOSED];
    uint8_t num;
} pt_closedl;

// Everything one clip_line call needs: the two bound chains, their extents,
// the chain being built and the closed-range fixup colour.
typedef struct pt_clip {
    pt_render *r;
    uint8_t thead;
    uint8_t bhead;      // 0 means an empty bottom chain (all closed)
    uint8_t tmax;
    uint8_t bmin;
    uint8_t role;
    pt_chainb *build;
    pt_pattern cbfb;    // child ceiling ^ child floor, the closed-range stand-in
    pt_closedl *closed;
    pt_visit *vis;      // the cell's visit: its active pieces
    uint8_t slot;       // which of the edge's lines is being clipped

    // Walk cursors, persistent across the pieces of one logical line: a merge
    // may relink a bound piece into a child chain, severing the chain behind
    // the cursor, so a later piece must continue forward from here rather
    // than re-seek from the head.
    uint8_t tp;         // 0 = not yet positioned
    bool bp_set;
    uint8_t bp;
} pt_clip;

// ---- row arithmetic ----
//
// Every reconstructed u8.8 row (piece_y, line_y, the plot DDA) carries the
// +128 rounding bias in its anchor: seeding the low byte with 128 is a free
// constant load, and rounding to the nearest row becomes a plain high-byte
// read (row_of). Re-anchoring must round rather than truncate: truncation
// always loses row fraction in the same direction, and chained splits would
// compound the drift beyond the one-row slack the parity property absorbs.
// The bias cancels in every line-vs-bound difference; only the conservative
// extents peel it off again, so floor is (y - 128) >> 8 and ceil is
// (y + 127) >> 8.

// y plus or minus g*dx: the add saturates on carry out, the subtract clamps
// at the screen top (a toggle pushed past the top is still active at row 0).
static uint16_t row_step(uint16_t y, uint16_t g, uint8_t dx)
{
    // at the anchor column or on a flat line there is nothing to multiply
    if (dx == 0 || pt_g_mag(g) == 0)
        return y;
    uint16_t d = pt_mulg_u16(pt_g_mag(g), dx);
    if (pt_g_isneg(g))
        return d > y ? 0 : (uint16_t)(y - d);
    uint16_t s = (uint16_t)(y + d);
    return s < y ? 0xFFFF : s;
}

// The nearest row to a biased u8.8 value: the high byte.
static uint8_t row_of(uint16_t y_b)
{
    uint8_t row = (uint8_t)(y_b >> 8);
    return row > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : row;
}

// The row a biased u8.8 value truncates into, for conservative extents.
static uint8_t row_of_floor(uint16_t y_b)
{
    return row_of(y_b < 128 ? 0 : (uint16_t)(y_b - 128));
}

// The row a biased u8.8 value rounds up into, for conservative extents.
static uint8_t row_of_ceil(uint16_t y_b)
{
    uint16_t s = (uint16_t)(y_b + 127);
    return row_of(s < y_b ? 0xFFFF : s);
}

// ---- pool ----

static uint8_t pool_alloc(pt_render *r, uint8_t x0, uint8_t x1, uint8_t y0, uint16_t g,
                          uint8_t ce, uint8_t co)
{
    RC_PANIC(r->pool_num < PT_POOL_SIZE);
    RC_ASSERT(x0 < x1);

    uint8_t i = (uint8_t)r->pool_num++;
    r->pool[i] = (pt_piece) {.x0 = x0, .x1 = x1, .y0 = y0, .g = g, .ce = ce, .co = co, .next = 0};
    return i;
}

// Row of a piece at column c in u8.8; valid for c at or beyond x0.
static uint16_t piece_y(const pt_render *r, uint8_t p, uint8_t c)
{
    const pt_piece *pp = &r->pool[p];
    RC_ASSERT(c >= pp->x0);
    return row_step((uint16_t)((pp->y0 << 8) | 128), pp->g, (uint8_t)(c - pp->x0));
}

static uint16_t line_y(const pt_line *l, uint8_t c)
{
    RC_ASSERT(c >= l->x0);
    return row_step((uint16_t)((l->y0 << 8) | 128), l->g, (uint8_t)(c - l->x0));
}

// Split piece p at column c: p keeps [x0, c), the new piece takes [c, x1).
static void pool_split(pt_render *r, uint8_t p, uint8_t c)
{
    pt_piece *pp = &r->pool[p];
    RC_ASSERT(c > pp->x0 && c < pp->x1);

    uint8_t y0 = row_of(piece_y(r, p, c));
    uint8_t q = pool_alloc(r, c, pp->x1, y0, pp->g, pp->ce, pp->co);
    r->pool[q].next = pp->next;
    pp->next = q;
    pp->x1 = c;
}

// The piece of a chain containing column c.
static uint8_t piece_at(const pt_render *r, uint8_t head, uint8_t c)
{
    uint8_t p = head;
    RC_ASSERT(p != 0);
    while (r->pool[p].x1 <= c) {
        p = r->pool[p].next;
        RC_ASSERT(p != 0);
    }
    RC_ASSERT(r->pool[p].x0 <= c);
    return p;
}

// ---- chain building ----

// Widen a chain's conservative row extents by piece q over columns [c0, c1),
// from the endpoint rows.
static void chain_extents(pt_render *r, pt_chainb *cb, uint8_t q, uint8_t c0, uint8_t c1)
{
    uint16_t ya = piece_y(r, q, c0);
    uint16_t yb = piece_y(r, q, (uint8_t)(c1 - 1));
    uint8_t hrow = row_of_ceil(ya > yb ? ya : yb);
    uint8_t lrow = row_of_floor(ya < yb ? ya : yb);
    if (hrow > cb->tmax)
        cb->tmax = hrow;
    if (lrow < cb->bmin)
        cb->bmin = lrow;
}

// Link q as the chain's new tail; its columns from c0 on count toward the
// extents (a piece extended from an earlier visit starts left of the span).
static void chain_append(pt_render *r, pt_chainb *cb, uint8_t q, uint8_t c0)
{
    pt_piece *p = &r->pool[q];

    p->next = 0;
    if (cb->tail) {
        RC_ASSERT(r->pool[cb->tail].x1 <= p->x0);
        r->pool[cb->tail].next = q;
    } else {
        cb->head = q;
    }
    cb->tail = q;
    chain_extents(r, cb, q, c0, p->x1);
}

static void closed_add(pt_closedl *cl, uint8_t l, uint8_t r)
{
    if (cl->num && cl->l[cl->num - 1] <= l && cl->r[cl->num - 1] >= l) {
        // touching or overlapping the previous range from the same line: merge
        if (r > cl->r[cl->num - 1])
            cl->r[cl->num - 1] = r;
        return;
    }
    RC_PANIC(cl->num < PT_MAX_CLOSED);
    cl->l[cl->num] = l;
    cl->r[cl->num] = r;
    cl->num++;
}

// ---- clipping a line against the bounds (spec 7.9) ----

// |ga - gb| for two sign-and-magnitude gradients: equal signs compare the
// magnitudes first, opposite signs add them (both below 0x8000, no carry).
static uint16_t grad_diff(uint16_t ga, uint16_t gb)
{
    uint16_t ma = pt_g_mag(ga);
    uint16_t mb = pt_g_mag(gb);
    if (pt_g_isneg(ga) == pt_g_isneg(gb))
        return ma >= mb ? (uint16_t)(ma - mb) : (uint16_t)(mb - ma);
    return (uint16_t)(ma + mb);
}

// The first column at or after which the sign of a linear difference has
// changed: floor(d0 / dg) + 1, clamped inside (c, e).
static uint8_t crossing(uint8_t c, uint8_t e, uint16_t d0, uint16_t dg)
{
    RC_ASSERT(dg != 0);

    uint8_t q = pt_div_cols(d0, dg);
    uint8_t span = (uint8_t)(e - 1 - c);
    uint8_t n = q >= span ? span : (uint8_t)(q + 1);
    if (n == 0)
        n = 1;
    RC_ASSERT(n <= span);
    return (uint8_t)(c + n);
}

// Emit one resolved range of a line. VISIBLE allocates a piece; ABOVE/BELOW
// merges the line's colour into the bound piece over exactly [r0, r1), leaving
// the cursor on the continuation of that chain.
static void emit(pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1, uint8_t st)
{
    RC_ASSERT(r0 < r1);
    pt_render *r = cx->r;

    if (st == ST_VISIBLE) {
        // The line's previous visible piece ending exactly here is the same
        // line continuing - the next block of this clip, or the seam with
        // the cell's previous visit - so it just grows, keeping its anchor.
        // Its gradient and colour must still match: splits and merges of
        // bound pieces (by the child walks between visits) change x1 or the
        // colour, and then a fresh piece starts. A piece carried over from
        // an earlier visit can only open the chain being built (it ends at
        // this visit's first column), never follow its tail.
        pt_chainb *cb = cx->build;
        uint8_t q = cx->vis->active[cx->slot];
        if (q && r->pool[q].x1 == r0 && r->pool[q].g == L->g && r->pool[q].ce == L->ce
            && r->pool[q].co == L->co && (!cb || cb->tail == q || cb->tail == 0)) {
            r->pool[q].x1 = r1;
            if (cb && cb->tail == q)
                chain_extents(r, cb, q, r0, r1);
            else if (cb)
                chain_append(r, cb, q, r0);
            return;
        }
        q = pool_alloc(r, r0, r1, row_of(line_y(L, r0)), L->g, L->ce, L->co);
        cx->vis->active[cx->slot] = q;
        if (cb)
            chain_append(r, cb, q, r0);
        return;
    }

    uint8_t *pp = (st == ST_ABOVE) ? &cx->tp : &cx->bp;
    uint8_t p = *pp;
    RC_ASSERT(p != 0);

    // isolate exactly [r0, r1) within the bound piece
    if (r->pool[p].x0 < r0) {
        pool_split(r, p, r0);
        p = r->pool[p].next;
    }
    RC_ASSERT(r->pool[p].x0 == r0);
    if (r->pool[p].x1 > r1)
        pool_split(r, p, r1);
    RC_ASSERT(r->pool[p].x1 == r1);

    // clamp = recolour; relinking may sever the chain link, so remember the
    // continuation for the walk first
    uint8_t cont = r->pool[p].next;
    r->pool[p].ce ^= L->ce;
    r->pool[p].co ^= L->co;

    bool closed = (cx->role == ROLE_BOTTOM && st == ST_ABOVE)
               || (cx->role == ROLE_TOP && st == ST_BELOW);
    if (closed) {
        // the piece stands in for the child's whole hidden stack
        r->pool[p].ce ^= cx->cbfb.e;
        r->pool[p].co ^= cx->cbfb.o;
        closed_add(cx->closed, r0, r1);
        *pp = p;    // stays in its chain; the walk steps past via x1
    } else if (cx->build) {
        chain_append(r, cx->build, p, r0);
        *pp = cont ? cont : p;
    } else {
        *pp = p;
    }
}

// Resolve one block [c, e) over which the line and both bound pieces are
// single linear functions, and emit its sub-ranges.
static void resolve_block(pt_clip *cx, const pt_line *L, uint8_t c, uint8_t e)
{
    pt_render *r = cx->r;
    uint8_t last = (uint8_t)(e - 1);
    uint16_t tg = r->pool[cx->tp].g;
    uint16_t bg = r->pool[cx->bp].g;

    uint16_t yl0 = line_y(L, c);
    uint16_t yl1 = line_y(L, last);
    uint16_t yt0 = piece_y(r, cx->tp, c);
    uint16_t yt1 = piece_y(r, cx->tp, last);

    // resolve against the top: the difference changes sign at most once
    struct range {
        uint8_t r0, r1, st;
    } rs[2];
    uint8_t nr = 0;
    if (yl0 <= yt0 && yl1 <= yt1) {
        rs[nr++] = (struct range) {c, e, ST_ABOVE};
    } else if (yl0 > yt0 && yl1 > yt1) {
        rs[nr++] = (struct range) {c, e, ST_OPEN};
    } else if (yl0 <= yt0) {
        uint8_t c1 = crossing(c, e, (uint16_t)(yt0 - yl0), grad_diff(L->g, tg));
        rs[nr++] = (struct range) {c, c1, ST_ABOVE};
        rs[nr++] = (struct range) {c1, e, ST_OPEN};
    } else {
        uint8_t c1 = crossing(c, e, (uint16_t)(yl0 - yt0), grad_diff(L->g, tg));
        rs[nr++] = (struct range) {c, c1, ST_OPEN};
        rs[nr++] = (struct range) {c1, e, ST_ABOVE};
    }

    // resolve each OPEN range against the bottom the same way
    for (uint8_t i = 0; i < nr; i++) {
        if (rs[i].st != ST_OPEN) {
            emit(cx, L, rs[i].r0, rs[i].r1, rs[i].st);
            continue;
        }
        uint8_t r0 = rs[i].r0;
        uint8_t r1 = rs[i].r1;
        // an open range sharing an end with the block reuses its line row
        uint16_t l0 = r0 == c ? yl0 : line_y(L, r0);
        uint16_t l1 = r1 == e ? yl1 : line_y(L, (uint8_t)(r1 - 1));
        uint16_t b0 = piece_y(r, cx->bp, r0);
        uint16_t b1 = piece_y(r, cx->bp, (uint8_t)(r1 - 1));
        if (l0 >= b0 && l1 >= b1) {
            emit(cx, L, r0, r1, ST_BELOW);
        } else if (l0 < b0 && l1 < b1) {
            emit(cx, L, r0, r1, ST_VISIBLE);
        } else if (l0 >= b0) {
            uint8_t c2 = crossing(r0, r1, (uint16_t)(l0 - b0), grad_diff(L->g, bg));
            emit(cx, L, r0, c2, ST_BELOW);
            emit(cx, L, c2, r1, ST_VISIBLE);
        } else {
            uint8_t c2 = crossing(r0, r1, (uint16_t)(b0 - l0), grad_diff(L->g, bg));
            emit(cx, L, r0, c2, ST_VISIBLE);
            emit(cx, L, c2, r1, ST_BELOW);
        }
    }
}

// Clamp a line between the top and bottom chains over its whole span.
static void clip_line(pt_clip *cx, const pt_line *L)
{
    pt_render *r = cx->r;
    RC_ASSERT(L->x0 < L->x1);

    // fast path: the line lies wholly inside the row extents of the chains
    uint16_t ya = (uint16_t)((L->y0 << 8) | 128);
    uint16_t yb = line_y(L, (uint8_t)(L->x1 - 1));
    uint16_t lo = ya < yb ? ya : yb;
    uint16_t hi = ya > yb ? ya : yb;
    if (row_of_floor(lo) >= cx->tmax && row_of_ceil(hi) <= cx->bmin) {
        emit(cx, L, L->x0, L->x1, ST_VISIBLE);
        return;
    }

    uint8_t c = L->x0;
    cx->tp = piece_at(r, cx->tp ? cx->tp : cx->thead, c);
    if (!cx->bp_set) {
        cx->bp = cx->bhead;
        cx->bp_set = true;
    }
    while (cx->bp && r->pool[cx->bp].x1 <= c)
        cx->bp = r->pool[cx->bp].next;

    while (c < L->x1) {
        uint8_t e;
        if (cx->bp == 0 || c < r->pool[cx->bp].x0) {
            // gap in the bottom chain (a closed range): the line is above the
            // top bound throughout
            e = L->x1;
            if (r->pool[cx->tp].x1 < e)
                e = r->pool[cx->tp].x1;
            if (cx->bp && r->pool[cx->bp].x0 < e)
                e = r->pool[cx->bp].x0;
            emit(cx, L, c, e, ST_ABOVE);
        } else {
            e = L->x1;
            if (r->pool[cx->tp].x1 < e)
                e = r->pool[cx->tp].x1;
            if (r->pool[cx->bp].x1 < e)
                e = r->pool[cx->bp].x1;
            resolve_block(cx, L, c, e);
        }
        c = e;
        if (c >= L->x1)
            break;
        while (r->pool[cx->tp].x1 <= c) {
            cx->tp = r->pool[cx->tp].next;
            RC_ASSERT(cx->tp != 0);
        }
        while (cx->bp && r->pool[cx->bp].x1 <= c)
            cx->bp = r->pool[cx->bp].next;
    }
}

// ---- line generation (spec 7.7) ----

// A relative height: |z - eye_z|, all in the map's 8-unit z steps, the side
// kept apart. One byte compare-and-subtract; the projections compensate for
// the quantisation with a wider shift.
typedef struct pt_relh {
    uint8_t m;        // |z - eye_z|
    uint8_t below;    // z below the eye (z increases downwards)
} pt_relh;

static pt_relh rel_h(uint8_t z, uint8_t eye_z)
{
    if (z >= eye_z)
        return (pt_relh) {.m = (uint8_t)(z - eye_z), .below = 1};
    return (pt_relh) {.m = (uint8_t)(eye_z - z), .below = 0};
}

// The screen row of a height at an endpoint's depth: the centre plus or
// minus the projected offset, the side picking the direction.
static uint8_t row_from(pt_relh h, const pt_ep *e)
{
    uint8_t q = pt_project_rowd(h.m, e->x, e->rx);
    return (uint8_t)(h.below ? PT_SCREEN_HEIGHT / 2 + q : PT_SCREEN_HEIGHT / 2 - q);
}

static pt_line flat(uint8_t x0, uint8_t x1, uint8_t y, pt_pattern col)
{
    return (pt_line) {.x0 = x0, .x1 = x1, .y0 = y, .g = 0, .ce = col.e, .co = col.o};
}

// Build one or two pieces for the edge's line at relative height h, near
// clipping against x = 2h; pieces partition [e0.sx, e1.sx) left to right.
static uint8_t make_line(pt_line out[2], pt_ep e0, pt_ep e1, pt_relh h, pt_pattern col)
{
    RC_ASSERT(e0.x >= 1 && e1.x >= 1);
    RC_ASSERT(e0.sx < e1.sx);

    // The clip plane sits exactly where the row projection reaches the
    // screen edge: x = 128h/H, the 64-row screen's classic x = 2h = 16*h.m
    // (for the 80-row screen it is 8h/5, shifts and adds on the 6502).
    uint16_t d = (uint16_t)(((uint32_t)h.m * (8 * 128) + PT_SCREEN_HEIGHT - 1)
                            / PT_SCREEN_HEIGHT);
    uint8_t edge = h.below ? PT_SCREEN_HEIGHT : 0;
    bool near0 = e0.x < d;
    bool near1 = e1.x < d;

    if (near0 && near1) {
        out[0] = flat(e0.sx, e1.sx, edge, col);
        return 1;
    }
    if (!near0 && !near1) {
        uint8_t sy0 = row_from(h, &e0);
        uint8_t sy1 = row_from(h, &e1);
        uint8_t dx = (uint8_t)(e1.sx - e0.sx);
        uint16_t g = sy1 >= sy0 ? pt_g_make(pt_grad_mag((uint8_t)(sy1 - sy0), dx), 0)
                                : pt_g_make(pt_grad_mag((uint8_t)(sy0 - sy1), dx), 1);
        out[0] = (pt_line) {.x0 = e0.sx, .x1 = e1.sx, .y0 = sy0, .g = g, .ce = col.e, .co = col.o};
        return 1;
    }

    // exactly one endpoint too close: clip the edge at depth d
    const pt_ep *ne = near0 ? &e0 : &e1;
    const pt_ep *fe = near0 ? &e1 : &e0;
    uint8_t t = pt_div_t((uint16_t)(d - ne->x), (uint16_t)(fe->x - ne->x));
    int16_t ys = pt_lerp16(ne->y, fe->y, t);
    uint16_t sxq = pt_project_col_q8(ys, d);
    uint8_t syf = row_from(h, fe);

    uint8_t cs = (uint8_t)((uint16_t)(sxq + 255) >> 8);
    if (cs < e0.sx)
        cs = e0.sx;
    if (cs > e1.sx)
        cs = e1.sx;

    uint8_t n = 0;
    if (near0) {
        if (e0.sx < cs)
            out[n++] = flat(e0.sx, cs, edge, col);
        if (cs < e1.sx) {
            uint16_t dxq = (uint16_t)(((uint16_t)(e1.sx << 8)) - sxq);
            uint8_t fr = (uint8_t)(((uint16_t)(cs << 8)) - sxq);
            uint16_t base = (uint16_t)((edge << 8) | 128);    // rounding bias in the anchor
            uint16_t g;
            uint16_t y0q;
            if (syf >= edge) {
                uint16_t gm = pt_gradq8_mag((uint8_t)(syf - edge), dxq);
                uint16_t off = pt_mulu16_frac(gm, fr);
                g = pt_g_make(gm, 0);
                y0q = (uint16_t)(base + off);
                if (y0q < base)
                    y0q = 0xFFFF;    // carry out: clamp
            } else {
                uint16_t gm = pt_gradq8_mag((uint8_t)(edge - syf), dxq);
                uint16_t off = pt_mulu16_frac(gm, fr);
                g = pt_g_make(gm, 1);
                y0q = off > base ? 0 : (uint16_t)(base - off);
            }
            // round like every other re-anchor; PT_SCREEN_HEIGHT is fine,
            // the piece comes on screen where its gradient brings the row
            // under the screen edge less a half
            uint8_t y0 = row_of(y0q);
            out[n++] = (pt_line) {.x0 = cs, .x1 = e1.sx, .y0 = y0, .g = g, .ce = col.e, .co = col.o};
        }
    } else {
        if (e0.sx < cs) {
            uint16_t dxq = (uint16_t)(sxq - ((uint16_t)(e0.sx << 8)));
            uint16_t g = edge >= syf ? pt_g_make(pt_gradq8_mag((uint8_t)(edge - syf), dxq), 0)
                                     : pt_g_make(pt_gradq8_mag((uint8_t)(syf - edge), dxq), 1);
            out[n++] = (pt_line) {.x0 = e0.sx, .x1 = cs, .y0 = syf, .g = g, .ce = col.e, .co = col.o};
        }
        if (cs < e1.sx)
            out[n++] = flat(cs, e1.sx, edge, col);
    }
    return n;
}

// Restrict a line to [xa, xb); false when nothing is left.
static bool clamp_line(pt_line *L, uint8_t xa, uint8_t xb)
{
    if (L->x0 >= L->x1)
        return false;
    if (L->x1 > xb)
        L->x1 = xb;
    if (L->x0 < xa) {
        if (xa >= L->x1)
            return false;
        L->y0 = row_of(line_y(L, xa));
        L->x0 = xa;
    }
    return L->x0 < L->x1;
}

// ---- vertex transform and classification (spec 3.2, 7.3) ----

// The classification is rounded, and the walk is built to survive that. The
// camera may legitimately stand ON its cell's boundary (the transition's
// exact tie rule can park it on a portal line, even on a fan vertex), where
// vertex sums near zero can round to either side. Nothing here needs to be
// exact, because every consumer is robust by structure: child frames start
// from the portal (or resume a finished visit's edge), spans are clamped to
// the cursors' frontier in process_edge (a vertex projected left of its
// predecessor yields an empty span, never a cut regression), a ring that
// never fires its stop burns its steps and closes the frame (its visit then
// forgets its cursor), and a nested visit of a cell already walking is
// skipped. The one load-bearing consumer of these signs is
// push_root's winding search on the CAMERA's cell - and that, once per
// frame at most, retries with exact sums when the rounded ones fail. At an
// exact boundary tie a frame may therefore carry a one-column sliver where
// perfection would need exact spans; it never traps.
static pt_vv transform(const pt_render *r, pt_vertex w)
{
    // camera-relative position and the rotation: two's complement sums of
    // sign-branched unsigned products
    int16_t rx = (int16_t)(w.x - r->cam_x);
    int16_t ry = (int16_t)(w.y - r->cam_y);
    pt_xy rv = pt_rot(rx, ry, r->angle);
    int16_t x = rv.x;
    int16_t y = rv.y;

    // the ray sums exist only here: their sign and zero states become flag
    // bits (on the 6502, the adds' own N and Z), and only the flags persist
    int32_t a = (int32_t)x + y;
    int32_t b = (int32_t)x - y;
    RC_ASSERT(a >= INT16_MIN && a <= INT16_MAX && b >= INT16_MIN && b <= INT16_MAX);
    uint8_t flags = 0;
    if (a < 0)
        flags |= PT_VF_ANEG;
    else if (a == 0)
        flags |= PT_VF_AZERO;
    if (b < 0)
        flags |= PT_VF_BNEG;
    else if (b == 0)
        flags |= PT_VF_BZERO;

    // the ray sums are also zero on the rays' backward extensions, where the
    // zero flags would wrongly read as "exactly on the (forward) ray" - no
    // right clip, no stop, and the walk would wrap. A vertex on a seam of
    // the behind quadrant counts as behind (rare: the branches sit behind
    // the zero tests, and on the 6502 behind the adds' own Z)
    if ((flags & PT_VF_ANEG) && (flags & PT_VF_BZERO))
        flags |= PT_VF_BNEG;
    if ((flags & PT_VF_BNEG) && (flags & PT_VF_AZERO))
        flags |= PT_VF_ANEG;

    // a vertex exactly on a ray gets the exact column, so the stop tests and
    // the span columns can never disagree at the portal edges. The depth's
    // reciprocal is normalised once here and reused by every row projection
    // at this vertex (the eye vertex, x = 0, takes depth 1 like depth_of)
    pt_vv v = {.x = x, .y = y, .sx = 64, .flags = flags};
    if (!(flags & (PT_VF_ANEG | PT_VF_BNEG))) {
        v.rx = pt_recip16(x > 0 ? (uint16_t)x : 1);
        if (flags & PT_VF_AZERO)
            v.sx = 0;
        else if (flags & PT_VF_BZERO)
            v.sx = 128;
        else if (x > 0)
            v.sx = pt_project_col(y, (uint16_t)x, v.rx);
    }
    return v;
}

// Is the vertex's column strictly right of column c? Uses the same projection
// as the span columns, so the walk and the spans agree.
static bool col_gt(pt_vv v, uint8_t c)
{
    if (!(v.flags & (PT_VF_ANEG | PT_VF_BNEG)))
        return v.sx > c;
    if (!(v.flags & PT_VF_BNEG))
        return false;    // outside left
    if (!(v.flags & PT_VF_ANEG))
        return true;     // outside right

    // behind: mirror through the camera, project, compare the other way round
    return pt_project_col((int16_t)-v.y, (uint16_t)-v.x, pt_recip16((uint16_t)-v.x)) < c;
}

// Is the vertex's column at or right of column c?
static bool col_ge(pt_vv v, uint8_t c)
{
    if (!(v.flags & (PT_VF_ANEG | PT_VF_BNEG)))
        return v.sx >= c;
    if (!(v.flags & PT_VF_BNEG))
        return false;
    if (!(v.flags & PT_VF_ANEG))
        return true;
    return pt_project_col((int16_t)-v.y, (uint16_t)-v.x, pt_recip16((uint16_t)-v.x)) <= c;
}

// ---- frustum side clips (spec 7.6) ----

// Clamp a lerped depth to at least 1 so the projections stay well defined.
static uint16_t depth_of(int16_t x)
{
    return x < 1 ? 1 : (uint16_t)x;
}

// An endpoint left where the vertex is. An in-frustum vertex brings its
// reciprocal; one outside a ray (an edge's far end that no clip claimed,
// rare) normalises its own.
static pt_ep ep_unclipped(pt_vv v, uint8_t sx)
{
    uint16_t x = depth_of(v.x);
    pt_recip rx = (v.flags & (PT_VF_ANEG | PT_VF_BNEG)) ? pt_recip16(x) : v.rx;
    return (pt_ep) {.x = x, .y = v.y, .sx = sx, .rx = rx};
}

static pt_ep clip_left(pt_vv v0, pt_vv v1)
{
    RC_ASSERT(v0.flags & PT_VF_ANEG);
    RC_ASSERT(!(v1.flags & (PT_VF_ANEG | PT_VF_AZERO)));

    // the ray magnitudes, recomputed with the signs the flags fix: -(x + y)
    // is one subtract chain from zero, nothing is negated
    uint16_t na = (uint16_t)(0u - (uint16_t)v0.x - (uint16_t)v0.y);
    uint16_t den = (uint16_t)(na + (uint16_t)(v1.x + v1.y));
    RC_ASSERT(den >= na);
    uint8_t t = pt_div_t(na, den);
    uint16_t x = depth_of(pt_lerp16(v0.x, v1.x, t));
    return (pt_ep) {.x = x, .y = (int16_t)-x, .sx = 0, .rx = pt_recip16(x)};
}

static pt_ep clip_right(uint16_t x0, uint16_t b0, pt_vv v1)
{
    RC_ASSERT(v1.flags & PT_VF_BNEG);

    // -(x - y) is the reversed subtraction y - x: no negate at all
    uint16_t den = (uint16_t)(b0 + (uint16_t)((uint16_t)v1.y - (uint16_t)v1.x));
    RC_ASSERT(den >= b0 && den > 0);
    uint8_t t = pt_div_t(b0, den);
    RC_ASSERT(x0 <= 0x7FFF);
    uint16_t x = depth_of(pt_lerp16((int16_t)x0, v1.x, t));
    return (pt_ep) {.x = x, .y = (int16_t)x, .sx = 128, .rx = pt_recip16(x)};
}

// ---- chains: taking ownership of a span (spec 7.9.2) ----

static uint8_t cut(pt_render *r, uint8_t *cursor, uint8_t xa, uint8_t xb)
{
    uint8_t cur = *cursor;
    RC_ASSERT(cur != 0);

    while (r->pool[cur].x1 <= xa) {
        cur = r->pool[cur].next;
        RC_ASSERT(cur != 0);
    }
    if (r->pool[cur].x0 < xa) {
        pool_split(r, cur, xa);
        cur = r->pool[cur].next;
    }
    uint8_t head = cur;
    uint8_t p = cur;
    while (r->pool[p].x1 < xb) {
        p = r->pool[p].next;
        RC_ASSERT(p != 0);
    }
    if (r->pool[p].x1 > xb)
        pool_split(r, p, xb);
    *cursor = r->pool[p].next;
    return head;
}

// ---- pushing portals ----

// The ring successor of ring index k.
static uint8_t ring_k_next(const pt_cell *c, uint8_t k)
{
    return (uint8_t)(k + 1 == c->num_edges ? 0 : k + 1);
}

// The start vertex of the cell's edge at ring index k.
static pt_vertex ring_vertex(const pt_map *m, const pt_cell *c, uint8_t k)
{
    return m->verts[m->edges[c->first_edge + k].v0];
}

// The cell's visit record, created on its first visit of the frame.
static uint8_t visit_of(pt_render *r, uint8_t cell)
{
    uint8_t i = r->cell_visit[cell];
    if (i)
        return i;
    RC_PANIC(r->num_visits < PT_MAX_VISITS);
    i = r->num_visits++;
    r->visits[i] = (pt_visit) {.cell = cell, .last_xr = 0xFF, .act_k = 0xFF};
    r->cell_visit[cell] = i;
    return i;
}

// Push a pending child frame through portal edge ei; it finds its first
// edge when it starts. pv is P, the portal's start vertex, where a search
// from the portal begins: its edge is the twin's ring successor.
static void push_child(pt_render *r, uint8_t ei, uint8_t xl, uint8_t xr, uint8_t thead,
                       uint8_t bhead, uint8_t tmax, uint8_t bmin, pt_vv pv)
{
    RC_PANIC(r->depth < PT_MAX_DEPTH);
    const pt_map *m = r->map;
    uint8_t cell = m->edges[ei].adj;
    const pt_cell *c = &m->cells[cell];

    r->stack[r->depth++] = (pt_pframe) {
        .visit = visit_of(r, cell),
        .xl = xl,
        .xr = xr,
        .tcur = thead,
        .bcur = bhead,
        .tmax = tmax,
        .bmin = bmin,
        .k0 = ring_k_next(c, (uint8_t)(pt_map_twin(m, ei) - c->first_edge)),
        .steps = (uint8_t)(c->num_edges + 1),
        .pv = pv,
    };
}

// Find a starting frame's first edge (spec 7.4): the first whose end vertex
// is strictly right of xl. A cell visited before resumes from the edge its
// last visit ended on - every earlier edge ends left of that visit's xr, so
// none can be first - with that edge's vertices already in hand; otherwise
// the search begins at P. False when the frame must not walk: a degenerate
// ring, or a nested visit of a cell already walking (reachable only through
// boundary rounding; that child is skipped).
static bool start_frame(pt_render *r, pt_pframe *f)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[f->visit];
    const pt_cell *c = &m->cells[vs->cell];

    if (vs->live)
        return false;
    if (vs->last_xr > f->xl) {
        vs->k = f->k0;
        vs->v0 = f->pv;
        vs->v1 = transform(r, ring_vertex(m, c, ring_k_next(c, f->k0)));
    }
    uint8_t guard = c->num_edges;
    while (!col_gt(vs->v1, f->xl)) {
        if (guard-- == 0) {
            RC_ASSERT(0);
            return false;    // degenerate ring; skip the child rather than spin
        }
        vs->k = ring_k_next(c, vs->k);
        vs->v0 = vs->v1;
        vs->v1 = transform(r, ring_vertex(m, c, ring_k_next(c, vs->k)));
    }
    vs->live = 1;
    return true;
}

// ---- one edge of the walk (spec 7.8) ----

static void process_edge(pt_render *r, pt_pframe *f)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[f->visit];
    const pt_cell *ca = &m->cells[vs->cell];
    uint8_t ei = (uint8_t)(ca->first_edge + vs->k);
    const pt_edge *e = &m->edges[ei];
    pt_vv v0 = vs->v0;
    pt_vv v1 = vs->v1;

    // the active pieces belong to one edge: a new edge starts its lines
    // afresh, while a visit resuming on the previous visit's last edge
    // keeps them, so its lines can extend across the seam
    if (vs->act_k != vs->k) {
        vs->act_k = vs->k;
        for (uint8_t i = 0; i < PT_NUM_SLOTS; i++)
            vs->active[i] = 0;
    }

    // wholly outside one frustum ray: no columns, nothing to do
    if ((v0.flags & PT_VF_ANEG) && (v1.flags & (PT_VF_ANEG | PT_VF_AZERO)))
        return;
    if ((v1.flags & PT_VF_BNEG) && !(v0.flags & PT_VF_ANEG)
        && (v0.flags & (PT_VF_BNEG | PT_VF_BZERO)))
        return;

    // 1. side clip and column span
    pt_ep e0;
    uint16_t b0;
    if (v0.flags & PT_VF_ANEG) {
        e0 = clip_left(v0, v1);
        b0 = (uint16_t)(e0.x << 1);    // on the left ray b = 2x
    } else if (v0.flags & PT_VF_BNEG) {
        e0 = ep_unclipped(v0, 128);
        b0 = 0;
    } else {
        e0 = ep_unclipped(v0, v0.sx);
        b0 = (uint16_t)(int16_t)(v0.x - v0.y);
    }
    pt_ep e1;
    if (v1.flags & PT_VF_BNEG)
        e1 = clip_right(e0.x, b0, v1);
    else
        e1 = ep_unclipped(v1, (v1.flags & PT_VF_ANEG) ? 0 : v1.sx);

    uint8_t xa = e0.sx > f->xl ? e0.sx : f->xl;
    uint8_t xb = e1.sx < f->xr ? e1.sx : f->xr;

    // ownership only ever moves right: clamp the span to the cursors'
    // frontier. A healthy ring is contiguous (a shared vertex projects to
    // one column in both roles), so this never bites; it is the backstop
    // that turns a degenerate ring's wrap - or rounding projecting a vertex
    // left of its predecessor - into empty spans instead of a cut
    // regression. An exhausted cursor (0) means the whole span is owned.
    if (f->tcur == 0 || f->bcur == 0)
        return;
    uint8_t front = r->pool[f->tcur].x0 > r->pool[f->bcur].x0 ? r->pool[f->tcur].x0
                                                              : r->pool[f->bcur].x0;
    if (xa < front)
        xa = front;
    if (xa >= xb)
        return;

    // 2. take ownership of the bound sub-chains over [xa, xb)
    uint8_t th = cut(r, &f->tcur, xa, xb);
    uint8_t bh = cut(r, &f->bcur, xa, xb);

    if (e->colour == PT_EDGE_SOFT) {
        push_child(r, ei, xa, xb, th, bh, f->tmax, f->bmin, v0);
        return;
    }

    pt_pattern wall = pt_shade((uint8_t)(e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_relh ha_c = rel_h(ca->ceil_z, r->eye_z);
    pt_relh ha_f = rel_h(ca->floor_z, r->eye_z);
    pt_line ls[2];
    uint8_t n;

    if (e->adj == 0) {
        // solid wall: ceiling line and floor line, recolour only; each line
        // gets fresh cursors (role NONE never severs the chains)
        pt_clip cx = {.r = r, .thead = th, .bhead = bh, .tmax = f->tmax, .bmin = f->bmin,
                      .role = ROLE_NONE, .vis = vs, .slot = PT_SLOT_CEIL};
        n = make_line(ls, e0, e1, ha_c, pt_pattern_xor(ca_ceil, wall));
        for (uint8_t i = 0; i < n; i++)
            if (clamp_line(&ls[i], xa, xb))
                clip_line(&cx, &ls[i]);
        cx.tp = 0;
        cx.bp_set = false;
        cx.slot = PT_SLOT_FLOOR;
        n = make_line(ls, e0, e1, ha_f, pt_pattern_xor(wall, ca_floor));
        for (uint8_t i = 0; i < n; i++)
            if (clamp_line(&ls[i], xa, xb))
                clip_line(&cx, &ls[i]);
        return;
    }

    const pt_cell *cb = &m->cells[e->adj];
    pt_relh hb_c = rel_h(cb->ceil_z, r->eye_z);
    pt_relh hb_f = rel_h(cb->floor_z, r->eye_z);
    pt_pattern cb_ceil = pt_shade(cb->ceil);
    pt_pattern cb_floor = pt_shade(cb->floor);
    pt_pattern cbfb = pt_pattern_xor(cb_ceil, cb_floor);

    // 3. non-bound-forming lines first: they only recolour, and need the top
    // and bottom chains intact
    pt_clip cxn = {.r = r, .thead = th, .bhead = bh, .tmax = f->tmax, .bmin = f->bmin,
                   .role = ROLE_NONE, .vis = vs, .slot = PT_SLOT_CEIL};
    if (cb->ceil_z > ca->ceil_z) {
        n = make_line(ls, e0, e1, ha_c, pt_pattern_xor(ca_ceil, wall));
        for (uint8_t i = 0; i < n; i++)
            if (clamp_line(&ls[i], xa, xb))
                clip_line(&cxn, &ls[i]);
        cxn.tp = 0;
        cxn.bp_set = false;
    }
    if (cb->floor_z < ca->floor_z) {
        cxn.slot = PT_SLOT_FLOOR;
        n = make_line(ls, e0, e1, ha_f, pt_pattern_xor(wall, ca_floor));
        for (uint8_t i = 0; i < n; i++)
            if (clamp_line(&ls[i], xa, xb))
                clip_line(&cxn, &ls[i]);
    }

    // 4. bound-forming lines, floor first: it builds the child's bottom chain
    // against T and B; the ceiling line then builds the child's top chain
    // against T and the chain the floor line just built
    pt_chainb childb = {.bmin = PT_SCREEN_HEIGHT};
    pt_chainb childt = {.bmin = PT_SCREEN_HEIGHT};
    pt_closedl closed = {0};

    pt_line fl[2];
    uint8_t nf = (cb->floor_z < ca->floor_z)
                     ? make_line(fl, e0, e1, hb_f, pt_pattern_xor(cb_floor, wall))
                     : make_line(fl, e0, e1, ha_f, pt_pattern_xor(cb_floor, ca_floor));
    pt_line cl[2];
    uint8_t nc = (cb->ceil_z > ca->ceil_z)
                     ? make_line(cl, e0, e1, hb_c, pt_pattern_xor(wall, cb_ceil))
                     : make_line(cl, e0, e1, ha_c, pt_pattern_xor(ca_ceil, cb_ceil));

    pt_clip cxf = {.r = r, .thead = th, .bhead = bh, .tmax = f->tmax, .bmin = f->bmin,
                   .role = ROLE_BOTTOM, .build = &childb, .cbfb = cbfb, .closed = &closed,
                   .vis = vs, .slot = PT_SLOT_FLOOR_BOUND};
    for (uint8_t i = 0; i < nf; i++)
        if (clamp_line(&fl[i], xa, xb))
            clip_line(&cxf, &fl[i]);

    pt_clip cxc = {.r = r, .thead = th, .bhead = childb.head, .tmax = f->tmax,
                   .bmin = childb.head ? childb.bmin : 0, .role = ROLE_TOP, .build = &childt,
                   .cbfb = cbfb, .closed = &closed, .vis = vs, .slot = PT_SLOT_CEIL_BOUND};
    for (uint8_t i = 0; i < nc; i++)
        if (clamp_line(&cl[i], xa, xb))
            clip_line(&cxc, &cl[i]);

    // 5. one child portal per open sub-range of [xa, xb), pushed right to
    // left so the leftmost runs first
    uint8_t ol[PT_MAX_CLOSED + 1];
    uint8_t orr[PT_MAX_CLOSED + 1];
    uint8_t no = 0;

    // insertion sort the closed ranges by left edge (floor and ceiling ranges
    // may interleave)
    for (uint8_t i = 1; i < closed.num; i++) {
        uint8_t l = closed.l[i];
        uint8_t rr = closed.r[i];
        uint8_t j = i;
        while (j > 0 && closed.l[j - 1] > l) {
            closed.l[j] = closed.l[j - 1];
            closed.r[j] = closed.r[j - 1];
            j--;
        }
        closed.l[j] = l;
        closed.r[j] = rr;
    }
    uint8_t cur = xa;
    for (uint8_t i = 0; i < closed.num; i++) {
        if (closed.l[i] > cur) {
            ol[no] = cur;
            orr[no] = closed.l[i];
            no++;
        }
        if (closed.r[i] > cur)
            cur = closed.r[i];
    }
    if (cur < xb) {
        ol[no] = cur;
        orr[no] = xb;
        no++;
    }
    for (uint8_t i = no; i-- > 0;) {
        uint8_t cth = piece_at(r, childt.head, ol[i]);
        uint8_t cbh = piece_at(r, childb.head, ol[i]);
        push_child(r, ei, ol[i], orr[i], cth, cbh, childt.tmax, childb.bmin, v0);
    }
}

// ---- the frame (spec 7.1, 7.4, 7.5) ----

// The exact retry behind push_root: the rounded winding search can fail
// when the eye rests on (or within pt_rot's rounding of) its cell's
// boundary with an aligned facing - every sum's true magnitude is then
// smaller than the rounding error, and the ring can read one-sided. The
// exact ray sums are linear in the world offsets (pt_ray_basis), so
// convexity guarantees them a transition for any eye in its closed cell:
// find the edge whose start is strictly left of the left ray and whose end
// is not. The one case with no strictly-negative vertex is the eye standing
// ON a ring vertex with the whole cell to its right; that vertex's sums are
// exactly zero and it is the root - column 0 begins at it, and the edge it
// opens (a through-the-eye portal, or a wall seen end-on) spans rightward.
// Cold code, entered at most once per frame, and only at boundary poses.
static uint8_t root_exact(const pt_render *r, const pt_cell *c)
{
    const pt_map *m = r->map;
    int16_t k1, k2;
    pt_ray_basis(r->angle, &k1, &k2);

    int32_t prev = 0;
    uint8_t zero = 0xFF;
    uint8_t root = 0xFF;
    for (uint8_t k = 0; k <= c->num_edges; k++) {
        uint8_t n = (uint8_t)(c->first_edge + (k == c->num_edges ? 0 : k));
        int16_t rx = (int16_t)(m->verts[m->edges[n].v0].x - r->cam_x);
        int16_t ry = (int16_t)(m->verts[m->edges[n].v0].y - r->cam_y);
        int32_t a = (int32_t)rx * k1 + (int32_t)ry * k2;
        if (a == 0)
            zero = n;
        if (k > 0 && prev < 0 && a >= 0)
            root = (uint8_t)(c->first_edge + k - 1);
        prev = a;
    }
    if (root != 0xFF)
        return root;
    RC_ASSERT(zero != 0xFF);    // neither a transition nor the eye-vertex:
    return zero;                // broken map (falls back to the ring start)
}

// Find the camera cell's first edge (where a goes from <= 0 to > 0) and push
// the root portal frame. The searches are guarded: with the eye on the
// cell's boundary the rounded sums can read one-sided, and the exact retry
// decides instead (see root_exact). A ring the retry roots at a zero vertex
// has no closing column, so the walk ends by its steps - accepted, not an
// error.
static void push_root(pt_render *r, uint8_t roott, uint8_t rootb)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[r->cam_cell];

    uint8_t n = c->first_edge;
    pt_vv v = transform(r, m->verts[m->edges[n].v0]);
    pt_vv w;    // the root edge's end vertex, handed to the frame
    uint8_t guard = c->num_edges;
    while (!(v.flags & (PT_VF_ANEG | PT_VF_AZERO))) {    // a > 0
        if (guard-- == 0)
            goto exact;
        n = pt_map_ring_next(c, n);
        v = transform(r, m->verts[m->edges[n].v0]);
    }
    guard = c->num_edges;
    for (;;) {
        uint8_t nn = pt_map_ring_next(c, n);
        w = transform(r, m->verts[m->edges[nn].v0]);
        if (!(w.flags & (PT_VF_ANEG | PT_VF_AZERO)))    // a > 0
            goto rooted;
        n = nn;
        v = w;
        if (guard-- == 0)
            goto exact;
    }
exact:
    n = root_exact(r, c);
    v = transform(r, m->verts[m->edges[n].v0]);
    w = transform(r, m->verts[m->edges[pt_map_ring_next(c, n)].v0]);
rooted:;

    // the camera's cell is never revisited (every ray starts in it), so its
    // visit is simply begun here, already started on the root edge
    uint8_t vi = visit_of(r, r->cam_cell);
    pt_visit *vs = &r->visits[vi];
    vs->k = (uint8_t)(n - c->first_edge);
    vs->v0 = v;
    vs->v1 = w;
    vs->live = 1;
    r->stack[r->depth++] = (pt_pframe) {
        .visit = vi,
        .xl = 0,
        .xr = 128,
        .tcur = roott,
        .bcur = rootb,
        .tmax = 0,
        .bmin = PT_SCREEN_HEIGHT,
        .steps = (uint8_t)(c->num_edges + 1),
        .started = 1,
    };
}

// Plot every pool piece with a colour, one pixel pair per column: an
// ascending and a descending loop, both pure unsigned DDA. Rows round to
// nearest, the same convention every re-anchor uses: a piece split at a cut
// then plots the same rows the unsplit line would have, so chain seams
// cannot notch the boundary.
static void plot(pt_render *r)
{
    for (uint16_t i = 1; i < r->pool_num; i++) {
        const pt_piece *p = &r->pool[i];
        if ((uint8_t)(p->ce | p->co) == 0)
            continue;
        pt_pattern pat = {.e = p->ce, .o = p->co};
        // rounding bias folded in once: the row is then just the high byte
        uint16_t y = (uint16_t)((p->y0 << 8) | 128);
        uint16_t gm = pt_g_mag(p->g);
        if (!pt_g_isneg(p->g)) {
            for (uint8_t c = p->x0; c < p->x1; c++) {
                uint8_t row = (uint8_t)(y >> 8);
                if (row >= PT_SCREEN_HEIGHT)
                    break;    // rows only grow; the rest is below the screen
                pt_mode5_plot(r->fb, c, row, pat);
                y = (uint16_t)(y + gm);    // row < 64 here, so no wrap
            }
        } else {
            for (uint8_t c = p->x0; c < p->x1; c++) {
                uint8_t row = (uint8_t)(y >> 8);
                if (row < PT_SCREEN_HEIGHT)
                    pt_mode5_plot(r->fb, c, row, pat);
                // clamp at the screen top: the toggle stays live at row 0
                y = gm > y ? 0 : (uint16_t)(y - gm);
            }
        }
    }
}

void pt_render_init(pt_render *r, const pt_map *m)
{
    *r = (pt_render) {0};
    r->map = m;
}

void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell)
{
    const pt_map *m = r->map;
    RC_ASSERT(cell >= 1 && cell < m->num_cells);

    r->cam_x = cam_x;
    r->cam_y = cam_y;
    r->cam_cell = cell;
    RC_ASSERT(m->cells[cell].floor_z >= PT_CAM_HEIGHT);
    r->eye_z = (uint8_t)(m->cells[cell].floor_z - PT_CAM_HEIGHT);
    r->angle = angle;

    pt_mode5_clear(r->fb);
    r->pool_num = 1;
    r->pool[0] = (pt_piece) {0};
    r->depth = 0;
    r->num_visits = 1;

    // the root portal: a plotted top bound seeding the fill with the camera
    // cell's ceiling, and a dummy off-screen bottom bound
    pt_pattern ceil = pt_shade(m->cells[cell].ceil);
    uint8_t roott = pool_alloc(r, 0, 128, 0, 0, ceil.e, ceil.o);
    uint8_t rootb = pool_alloc(r, 0, 128, PT_SCREEN_HEIGHT, 0, 0, 0);
    push_root(r, roott, rootb);

    while (r->depth) {
        pt_pframe *f = &r->stack[r->depth - 1];
        pt_visit *vs = &r->visits[f->visit];
        if (f->done) {
            if (f->started)
                vs->live = 0;
            r->depth--;
            continue;
        }
        if (!f->started) {
            if (!start_frame(r, f)) {
                f->done = 1;
                continue;
            }
            f->started = 1;
        }
        // a healthy ring stops on col_ge before its steps run out; a ring
        // rooted at a zero vertex (the eye on it) has no closing column, no
        // stop fires, and the walk wraps through empty clamped spans until
        // the steps end the frame here - an accepted outcome, not an error
        if (f->steps == 0) {
            f->done = 1;
            vs->last_xr = 0xFF;    // the cursor is anywhere: later visits search
            continue;
        }
        f->steps--;

        // the visit holds both vertices of its current edge: each vertex is
        // transformed once, as the end of one edge, and carried on as the
        // start of the next
        bool stop = col_ge(vs->v1, f->xr);
        process_edge(r, f);    // may push children, which run first
        if (stop) {
            f->done = 1;
            vs->last_xr = f->xr;    // the cursor stays on this edge for a resume
        } else {
            const pt_cell *fc = &m->cells[vs->cell];
            vs->k = ring_k_next(fc, vs->k);
            vs->v0 = vs->v1;
            vs->v1 = transform(r, ring_vertex(m, fc, ring_k_next(fc, vs->k)));
        }
    }

    plot(r);
    pt_mode5_fill(r->fb);

    if (r->pool_num > r->pool_high)
        r->pool_high = r->pool_num;
    if (r->num_visits > r->visits_high)
        r->visits_high = r->num_visits;

    // empty the visit cache: each record clears its own cell's index
    for (uint8_t i = 1; i < r->num_visits; i++)
        r->cell_visit[r->visits[i].cell] = 0;
}
