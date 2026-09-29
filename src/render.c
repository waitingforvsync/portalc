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

// The biased u8.8 value of a whole row: the row in the high byte, the
// rounding bias in the low.
static uint16_t row_anchor(uint8_t row)
{
    return (uint16_t)((row << 8) | 128);
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
    return row_step(row_anchor(pp->y0), pp->g, (uint8_t)(c - pp->x0));
}

static uint16_t line_y(const pt_line *l, uint8_t c)
{
    RC_ASSERT(c >= l->x0);
    return row_step(row_anchor(l->y0), l->g, (uint8_t)(c - l->x0));
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

// Clamp a line between the top and bottom chains over its whole span. With
// st ST_ABOVE or ST_BELOW the span is known to lie outside the portal on
// that side (columns the screen pre-clip cut off, clip_edge_line): nothing
// is evaluated, the bound pieces over it are just recoloured.
static void clip_line(pt_clip *cx, const pt_line *L, uint8_t st)
{
    pt_render *r = cx->r;
    RC_ASSERT(L->x0 < L->x1);

    // fast path: the line lies wholly inside the row extents of the chains.
    // The rows are unclamped, so a line running off the screen (one carried
    // past its pre-clip column) fails it.
    if (st == ST_OPEN) {
        uint16_t ya = row_anchor(L->y0);
        uint16_t yb = line_y(L, (uint8_t)(L->x1 - 1));
        uint16_t lo = ya < yb ? ya : yb;
        uint16_t hi = ya > yb ? ya : yb;
        uint16_t hc = (uint16_t)(hi + 127);
        if (lo >= 128 && (uint8_t)((uint16_t)(lo - 128) >> 8) >= cx->tmax && hc > hi
            && (uint8_t)(hc >> 8) <= cx->bmin) {
            emit(cx, L, L->x0, L->x1, ST_VISIBLE);
            return;
        }
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
        // a block ends at the next piece boundary of each chain it involves
        uint8_t e = L->x1;
        uint8_t s = st;
        if (cx->bp == 0 || c < r->pool[cx->bp].x0) {
            // gap in the bottom chain (a closed range): the line is above the
            // top bound throughout
            s = ST_ABOVE;
            if (cx->bp && r->pool[cx->bp].x0 < e)
                e = r->pool[cx->bp].x0;
        } else if (s != ST_ABOVE && r->pool[cx->bp].x1 < e) {
            e = r->pool[cx->bp].x1;
        }
        if (s != ST_BELOW && r->pool[cx->tp].x1 < e)
            e = r->pool[cx->tp].x1;
        if (s == ST_OPEN)
            resolve_block(cx, L, c, e);
        else
            emit(cx, L, c, e, s);
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

// The edge's line at relative height h, pre-clipped to the screen: the part
// nearer than x = 2h projects past the screen's top or bottom edge (h's
// side), so the line stops at the column where it reaches that edge. The
// columns cut off, the rest of [e0.sx, e1.sx), are outside every portal on
// that side (clip_edge_line); the line is empty (x0 == x1) when both
// endpoints are too near.
static pt_line make_line(pt_ep e0, pt_ep e1, pt_relh h, pt_pattern col)
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

    pt_line L = {.x0 = e0.sx, .x1 = e0.sx, .ce = col.e, .co = col.o};
    if (near0 && near1)
        return L;
    if (!near0 && !near1) {
        uint8_t sy0 = row_from(h, &e0);
        uint8_t sy1 = row_from(h, &e1);
        uint8_t dx = (uint8_t)(e1.sx - e0.sx);
        L.x1 = e1.sx;
        L.y0 = sy0;
        L.g = sy1 >= sy0 ? pt_g_make(pt_grad_mag((uint8_t)(sy1 - sy0), dx), 0)
                         : pt_g_make(pt_grad_mag((uint8_t)(sy0 - sy1), dx), 1);
        return L;
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

    if (near0) {
        if (cs < e1.sx) {
            uint16_t dxq = (uint16_t)(((uint16_t)(e1.sx << 8)) - sxq);
            uint8_t fr = (uint8_t)(((uint16_t)(cs << 8)) - sxq);
            uint16_t base = (uint16_t)((edge << 8) | 128);    // rounding bias in the anchor
            uint16_t y0q;
            if (syf >= edge) {
                uint16_t gm = pt_gradq8_mag((uint8_t)(syf - edge), dxq);
                uint16_t off = pt_mulu16_frac(gm, fr);
                L.g = pt_g_make(gm, 0);
                y0q = (uint16_t)(base + off);
                if (y0q < base)
                    y0q = 0xFFFF;    // carry out: clamp
            } else {
                uint16_t gm = pt_gradq8_mag((uint8_t)(edge - syf), dxq);
                uint16_t off = pt_mulu16_frac(gm, fr);
                L.g = pt_g_make(gm, 1);
                y0q = off > base ? 0 : (uint16_t)(base - off);
            }
            // round like every other re-anchor; PT_SCREEN_HEIGHT is fine,
            // the line comes on screen where its gradient brings the row
            // under the screen edge less a half
            L.x0 = cs;
            L.x1 = e1.sx;
            L.y0 = row_of(y0q);
        }
    } else if (e0.sx < cs) {
        uint16_t dxq = (uint16_t)(sxq - ((uint16_t)(e0.sx << 8)));
        L.x1 = cs;
        L.y0 = syf;
        L.g = edge >= syf ? pt_g_make(pt_gradq8_mag((uint8_t)(edge - syf), dxq), 0)
                          : pt_g_make(pt_gradq8_mag((uint8_t)(syf - edge), dxq), 1);
    }
    return L;
}

// Clip an edge's line over this visit's columns [xa, xb). Columns outside
// the line's own span are the ones its screen pre-clip cut off: outside the
// portal on the side it left the screen by, so they only recolour that
// side's bound, with nothing evaluated. The ranges go left to right, as the
// clip cursors require.
static void clip_edge_line(pt_clip *cx, pt_line L, pt_relh h, uint8_t xa, uint8_t xb)
{
    uint8_t out = h.below ? ST_BELOW : ST_ABOVE;
    uint8_t l0 = L.x0 > xa ? L.x0 : xa;
    uint8_t l1 = L.x1 < xb ? L.x1 : xb;
    if (l0 >= l1)
        l0 = l1 = xb;    // cut off over the whole visit

    pt_line o = L;
    if (xa < l0) {
        o.x0 = xa;
        o.x1 = l0;
        clip_line(cx, &o, out);
    }
    if (l0 < l1) {
        if (L.x0 < l0)
            L.y0 = row_of(line_y(&L, l0));    // re-anchor at the visit's first column
        L.x0 = l0;
        L.x1 = l1;
        clip_line(cx, &L, ST_OPEN);
    }
    if (l1 < xb) {
        o.x0 = l1;
        o.x1 = xb;
        clip_line(cx, &o, out);
    }
}

// ---- vertex transform and classification (spec 3.2, 7.3) ----

// The classification is rounded, and the walk is built to survive that. The
// camera may legitimately stand ON its cell's boundary (the transition's
// exact tie rule can park it on a portal line, even on a fan vertex), where
// vertex sums near zero can round to either side. Nothing here needs to be
// exact, because every consumer is robust by structure: a child portal
// starts from its portal edge (or resumes a finished visit's edge), each
// portal's spans start no earlier than where its last one ended, and a ring
// that never fires its stop burns its steps and ends the walk. The one
// load-bearing consumer of these signs is root_portal's winding search on
// the CAMERA's cell - and that, once per frame at most, retries with exact
// sums when the rounded ones fail. At an exact boundary tie a portal may
// therefore carry a one-column sliver where perfection would need exact
// spans; it never traps.
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

// A new visit record for a cell's first visit of the frame.
static pt_visit *visit_new(pt_render *r, uint8_t cell)
{
    RC_PANIC(r->num_visits < PT_MAX_VISITS);
    uint8_t i = r->num_visits++;
    r->visits[i] = (pt_visit) {.cell = cell};
    r->cell_visit[cell] = i;
    return &r->visits[i];
}

// Move a visit's cursor to the next edge: its end vertex becomes the start,
// and that edge's lines start afresh.
static void visit_advance(pt_render *r, pt_visit *vs, const pt_cell *c)
{
    vs->k = ring_k_next(c, vs->k);
    vs->v0 = vs->v1;
    vs->v1 = transform(r, ring_vertex(r->map, c, ring_k_next(c, vs->k)));
    for (uint8_t i = 0; i < PT_NUM_SLOTS; i++)
        vs->active[i] = 0;
}

// The child portal over [xl, xr) through portal edge ei, ready to walk:
// its cell's record gets the cursor on the child's first edge (spec 7.4),
// the first whose end vertex is strictly right of xl. A first visit
// searches from P (pv), the portal's start vertex, whose edge is the
// twin's ring successor. A cell visited before resumes from the edge its
// last visit ended on - every earlier edge ends left of that visit's xr,
// which is at or left of xl - with that edge's vertices and its lines'
// active pieces in hand (extended if the visits are contiguous, and
// otherwise the lines' references for new pieces past the gap). False for
// a degenerate ring (no child).
static bool child_portal(pt_render *r, uint8_t ei, uint8_t xl, uint8_t xr, uint8_t thead,
                         uint8_t bhead, uint8_t tmax, uint8_t bmin, pt_vv pv, pt_portal *out)
{
    const pt_map *m = r->map;
    uint8_t cell = m->edges[ei].adj;
    const pt_cell *c = &m->cells[cell];

    pt_visit *vs;
    uint8_t vi = r->cell_visit[cell];
    if (vi != 0) {
        vs = &r->visits[vi];
        RC_ASSERT(vs->xr <= xl);    // portals never overlap: visits run left to right
    } else {
        vs = visit_new(r, cell);
        vs->k = ring_k_next(c, (uint8_t)(pt_map_twin(m, ei) - c->first_edge));
        vs->v0 = pv;
        vs->v1 = transform(r, ring_vertex(m, c, ring_k_next(c, vs->k)));
    }
    uint8_t guard = c->num_edges;
    while (!col_gt(vs->v1, xl)) {
        if (guard-- == 0) {
            RC_ASSERT(0);
            return false;    // degenerate ring; skip the child rather than spin
        }
        visit_advance(r, vs, c);
    }
    vs->xr = xr;

    *out = (pt_portal) {
        .cell = cell,
        .x = xl,
        .xr = xr,
        .tcur = thead,
        .bcur = bhead,
        .tmax = tmax,
        .bmin = bmin,
    };
    return true;
}

// The next open range of [*c, xb) left to right: skip the closed ranges
// covering *c, then run to the next closed range's start (or xb). False
// when nothing is left; *c moves past the range returned.
static bool next_open(const pt_closedl *cl, uint8_t *c, uint8_t xb, uint8_t *ol, uint8_t *orr)
{
    uint8_t s = *c;
    for (bool moved = true; moved;) {
        moved = false;
        for (uint8_t j = 0; j < cl->num; j++) {
            if (cl->l[j] <= s && s < cl->r[j]) {
                s = cl->r[j];
                moved = true;
            }
        }
    }
    if (s >= xb)
        return false;
    uint8_t e = xb;
    for (uint8_t j = 0; j < cl->num; j++)
        if (cl->l[j] > s && cl->l[j] < e)
            e = cl->l[j];
    *ol = s;
    *orr = e;
    *c = e;
    return true;
}

// Close open columns [l, r) after the fact (a pinched sliver that is not
// the child's portal): the child's whole stack there telescopes to
// ceiling ^ floor, EORed into its bottom chain's pieces over exactly those
// columns, as emit does for a closed range.
static void close_columns(pt_render *r, uint8_t head, uint8_t l, uint8_t rr, pt_pattern cbfb)
{
    uint8_t p = piece_at(r, head, l);
    if (r->pool[p].x0 < l) {
        pool_split(r, p, l);
        p = r->pool[p].next;
    }
    while (p && r->pool[p].x0 < rr) {
        if (r->pool[p].x1 > rr)
            pool_split(r, p, rr);
        r->pool[p].ce ^= cbfb.e;
        r->pool[p].co ^= cbfb.o;
        p = r->pool[p].next;
    }
}

// ---- one edge of the walk (spec 7.8) ----

// An edge's lines for one visit. A line with an active piece on this edge
// is already in screen space: the piece's x0, y0 and g are its line,
// which runs on to the edge's end column, so it is used as is -
// clip_edge_line starts it at the span's left column - with no projection
// or gradient division (the piece's start row is rounded, so a restart past
// an occluder may sit a row off the regenerated line; accepted). A line
// pre-clipped at the screen edge needs no exception: carried on past that
// column, its rows run off the screen, beyond every bound, and the clip
// recolours the bound there as the cut-off columns would have. Only a line
// with no active piece (0: not yet generated on this edge, or wholly hidden
// so far) is projected, and only then are the edge's endpoints side-clipped
// (the divisions), once for all its lines.
typedef struct pt_edgelines {
    const pt_render *r;
    const pt_visit *vs;
    pt_vv v0, v1;
    uint8_t sx0, sx1;
    bool have_ep;
    pt_ep e0, e1;
} pt_edgelines;

static pt_line edge_line(pt_edgelines *el, uint8_t slot, pt_relh h, pt_pattern col)
{
    uint8_t q = el->vs->active[slot];
    if (q) {
        const pt_piece *pp = &el->r->pool[q];
        return (pt_line) {.x0 = pp->x0, .x1 = el->sx1, .y0 = pp->y0, .g = pp->g,
                          .ce = col.e, .co = col.o};
    }

    if (!el->have_ep) {
        // side clip: the endpoints the lines are made from
        pt_vv v0 = el->v0;
        pt_vv v1 = el->v1;
        uint16_t b0;
        if (v0.flags & PT_VF_ANEG) {
            el->e0 = clip_left(v0, v1);
            b0 = (uint16_t)(el->e0.x << 1);    // on the left ray b = 2x
        } else if (v0.flags & PT_VF_BNEG) {
            el->e0 = ep_unclipped(v0, 128);
            b0 = 0;
        } else {
            el->e0 = ep_unclipped(v0, v0.sx);
            b0 = (uint16_t)(int16_t)(v0.x - v0.y);
        }
        if (v1.flags & PT_VF_BNEG)
            el->e1 = clip_right(el->e0.x, b0, v1);
        else
            el->e1 = ep_unclipped(v1, el->sx1);
        RC_ASSERT(el->e0.sx == el->sx0 && el->e1.sx == el->sx1);
        el->have_ep = true;
    }
    return make_line(el->e0, el->e1, h, col);
}

// Process the portal's current edge: its lines, and the child portal
// through it, if any, returned in *child.
static bool process_edge(pt_render *r, pt_portal *p, pt_portal *child)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[r->cell_visit[p->cell]];
    const pt_cell *ca = &m->cells[p->cell];
    uint8_t ei = (uint8_t)(ca->first_edge + vs->k);
    const pt_edge *e = &m->edges[ei];
    pt_vv v0 = vs->v0;
    pt_vv v1 = vs->v1;

    // 1. the column span, before any clipping, from the vertices' flags and
    // columns. It starts where the frame's last span ended (p->x, the
    // portal's left column for its first edge), or at the start vertex if
    // that is further right - it never is in a healthy ring: a first edge
    // starts at or left of the portal, and a later one at the previous
    // edge's end vertex. It ends at the end vertex's column. So an edge with
    // no columns, including every edge once the span has reached xr, costs
    // no division. Two edges have no columns whatever the columns say: one
    // with a < 0 at both ends lies wholly outside the left ray (even ending
    // behind the camera, where the end reads as the right ray), and one
    // starting on or past the right ray comes only after the stop, in a
    // ring that wrapped (the eye on its vertex). A vertex behind the camera
    // reads as the right ray at an edge's end (an edge from the frustum to
    // behind it leaves that way) and the left at its start.
    if ((v0.flags & PT_VF_ANEG) && (v1.flags & (PT_VF_ANEG | PT_VF_AZERO)))
        return false;
    if ((v1.flags & PT_VF_BNEG) && !(v0.flags & PT_VF_ANEG)
        && (v0.flags & (PT_VF_BNEG | PT_VF_BZERO)))
        return false;
    uint8_t sx0 = (v0.flags & PT_VF_ANEG) ? 0 : (v0.flags & PT_VF_BNEG) ? 128 : v0.sx;
    uint8_t sx1 = (v1.flags & PT_VF_BNEG) ? 128 : (v1.flags & PT_VF_ANEG) ? 0 : v1.sx;
    uint8_t xa = sx0 > p->x ? sx0 : p->x;
    uint8_t xb = sx1 < p->xr ? sx1 : p->xr;
    if (xa >= xb)
        return false;
    RC_ASSERT(r->pool[p->tcur].x0 <= xa && r->pool[p->bcur].x0 <= xa);    // never backwards
    p->x = xb;

    pt_edgelines el = {.r = r, .vs = vs, .v0 = v0, .v1 = v1, .sx0 = sx0, .sx1 = sx1};

    // 2. take ownership of the bound sub-chains over [xa, xb)
    uint8_t th = cut(r, &p->tcur, xa, xb);
    uint8_t bh = cut(r, &p->bcur, xa, xb);

    if (e->colour == PT_EDGE_SOFT)
        return child_portal(r, ei, xa, xb, th, bh, p->tmax, p->bmin, v0, child);

    pt_pattern wall = pt_shade((uint8_t)(e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_relh ha_c = rel_h(ca->ceil_z, r->eye_z);
    pt_relh ha_f = rel_h(ca->floor_z, r->eye_z);

    if (e->adj == 0) {
        // solid wall: ceiling line and floor line, recolour only; each line
        // gets fresh cursors (role NONE never severs the chains)
        pt_clip cx = {.r = r, .thead = th, .bhead = bh, .tmax = p->tmax, .bmin = p->bmin,
                      .role = ROLE_NONE, .vis = vs, .slot = PT_SLOT_CEIL};
        clip_edge_line(&cx, edge_line(&el, PT_SLOT_CEIL, ha_c, pt_pattern_xor(ca_ceil, wall)),
                       ha_c, xa, xb);
        cx.tp = 0;
        cx.bp_set = false;
        cx.slot = PT_SLOT_FLOOR;
        clip_edge_line(&cx, edge_line(&el, PT_SLOT_FLOOR, ha_f, pt_pattern_xor(wall, ca_floor)),
                       ha_f, xa, xb);
        return false;
    }

    const pt_cell *cb = &m->cells[e->adj];
    pt_pattern cb_ceil = pt_shade(cb->ceil);
    pt_pattern cb_floor = pt_shade(cb->floor);
    pt_pattern cbfb = pt_pattern_xor(cb_ceil, cb_floor);

    // 3. non-bound-forming lines first: they only recolour, and need the top
    // and bottom chains intact
    pt_clip cxn = {.r = r, .thead = th, .bhead = bh, .tmax = p->tmax, .bmin = p->bmin,
                   .role = ROLE_NONE, .vis = vs, .slot = PT_SLOT_CEIL};
    if (cb->ceil_z > ca->ceil_z) {
        clip_edge_line(&cxn, edge_line(&el, PT_SLOT_CEIL, ha_c, pt_pattern_xor(ca_ceil, wall)),
                       ha_c, xa, xb);
        cxn.tp = 0;
        cxn.bp_set = false;
    }
    if (cb->floor_z < ca->floor_z) {
        cxn.slot = PT_SLOT_FLOOR;
        clip_edge_line(&cxn, edge_line(&el, PT_SLOT_FLOOR, ha_f, pt_pattern_xor(wall, ca_floor)),
                       ha_f, xa, xb);
    }

    // 4. bound-forming lines, floor first: it builds the child's bottom chain
    // against T and B; the ceiling line then builds the child's top chain
    // against T and the chain the floor line just built
    pt_chainb childb = {.bmin = PT_SCREEN_HEIGHT};
    pt_chainb childt = {.bmin = PT_SCREEN_HEIGHT};
    pt_closedl closed = {0};

    pt_relh hf = ha_f;
    pt_pattern colf = pt_pattern_xor(cb_floor, ca_floor);
    if (cb->floor_z < ca->floor_z) {
        hf = rel_h(cb->floor_z, r->eye_z);
        colf = pt_pattern_xor(cb_floor, wall);
    }
    pt_clip cxf = {.r = r, .thead = th, .bhead = bh, .tmax = p->tmax, .bmin = p->bmin,
                   .role = ROLE_BOTTOM, .build = &childb, .cbfb = cbfb, .closed = &closed,
                   .vis = vs, .slot = PT_SLOT_FLOOR_BOUND};
    clip_edge_line(&cxf, edge_line(&el, PT_SLOT_FLOOR_BOUND, hf, colf), hf, xa, xb);

    pt_relh hc = ha_c;
    pt_pattern colc = pt_pattern_xor(ca_ceil, cb_ceil);
    if (cb->ceil_z > ca->ceil_z) {
        hc = rel_h(cb->ceil_z, r->eye_z);
        colc = pt_pattern_xor(wall, cb_ceil);
    }
    pt_clip cxc = {.r = r, .thead = th, .bhead = childb.head, .tmax = p->tmax,
                   .bmin = childb.head ? childb.bmin : 0, .role = ROLE_TOP, .build = &childt,
                   .cbfb = cbfb, .closed = &closed, .vis = vs, .slot = PT_SLOT_CEIL_BOUND};
    clip_edge_line(&cxc, edge_line(&el, PT_SLOT_CEIL_BOUND, hc, colc), hc, xa, xb);

    // 5. the child portal. Every portal opening is convex on screen (spec
    // 7.9.4), so the open columns form one interval and closed ranges only
    // trim the span's ends. Rounding can still pinch a sliver-thin opening
    // (under two rows) shut somewhere in the middle; then the widest open
    // range is the portal, and the other open columns are closed too, with
    // the same fix-up.
    uint8_t pl = 0;
    uint8_t pr = 0;
    uint8_t ol, orr;
    for (uint8_t c = xa; next_open(&closed, &c, xb, &ol, &orr);) {
        if (orr - ol > pr - pl) {
            pl = ol;
            pr = orr;
        }
    }
    if (closed.num) {
        for (uint8_t c = xa; next_open(&closed, &c, xb, &ol, &orr);)
            if (ol != pl)
                close_columns(r, childb.head, ol, orr, cbfb);    // only ever on a pinch
    }
    return pl < pr
        && child_portal(r, ei, pl, pr, piece_at(r, childt.head, pl), piece_at(r, childb.head, pl),
                        childt.tmax, childb.bmin, v0, child);
}

// ---- the frame (spec 7.1, 7.4, 7.5) ----

// The exact retry behind root_portal: the rounded winding search can fail
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

// Find the camera cell's first edge (where a goes from <= 0 to > 0) and
// return the root portal, the whole screen. The searches are guarded: with the eye on the
// cell's boundary the rounded sums can read one-sided, and the exact retry
// decides instead (see root_exact). A ring the retry roots at a zero vertex
// has no closing column, so the walk ends by its steps - accepted, not an
// error.
static pt_portal root_portal(pt_render *r, uint8_t roott, uint8_t rootb)
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
    pt_visit *vs = visit_new(r, r->cam_cell);
    vs->k = (uint8_t)(n - c->first_edge);
    vs->xr = 128;
    vs->v0 = v;
    vs->v1 = w;
    return (pt_portal) {
        .cell = r->cam_cell,
        .x = 0,
        .xr = 128,
        .tcur = roott,
        .bcur = rootb,
        .tmax = 0,
        .bmin = PT_SCREEN_HEIGHT,
    };
}

static void walk(pt_render *r, pt_portal p, uint8_t depth);

// Walk a child portal one level deeper, or, past the depth cap, close its
// columns instead: the fix-up stands in for its whole stack, so the
// opening shows the child's ceiling colour flat - a draw-distance cutoff
// rather than a glitch.
static void descend(pt_render *r, const pt_portal *child, uint8_t depth)
{
    if (depth > PT_MAX_DEPTH) {
        const pt_cell *c = &r->map->cells[child->cell];
        pt_pattern cbfb = pt_pattern_xor(pt_shade(c->ceil), pt_shade(c->floor));
        close_columns(r, child->bcur, child->x, child->xr, cbfb);
        return;
    }
    walk(r, *child, depth);
}

// Walk a portal (spec 7.5): each edge of its cell from the one the visit
// record's cursor is on, until an edge ends at or right of xr. A child
// through any earlier edge is walked recursively, and the portal carries on
// from its own column when it returns; the child through the last edge is
// walked in this same call (a tail call: on the 6502 a JMP, no stack), so
// the depth grows only with portals nested along a line of sight. A healthy
// ring stops on col_ge before its steps run out; a ring rooted at a zero
// vertex (the eye on it, only ever the camera's cell) has no closing
// column, no stop fires, and the walk wraps through empty spans until the
// steps end it - an accepted outcome, not an error.
static void walk(pt_render *r, pt_portal p, uint8_t depth)
{
    const pt_map *m = r->map;
    if (depth > r->depth_high)
        r->depth_high = depth;

    for (;;) {
        pt_visit *vs = &r->visits[r->cell_visit[p.cell]];
        const pt_cell *c = &m->cells[p.cell];
        uint8_t steps = (uint8_t)(c->num_edges + 1);
        for (;;) {
            if (steps-- == 0)
                return;

            // the visit holds both vertices of its current edge: each vertex
            // is transformed once, as the end of one edge, and carried on as
            // the start of the next; the last edge leaves the cursor on it
            // for a later visit to resume from
            bool last = col_ge(vs->v1, p.xr);
            pt_portal child;
            bool has_child = process_edge(r, &p, &child);
            if (last) {
                if (!has_child)
                    return;
                p = child;    // the tail call
                break;
            }
            if (has_child)
                descend(r, &child, (uint8_t)(depth + 1));
            visit_advance(r, vs, c);
        }
    }
}

// ---- plotting (spec 8.1) ----

// The pixels of one byte of a shallow line (|g| < 1), by which of the three
// steps inside the byte carried into the next row (bit 2 the step after the
// leftmost pixel): the pixel mask of each row the byte touches, in order,
// 0 after the last.
static const uint8_t sprite_[8][4] = {
    {0xFF},                      // all four pixels on one row
    {0xEE, 0x11},
    {0xCC, 0x33},
    {0xCC, 0x22, 0x11},
    {0x88, 0x77},
    {0x88, 0x66, 0x11},
    {0x88, 0x44, 0x33},
    {0x88, 0x44, 0x22, 0x11},    // a row per pixel
};

// The pixels from p to the byte's right end, and from its left end to p.
static const uint8_t mask_from_[4] = {0xFF, 0x77, 0x33, 0x11};
static const uint8_t mask_to_[4] = {0x88, 0xCC, 0xEE, 0xFF};

// One column's step of a falling line's row fraction: the carry, 1 when the
// next column is a row down.
static uint8_t carry_down(uint8_t *f, uint8_t gm)
{
    uint16_t s = (uint16_t)(*f + gm);
    *f = (uint8_t)s;
    return (uint8_t)(s >> 8);
}

// And a rising line's: the borrow, 1 when the next column is a row up.
static uint8_t borrow_up(uint8_t *f, uint8_t gm)
{
    uint8_t b = *f < gm;
    *f = (uint8_t)(*f - gm);
    return b;
}

// The lines are plotted as the 6502 will, through a screen cursor (a
// character block's address and Y, pt_mode5_cur) placed once at the piece's
// first pixel and then only moved: a row down or up (Y by two, a character
// row on at a block's edge), a byte right, or a pixel right (the mask).

// A shallow line: its row moves by at most one per column, so it is kept as
// a row byte and a fraction, and each step's carry out of the fraction says
// whether the next column is a row on. Whole bytes go four pixels at a time:
// the carries of the three steps inside the byte pick a sprite of up to four
// row masks, and the fourth step's carry moves the row on for the next byte.
// The partial bytes at either end go a pixel at a time. A flat line has no
// steps: whole bytes on one row, the end bytes masked.
static void plot_shallow(uint8_t *fb, const pt_piece *p, pt_pattern pat)
{
    uint8_t gm = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = 128;    // the rounding bias, as row_anchor
    uint8_t c = p->x0;

    if (gm == 0) {
        uint8_t n = (uint8_t)(((uint8_t)(p->x1 - 1) >> 2) - (p->x0 >> 2));    // bytes after the first
        uint8_t m0 = mask_from_[p->x0 & 3];
        uint8_t m1 = mask_to_[(p->x1 - 1) & 3];
        if (n == 0) {
            pt_mode5_eor(fb, cur, m0 & m1, pat);
            return;
        }
        pt_mode5_eor(fb, cur, m0, pat);
        while (--n) {
            pt_mode5_next(&cur);
            pt_mode5_eor(fb, cur, 0xFF, pat);
        }
        pt_mode5_next(&cur);
        pt_mode5_eor(fb, cur, m1, pat);
    } else if (!pt_g_isneg(p->g)) {
        for (; (c & 3) != 0 && c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (carry_down(&f, gm))
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4)) {
            uint8_t i = carry_down(&f, gm);
            i = (uint8_t)((i << 1) | carry_down(&f, gm));
            i = (uint8_t)((i << 1) | carry_down(&f, gm));
            const uint8_t *sp = sprite_[i];
            pt_mode5_eor(fb, cur, sp[0], pat);
            for (uint8_t k = 1; k < 4 && sp[k]; k++) {
                pt_mode5_down(&cur);
                pt_mode5_eor(fb, cur, sp[k], pat);
            }
            if (carry_down(&f, gm))
                pt_mode5_down(&cur);
            pt_mode5_next(&cur);
        }
        for (; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (carry_down(&f, gm))
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
    } else {
        for (; (c & 3) != 0 && c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (borrow_up(&f, gm))
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4)) {
            uint8_t i = borrow_up(&f, gm);
            i = (uint8_t)((i << 1) | borrow_up(&f, gm));
            i = (uint8_t)((i << 1) | borrow_up(&f, gm));
            const uint8_t *sp = sprite_[i];
            pt_mode5_eor(fb, cur, sp[0], pat);
            for (uint8_t k = 1; k < 4 && sp[k]; k++) {
                pt_mode5_up(&cur);
                pt_mode5_eor(fb, cur, sp[k], pat);
            }
            if (borrow_up(&f, gm))
                pt_mode5_up(&cur);
            pt_mode5_next(&cur);
        }
        for (; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (borrow_up(&f, gm))
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
    }
}

// A steep line (|g| >= 1): a pixel pair per column, the row moving on by
// the gradient's whole rows plus the fraction's carry, a row at a time.
static void plot_steep(uint8_t *fb, const pt_piece *p, pt_pattern pat)
{
    uint8_t gi = (uint8_t)(pt_g_mag(p->g) >> 8);
    uint8_t gf = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = 128;    // the rounding bias, as row_anchor

    if (!pt_g_isneg(p->g)) {
        for (uint8_t c = p->x0; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            for (uint8_t n = (uint8_t)(gi + carry_down(&f, gf)); n != 0; n--)
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
    } else {
        for (uint8_t c = p->x0; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            for (uint8_t n = (uint8_t)(gi + borrow_up(&f, gf)); n != 0; n--)
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
    }
}

// Plot every pool piece with a colour: shallow lines (|g| < 1) a byte at a
// time, steep ones a pixel pair per column, falling or rising, both pure
// unsigned DDA. Rows round to nearest, the same convention every re-anchor
// uses: a piece split at a cut then plots the same rows the unsplit line
// would have, so chain seams cannot notch the boundary. Nothing is bounds
// checked: pieces start on the screen or at its bottom edge, and the
// rounding accumulated along a line carries it at most a row or so past the
// top or bottom, into the junk rows. A piece starting on the bottom edge
// that does not rise is wholly below the screen, and skipped.
static void plot(pt_render *r)
{
    for (uint16_t i = 1; i < r->pool_num; i++) {
        const pt_piece *p = &r->pool[i];
        if ((uint8_t)(p->ce | p->co) == 0)
            continue;
        if (p->y0 >= PT_SCREEN_HEIGHT && !pt_g_isneg(p->g))
            continue;
        pt_pattern pat = {.e = p->ce, .o = p->co};
        if (pt_g_mag(p->g) < 0x100)
            plot_shallow(r->fb, p, pat);
        else
            plot_steep(r->fb, p, pat);
    }
}

void pt_render_init(pt_render *r, const pt_map *m)
{
    *r = (pt_render) {0};
    r->map = m;
    r->fb = r->fbmem + PT_FB_GUARD;
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
    r->num_visits = 1;

    // the root portal: a plotted top bound seeding the fill with the camera
    // cell's ceiling, and a dummy off-screen bottom bound
    pt_pattern ceil = pt_shade(m->cells[cell].ceil);
    uint8_t roott = pool_alloc(r, 0, 128, 0, 0, ceil.e, ceil.o);
    uint8_t rootb = pool_alloc(r, 0, 128, PT_SCREEN_HEIGHT, 0, 0, 0);
    walk(r, root_portal(r, roott, rootb), 1);

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
