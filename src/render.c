// render.c - the capture walk (spec section 7), line generation and bound
// clipping, EOR plotting and the fill. All per-frame arithmetic goes through
// the table helpers in arith.h; sums and differences are two's complement,
// while every multiply and divide runs on unsigned magnitudes.

#include "render.h"

#include <stdbool.h>

// clip statuses: where a line sits against the portal's bounds at a column
enum {
    ST_ABOVE,      // hidden above the top bound: merged into the top piece
    ST_BELOW,      // hidden below the bottom bound: merged into the bottom piece
    ST_VISIBLE,    // between the bounds: a piece of its own
    ST_NONE,       // (a clip's shut status) the line forms neither of the child's bounds
};

// A candidate line from make_line, before clipping against the bounds: on
// the screen over [x0, x1), and beyond its edge on the side below says
// either side of that (the screen pre-clip cut it off there).
typedef struct pt_line {
    uint8_t x0;
    uint8_t x1;
    uint16_t y;       // the row at x0, u8.8 with the rounding bias in
    uint16_t g;       // sign bit + u7.8 magnitude
    uint8_t below;    // the side it is cut off on: below the screen, or above
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

// One line's clip (spec 7.9): the bounds' pieces it has reached, its slot on
// its visit's edge, and, for a line forming one of the child's bounds, the
// child's visit it builds that bound in. Where the line closes the child
// (spec 7.9.4) a run of open columns ends; the child is the first.
typedef struct pt_clip {
    uint8_t tp, bp;       // the top and bottom bounds' pieces at or before the column reached
    uint8_t tmax, bmin;   // the bounds' row extents, for the fast path
    uint8_t vis;          // the visit whose edge the line is on
    uint8_t slot;         // and its slot there
    uint8_t shut;         // the status that closes the child: ST_ABOVE for the line
                          // forming its bottom, ST_BELOW its top, else ST_NONE
    uint8_t child;        // the visit whose bound the line builds
    uint8_t tail;         // that bound's last piece so far, 0 = none yet
    uint8_t own;          // the line's last range so far: 0 none, 1 hidden, 2 visible
    uint8_t lc;           // the line's row in hand: its column plus one (0 = none)...
    uint16_t ly;          // ...and the row
    pt_pattern cbfb;      // the child's ceiling ^ floor: its whole stack, telescoped
    uint8_t rl, rr;       // the open run being tallied, [rl, rr) (empty: none)
    uint8_t ol, orr;      // the first, the child's (empty: none yet)
} pt_clip;

// ---- row arithmetic ----
//
// Every u8.8 row (piece_y, line_y, the plot DDA) carries the +128 rounding
// bias in its anchor, so rounding to the nearest row is a plain high-byte
// read (row_of). A line placed on a whole row is anchored at its middle; a
// piece split off a line, or started partway along one, keeps the exact
// row there, fraction and all. So every piece of a line plots exactly the
// rows the line has, and the clip compares exactly what is plotted: a split
// never moves a row. The bias cancels in every line-vs-bound difference;
// only the conservative extents peel it off again, so floor is
// (y - 128) >> 8 and ceil is (y + 127) >> 8.

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

// A row one column on: row_step for one column, a plain add (the plotter's
// own step, so the clip rows are the plotted rows).
static uint16_t step1(uint16_t y, uint16_t g)
{
    uint16_t gm = pt_g_mag(g);
    if (pt_g_isneg(g))
        return gm > y ? 0 : (uint16_t)(y - gm);
    uint16_t s = (uint16_t)(y + gm);
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

// A piece starting at row y (biased u8.8). One starting further below the
// screen than its bottom edge starts on it: a hidden piece's line past its
// pre-clip column, which stays below every bound there either way.
static uint8_t pool_alloc(pt_render *r, uint8_t x0, uint8_t x1, uint16_t y, uint16_t g,
                          uint8_t ce, uint8_t co)
{
    RC_PANIC(r->pool_num < PT_POOL_SIZE);
    RC_ASSERT(x0 < x1);

    if ((y >> 8) > PT_SCREEN_HEIGHT)
        y = row_anchor(PT_SCREEN_HEIGHT);
    uint8_t i = (uint8_t)r->pool_num++;
    r->pool[i] = (pt_piece) {.x0 = x0, .x1 = x1, .y0 = (uint8_t)(y >> 8), .yf = (uint8_t)y,
                             .g = g, .ce = ce, .co = co, .next = 0};
    return i;
}

// Row of a piece at column c in u8.8; valid for c at or beyond x0.
static uint16_t piece_y(const pt_render *r, uint8_t p, uint8_t c)
{
    const pt_piece *pp = &r->pool[p];
    RC_ASSERT(c >= pp->x0);
    return row_step((uint16_t)((pp->y0 << 8) | pp->yf), pp->g, (uint8_t)(c - pp->x0));
}

// Row of a line at column c in u8.8. Where it was cut off at the screen's
// edge it is beyond every bound on that side: the furthest row there is.
static uint16_t line_y(const pt_line *l, uint8_t c)
{
    if (c < l->x0 || c >= l->x1)
        return l->below ? 0xFFFF : 0;
    return row_step(l->y, l->g, (uint8_t)(c - l->x0));
}

// The line's row at column c for its clip: the row in hand, or one column on
// from it (a step, no product), or else worked out; it's then in hand.
static uint16_t line_at(pt_clip *cx, const pt_line *l, uint8_t c)
{
    uint16_t y;
    if (cx->lc == c + 1) {
        y = cx->ly;
    } else if (cx->lc == c && c > l->x0 && c < l->x1) {
        y = step1(cx->ly, l->g);
    } else {
        y = line_y(l, c);
    }
    cx->lc = (uint8_t)(c + 1);
    cx->ly = y;
    return y;
}

// The end of the stretch of columns from c over which the line is one
// linear function: its first column, its end, or neither.
static uint8_t line_stretch(const pt_line *l, uint8_t c, uint8_t c1)
{
    uint8_t e = c < l->x0 ? l->x0 : c < l->x1 ? l->x1 : c1;
    return e < c1 ? e : c1;
}

// Split piece p at column c, where its row is y: p keeps [x0, c), the new
// piece takes [c, x1), starting on p's exact row there.
static void pool_split(pt_render *r, uint8_t p, uint8_t c, uint16_t y)
{
    pt_piece *pp = &r->pool[p];
    RC_ASSERT(c > pp->x0 && c < pp->x1);

    uint8_t q = pool_alloc(r, c, pp->x1, y, pp->g, pp->ce, pp->co);
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

// ---- clipping a line against the bounds (spec 7.9) ----
//
// A bound is a chain of pieces, left to right. A hidden stretch of a line
// is merged into the bound piece hiding it, split from it only at the
// stretch's ends. A bound-forming line builds the child's bound out of its
// own visible pieces and twins of the parent's pieces where it is hidden,
// so a chain, once built, only ever gains pieces by splits.

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

// The line's own piece over range [r0, r1), status st: coloured where it is
// visible, not where it is hidden (kept so that it can be extended there).
// The line's last piece grows if it ends here in that colour, keeping its
// anchor; else a new piece starts at y, the line's row at r0 (which the clip
// has in hand already). A bound-forming line's visible pieces
// are in the child's bound, so for one of those the piece must be visible
// or hidden alike too (a bound line can be uncoloured: the child's floor
// the same as the parent's), unless the piece is from an earlier visit.
static uint8_t own_piece(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1,
                         uint8_t st, uint16_t y)
{
    bool vis = st == ST_VISIBLE;
    uint8_t ce = vis ? L->ce : 0;
    uint8_t co = vis ? L->co : 0;
    uint8_t *a = &r->visits[cx->vis].active[cx->slot];
    pt_piece *q = &r->pool[*a];
    uint8_t own = vis ? 2 : 1;
    bool alike = cx->shut == ST_NONE || cx->own == 0 || cx->own == own;
    cx->own = own;
    if (*a != 0 && q->x1 == r0 && q->ce == ce && q->co == co && alike) {
        q->x1 = r1;
        return *a;
    }
    *a = pool_alloc(r, r0, r1, y, L->g, ce, co);
    return *a;
}

// Link piece q, over columns up to r1, into the bound the line builds: its
// head, or after its tail (q may be the tail already, extended). The bound's
// row extent takes in the new columns: the bottom's smallest row, the top's
// largest. A piece is straight, so that's at one end of them, the gradient's
// sign says which; y is the row at their first, in hand. q is the line's own
// piece, or a twin: in no other chain still walked.
static void chain_add(pt_render *r, pt_clip *cx, uint8_t q, uint8_t r1, uint16_t y)
{
    pt_visit *cv = &r->visits[cx->child];
    bool bottom = cx->shut == ST_ABOVE;
    if (cx->tail != q) {
        r->pool[q].next = 0;
        RC_ASSERT(cx->tail == 0 || r->pool[cx->tail].x1 <= r->pool[q].x0);
        if (cx->tail != 0)
            r->pool[cx->tail].next = q;
        else if (bottom)
            cv->bcur = q;
        else
            cv->tcur = q;
        cx->tail = q;
    }
    if (bottom == pt_g_isneg(r->pool[q].g))    // the far end: a rising bottom, a falling top
        y = piece_y(r, q, (uint8_t)(r1 - 1));
    if (bottom) {
        uint8_t lo = row_of_floor(y);
        if (lo < cv->bmin)
            cv->bmin = lo;
    } else {
        uint8_t hi = row_of_ceil(y);
        if (hi > cv->tmax)
            cv->tmax = hi;
    }
}

// Close columns [l, r) of a bound after the fact: the child's whole stack
// there telescopes to ceiling ^ floor, EORed into the bound's pieces over
// exactly those columns, as a merge does where the line closes the child.
static void close_columns(pt_render *r, uint8_t head, uint8_t l, uint8_t rr, pt_pattern cbfb)
{
    uint8_t p = piece_at(r, head, l);
    if (r->pool[p].x0 < l) {
        pool_split(r, p, l, piece_y(r, p, l));
        p = r->pool[p].next;
    }
    while (p && r->pool[p].x0 < rr) {
        if (r->pool[p].x1 > rr)
            pool_split(r, p, rr, piece_y(r, p, rr));
        r->pool[p].ce ^= cbfb.e;
        r->pool[p].co ^= cbfb.o;
        p = r->pool[p].next;
    }
}

// Columns [r0, r1) where the line leaves the child open: the run goes on.
static void run_open(pt_clip *cx, uint8_t r0, uint8_t r1)
{
    if (cx->rl == cx->rr)
        cx->rl = r0;
    cx->rr = r1;
}

// The open run ends. Every portal opening is convex on screen (spec 7.9.4),
// so its open columns are one run, closed only at its ends, and the child
// is that run. Exact rows keep it so but for a crossing's rounding, which
// could pinch a sliver-thin opening shut in the middle; then the child is
// the first run, and any later one is closed after the fact.
static void run_end(pt_render *r, pt_clip *cx)
{
    if (cx->rl == cx->rr)
        return;
    if (cx->ol == cx->orr) {
        cx->ol = cx->rl;
        cx->orr = cx->rr;
    } else {
        close_columns(r, r->visits[cx->child].bcur, cx->rl, cx->rr, cx->cbfb);
    }
    cx->rl = cx->rr = 0;
}

// Merge the line's colour into the bound piece on side st over exactly
// [r0, r1) (spec 7.9.1: the same as plotting the line on the bound), split
// from it only at the range's ends, and only if there is a colour to merge.
// Where the line closes the child, the child's stack goes in with it. Where
// the line forms the child's bound, the bound there is a twin of the piece:
// a new one on the same rows exactly, uncoloured until the child's own
// lines merge into it. So the parent's bound is never relinked: splits only
// ever add to a chain, and its cursors stay good. With own, the line keeps
// a hidden piece of its own there, where it is on the screen. yl and yp are
// the line's and the piece's rows at r0, in hand (the own piece's start, and
// the split's and the twin's; yl is not needed without own).
static void merge(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1,
                  uint8_t st, bool own, uint16_t yl, uint16_t yp)
{
    uint8_t *pp = st == ST_ABOVE ? &cx->tp : &cx->bp;
    uint8_t p = *pp;
    pt_pattern col = {.e = L->ce, .o = L->co};
    bool shut = st == cx->shut;
    if (shut)
        col = pt_pattern_xor(col, cx->cbfb);
    if ((col.e | col.o) != 0) {
        if (r->pool[p].x0 < r0) {
            pool_split(r, p, r0, yp);
            p = *pp = r->pool[p].next;
        }
        if (r->pool[p].x1 > r1)
            pool_split(r, p, r1, piece_y(r, p, r1));
        r->pool[p].ce ^= col.e;
        r->pool[p].co ^= col.o;
    }

    if (shut) {
        run_end(r, cx);
    } else if (cx->shut != ST_NONE) {
        chain_add(r, cx, pool_alloc(r, r0, r1, yp, r->pool[p].g, 0, 0), r1, yp);
        run_open(cx, r0, r1);
    }
    if (own) {
        uint8_t o0 = r0 > L->x0 ? r0 : L->x0;
        uint8_t o1 = r1 < L->x1 ? r1 : L->x1;
        if (o0 < o1)
            own_piece(r, cx, L, o0, o1, st, o0 == r0 ? yl : line_at(cx, L, o0));
    }
}

// A visible range: the line's own piece, coloured, and in the child's bound
// if it forms one. yl is the line's row at r0.
static void visible(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1,
                    uint16_t yl)
{
    uint8_t q = own_piece(r, cx, L, r0, r1, ST_VISIBLE, yl);
    if (cx->shut != ST_NONE) {
        chain_add(r, cx, q, r1, yl);
        run_open(cx, r0, r1);
    }
}

// Is the line, at rows yl, on side st of a bound at rows yp (on it counts:
// a line on a bound merges, the same pixels with one fewer plot)?
static bool beyond(uint16_t yl, uint16_t yp, uint8_t st)
{
    return st == ST_ABOVE ? yl <= yp : yl >= yp;
}

// The first column of [c, e) where the line, against bound piece p, is no
// longer beyond it on side st (with in) or is (without), or e; yl and yp are
// their rows at c. A stretch of the line at a time: over each both are
// single lines, so their difference changes sign at most once (and a
// stretch where the line was cut off is beyond on its side throughout).
static uint8_t flip(const pt_render *r, pt_clip *cx, const pt_line *L, uint8_t p, uint8_t c,
                    uint8_t e, uint8_t st, bool in, uint16_t yl, uint16_t yp)
{
    for (;;) {
        uint8_t s = line_stretch(L, c, e);
        uint8_t last = (uint8_t)(s - 1);
        if (beyond(yl, yp, st) != in)
            return c;
        if (beyond(line_at(cx, L, last), piece_y(r, p, last), st) != in) {
            uint16_t dg = grad_diff(L->g, r->pool[p].g);
            uint16_t d0 = (st == ST_ABOVE) == in ? (uint16_t)(yp - yl) : (uint16_t)(yl - yp);
            return dg != 0 ? crossing(c, s, d0, dg) : last;
        }
        if (s >= e)
            return e;
        c = s;
        yl = line_at(cx, L, c);
        yp = piece_y(r, p, c);
    }
}

// A hidden run from column c, the line beyond the bound on side st there
// (yl and yp, the line's and the bound's rows at c): it merges into that
// bound's pieces one after another until it comes back inside, or c1. The
// run's end.
static uint8_t hide(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t c, uint8_t c1,
                    uint8_t st, uint16_t yl, uint16_t yp)
{
    uint8_t *pp = st == ST_ABOVE ? &cx->tp : &cx->bp;
    for (;;) {
        uint8_t p = *pp = piece_at(r, *pp, c);
        uint8_t e = r->pool[p].x1 < c1 ? r->pool[p].x1 : c1;
        uint8_t x = flip(r, cx, L, p, c, e, st, true, yl, yp);
        if (x == c)
            return c;    // back inside where the piece starts
        merge(r, cx, L, c, x, st, true, yl, yp);
        if (x < e || x >= c1)
            return x;
        c = x;
        yl = line_at(cx, L, c);    // one on from the stretch's last: a step
        yp = piece_y(r, piece_at(r, *pp, c), c);    // the next piece's start: no product
    }
}

// Where a line inside the bounds at column c first meets the bound on side
// st (reaches it or goes beyond), walking that bound's pieces from p; c1 if
// it doesn't before then. yl and yp are the line's and p's rows at c.
static uint8_t meet(const pt_render *r, pt_clip *cx, const pt_line *L, uint8_t p, uint8_t c,
                    uint8_t c1, uint8_t st, uint16_t yl, uint16_t yp)
{
    for (;;) {
        p = piece_at(r, p, c);
        uint8_t e = r->pool[p].x1 < c1 ? r->pool[p].x1 : c1;
        uint8_t x = flip(r, cx, L, p, c, e, st, false, yl, yp);
        if (x < e || e >= c1)
            return x;
        c = e;
        yl = line_at(cx, L, c);    // one on from the stretch's last: a step
        yp = piece_y(r, piece_at(r, p, c), c);    // the next piece's start: no product
    }
}

// The fast path's test: is a row (biased u8.8, unclamped) inside the
// bounds' extents, at or below the top's lowest row and at or above the
// bottom's highest?
static bool inside(uint16_t y, const pt_clip *cx)
{
    uint16_t hc = (uint16_t)(y + 127);
    return y >= 128 && (uint8_t)((uint16_t)(y - 128) >> 8) >= cx->tmax && hc > y
        && (uint8_t)(hc >> 8) <= cx->bmin;
}

// Clip line L over a visit's columns [c, c1) against the bounds, left to
// right, a run at a time: a run goes on as long as the line stays beyond one
// bound, or between both, so a bound piece is split only where a run ends
// inside it. Columns where the line was cut off at the screen's edge are
// beyond every bound on that side, and join the run there. The fast path
// takes a line lying wholly inside the bounds' row extents whole. Its rows
// are unclamped, so a line cut off, or carried past its pre-clip column
// (running off the screen), never passes it. A line is straight, so it's
// inside if both its ends are; the last column's row is worked out only if
// the first's passes. Each run starts from the rows its first column's test
// took, the line's and the bounds'.
static void clip_line(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t c, uint8_t c1)
{
    RC_ASSERT(c < c1);
    cx->lc = 0;
    uint16_t yl = line_at(cx, L, c);
    if (inside(yl, cx) && inside(line_at(cx, L, (uint8_t)(c1 - 1)), cx)) {
        visible(r, cx, L, c, c1, yl);
        return;
    }

    for (;;) {
        cx->tp = piece_at(r, cx->tp, c);
        cx->bp = piece_at(r, cx->bp, c);
        uint16_t yt = piece_y(r, cx->tp, c);
        if (beyond(yl, yt, ST_ABOVE)) {
            c = hide(r, cx, L, c, c1, ST_ABOVE, yl, yt);
        } else {
            uint16_t yb = piece_y(r, cx->bp, c);
            if (beyond(yl, yb, ST_BELOW)) {
                c = hide(r, cx, L, c, c1, ST_BELOW, yl, yb);
            } else {
                uint8_t e = meet(r, cx, L, cx->tp, c, c1, ST_ABOVE, yl, yt);
                e = meet(r, cx, L, cx->bp, c, e, ST_BELOW, yl, yb);
                visible(r, cx, L, c, e, yl);
                c = e;
            }
        }
        if (c >= c1)
            return;
        yl = line_at(cx, L, c);
    }
}

// Columns [c, c1) where the floor line closed the child: the ceiling line
// is above the top there too, and merges into it with nothing evaluated. It
// keeps no piece of its own there.
static void merge_above(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t c, uint8_t c1)
{
    while (c < c1) {
        cx->tp = piece_at(r, cx->tp, c);
        uint8_t e = r->pool[cx->tp].x1 < c1 ? r->pool[cx->tp].x1 : c1;
        merge(r, cx, L, c, e, ST_ABOVE, false, 0, piece_y(r, cx->tp, c));
        c = e;
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
// that side (line_y); the line is empty (x0 == x1) when both endpoints are
// too near.
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

    pt_line L = {.x0 = e0.sx, .x1 = e0.sx, .below = h.below, .ce = col.e, .co = col.o};
    if (near0 && near1)
        return L;
    if (!near0 && !near1) {
        uint8_t sy0 = row_from(h, &e0);
        uint8_t sy1 = row_from(h, &e1);
        uint8_t dx = (uint8_t)(e1.sx - e0.sx);
        L.x1 = e1.sx;
        L.y = row_anchor(sy0);
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
            L.y = y0q;
        }
    } else if (e0.sx < cs) {
        uint16_t dxq = (uint16_t)(sxq - ((uint16_t)(e0.sx << 8)));
        L.x1 = cs;
        L.y = row_anchor(syf);
        L.g = edge >= syf ? pt_g_make(pt_gradq8_mag((uint8_t)(edge - syf), dxq), 0)
                          : pt_g_make(pt_gradq8_mag((uint8_t)(syf - edge), dxq), 1);
    }
    return L;
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

// ---- pushing portals ----

// The ring successor of ring index k.
static uint8_t ring_k_next(const pt_cell *c, uint8_t k)
{
    return (uint8_t)(k + 1 == (c->num + 1) ? 0 : k + 1);
}

// The start vertex of the cell's edge at ring index k.
static pt_vertex ring_vertex(const pt_map *m, const pt_cell *c, uint8_t k)
{
    return m->verts[m->halves[c->first + k].v0];
}

// ---- sprite instances (spec 9.2 - 9.4) ----

// Project a cell's sprite on the cell's first visit (spec 9.2): the scale,
// the top-left corner and the columns covered, or 0 when it can't be seen
// (none, behind, too near, or wholly off the screen).
static uint8_t inst_new(pt_render *r, uint8_t cell)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[cell];
    if (c->sprite == 0)
        return 0;
    const pt_sprite *sp = &m->sprites[c->sprite];
    const pt_sprite_art *a = &m->arts[sp->art];
    pt_vv v = transform(r, (pt_vertex) {.x = sp->x, .y = sp->y});
    if (v.x < PT_SPRITE_NEAR)
        return 0;
    uint16_t depth = (uint16_t)v.x;
    pt_recip rd = pt_recip16(depth);    // transform forms one only inside the view cone,
                                        // but a sprite can be on screen with its centre outside

    // screen pixels per source pixel: the height's projection over its rows,
    // 64 * size / (h * depth)
    uint8_t w = pt_sprite_width(a);
    uint8_t h = pt_sprite_height(a);
    uint16_t hd = pt_mulg_u16(depth, h);
    uint16_t scale = pt_project_q8(sp->size, hd, pt_recip16(hd));
    if (scale == 0)
        return 0;

    // the base's centre: its column from the lateral offset, its row from
    // the floor's height against the eye; then the corner, the width
    // centred and the height above
    uint16_t dy = pt_project_q8((uint16_t)(v.y < 0 ? -v.y : v.y), depth, rd);
    int32_t centre = v.y < 0 ? (64 << 8) - dy : (64 << 8) + dy;
    int16_t fz = (int16_t)(c->floor_z - r->eye_z);    // below the eye when positive
    uint16_t dz = pt_project_q8((uint16_t)(8 * (fz < 0 ? -fz : fz)), depth, rd);
    int32_t bottom = fz < 0 ? (PT_SCREEN_HEIGHT / 2 << 8) - dz : (PT_SCREEN_HEIGHT / 2 << 8) + dz;
    uint16_t width = pt_mulg_u16(scale, w);
    int32_t left = centre - (width >> 1);
    int32_t top = bottom - pt_mulg_u16(scale, h);

    // the pixels whose centres it covers: columns [round(left),
    // round(left + width)), clamped; none, or no rows, and it is not drawn
    int32_t c0 = (left + 128) >> 8;
    int32_t c1 = (left + width + 128) >> 8;
    if (c0 < 0)
        c0 = 0;
    if (c1 > PT_SCREEN_WIDTH)
        c1 = PT_SCREEN_WIDTH;
    if (c0 >= c1 || ((bottom + 128) >> 8) <= 0 || ((top + 128) >> 8) >= PT_SCREEN_HEIGHT)
        return 0;

    RC_PANIC(r->num_insts < PT_MAX_INSTS);
    uint8_t i = r->num_insts++;
    r->insts[i] = (pt_inst) {
        .sprite = c->sprite,
        .cx0 = (uint8_t)c0,
        .cx1 = (uint8_t)c1,
        .scale = scale,
        .left = left,
        .top = top,
    };
    return i;
}

// Append the bound chain's pieces over [s0, s1) to the segment being built,
// from q, a piece at or before s0; the count of them.
static uint8_t seg_pieces(pt_render *r, uint8_t q, uint8_t s0, uint8_t s1)
{
    while (r->pool[q].x1 <= s0)
        q = r->pool[q].next;
    uint8_t n = 0;
    for (;;) {
        RC_PANIC(r->seg_num < PT_SEG_POOL);
        r->segs[r->seg_num++] = q;
        n++;
        if (r->pool[q].x1 >= s1)
            return n;
        uint8_t x1 = r->pool[q].x1;
        q = r->pool[q].next;
        RC_ASSERT(q != 0 && r->pool[q].x0 == x1);    // the chain is contiguous here
    }
}

// On entering a portal of a cell with a sprite (spec 9.3): record the
// bound pieces over the columns the portal and the sprite share, while the
// chains are whole - the visit's own edges split them and its children
// relink their pieces, but the pieces themselves never change their line.
// The instance waits on the pending stack to be issued when this visit
// pops.
static void seg_record(pt_render *r, uint8_t vi)
{
    const pt_visit *vs = &r->visits[vi];
    uint8_t ii = vs->inst;
    if (ii == 0)
        return;
    pt_inst *in = &r->insts[ii];
    uint8_t s0 = vs->x > in->cx0 ? vs->x : in->cx0;
    uint8_t s1 = vs->xr < in->cx1 ? vs->xr : in->cx1;
    if (s0 >= s1)
        return;

    RC_PANIC(r->seg_num + PT_SEG_PIECES <= PT_SEG_POOL);
    uint8_t at = (uint8_t)r->seg_num;
    r->seg_num += PT_SEG_PIECES;
    r->segs[at + PT_SEG_X0] = s0;
    r->segs[at + PT_SEG_X1] = s1;
    r->segs[at + PT_SEG_NEXT] = 0;
    r->segs[at + PT_SEG_NT] = seg_pieces(r, vs->tcur, s0, s1);
    r->segs[at + PT_SEG_NB] = seg_pieces(r, vs->bcur, s0, s1);
    if (in->seg_tail)
        r->segs[in->seg_tail + PT_SEG_NEXT] = at;
    else
        in->seg = at;
    in->seg_tail = at;

    RC_PANIC(r->num_pend < PT_MAX_PEND);
    r->pend[r->num_pend++] = ii;
}

// Issue the instances of a walk call's portals as they pop (spec 9.4),
// innermost first: each moves to the tail of the draw order, so a sprite
// comes after everything seen through its cell's visits (farther) and
// before whatever they are seen through (nearer).
static void inst_issue(pt_render *r, uint8_t pend)
{
    if (r->num_pend > r->pend_high)
        r->pend_high = r->num_pend;
    while (r->num_pend > pend) {
        uint8_t i = r->pend[--r->num_pend];
        pt_inst *in = &r->insts[i];
        if (r->order_tail == i)
            continue;
        if (in->prev || r->order_head == i) {
            // unlink
            if (in->prev)
                r->insts[in->prev].next = in->next;
            else
                r->order_head = in->next;
            r->insts[in->next].prev = in->prev;    // not the tail, so next != 0
        }
        in->prev = r->order_tail;
        in->next = 0;
        if (r->order_tail)
            r->insts[r->order_tail].next = i;
        else
            r->order_head = i;
        r->order_tail = i;
    }
}

// ---- visits ----

// The cell's visit record, made on first need: when an edge into it is
// first clipped, so that the clip can build its bounds there.
static uint8_t visit_of(pt_render *r, uint8_t cell)
{
    uint8_t vi = r->cell_visit[cell];
    if (vi == 0) {
        RC_PANIC(r->num_visits < PT_MAX_VISITS);
        vi = r->num_visits++;
        r->visits[vi] = (pt_visit) {.cell = cell};
        r->cell_visit[cell] = vi;
    }
    return vi;
}

// Move a visit's cursor to the next edge: its end vertex becomes the start,
// and that edge's lines start afresh.
static void visit_advance(pt_render *r, uint8_t vi, const pt_cell *c)
{
    pt_visit *vs = &r->visits[vi];
    vs->k = ring_k_next(c, vs->k);
    vs->v0 = vs->v1;
    vs->v1 = transform(r, ring_vertex(r->map, c, ring_k_next(c, vs->k)));
    for (uint8_t i = 0; i < PT_NUM_SLOTS; i++)
        vs->active[i] = 0;
}

// Enter visit vi over [xl, xr) through edge ei of the cell before, its
// bounds already in place: the cursor goes to the cell's first edge (spec
// 7.4), the first whose end vertex is strictly right of xl. A first walk
// searches from P (pv), the portal's start vertex, whose edge is the twin's
// ring successor, and projects the cell's sprite. A later one resumes from
// the edge its last walk ended on - every earlier edge ends left of that
// walk's xr, which is at or left of xl - with that edge's vertices and its
// lines' last pieces in hand. 0 for a degenerate ring (not entered).
static uint8_t enter(pt_render *r, uint8_t vi, uint8_t ei, uint8_t xl, uint8_t xr, pt_vv pv)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[vi];
    const pt_cell *c = &m->cells[vs->cell];
    bool first = vs->xr == 0;
    if (first) {
        vs->k = ring_k_next(c, (uint8_t)(pt_map_twin(m, ei) - c->first));
        vs->v0 = pv;
        vs->v1 = transform(r, ring_vertex(m, c, ring_k_next(c, vs->k)));
    }
    uint8_t guard = (c->num + 1);
    while (!col_gt(vs->v1, xl)) {
        if (guard-- == 0) {
            RC_ASSERT(0);
            return 0;    // degenerate ring; skip the child rather than spin
        }
        visit_advance(r, vi, c);
    }
    if (first)
        vs->inst = inst_new(r, vs->cell);
    vs->x = xl;
    vs->xr = xr;
    return vi;
}

// ---- one edge of the walk (spec 7.8) ----

// An edge's lines for one visit. A line with a piece on this edge already
// is in screen space: the piece's x0, row and g are its line, which runs on
// to the edge's end column, so it is used as is (clipped from the span's
// left column) with no projection or gradient division.
// Hidden stretches keep pieces too, uncoloured, so only a line not yet
// generated on this edge, or cut off at the screen's edge so far, is
// projected, and only then are the edge's endpoints side-clipped (the
// divisions), once for all its lines. A line pre-clipped at the screen edge
// needs no exception: carried on past that column, its rows run off the
// screen, beyond every bound, and it merges into the bound there as the
// cut-off columns would have.
typedef struct pt_edgelines {
    uint8_t vis;
    pt_vv v0, v1;
    uint8_t sx0, sx1;
    bool have_ep;
    pt_ep e0, e1;
} pt_edgelines;

static pt_line edge_line(const pt_render *r, pt_edgelines *el, uint8_t slot, pt_relh h,
                         pt_pattern col)
{
    uint8_t q = r->visits[el->vis].active[slot];
    if (q) {
        const pt_piece *pp = &r->pool[q];
        return (pt_line) {.x0 = pp->x0, .x1 = el->sx1, .y = (uint16_t)((pp->y0 << 8) | pp->yf),
                          .g = pp->g, .below = h.below, .ce = col.e, .co = col.o};
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

// One of an edge's lines that forms no bound: clipped from the bounds'
// cursors at the span's start, merges only.
static void plain_line(pt_render *r, pt_clip *cx, pt_edgelines *el, uint8_t slot, pt_relh h,
                       pt_pattern col, uint8_t xa, uint8_t xb)
{
    const pt_visit *vs = &r->visits[el->vis];
    cx->tp = vs->tcur;
    cx->bp = vs->bcur;
    cx->own = 0;
    cx->slot = slot;
    pt_line l = edge_line(r, el, slot, h, col);
    clip_line(r, cx, &l, xa, xb);
}

// Process the visit's current edge: its lines, and the child through it, if
// any (its visit, entered; 0 for none).
static uint8_t process_edge(pt_render *r, uint8_t vi)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[vi];
    const pt_cell *ca = &m->cells[vs->cell];
    uint8_t ei = (uint8_t)(ca->first + vs->k);
    const pt_half *e = &m->halves[ei];
    pt_vv v0 = vs->v0;
    pt_vv v1 = vs->v1;

    // 1. the column span, before any clipping, from the vertices' flags and
    // columns. It starts where the visit's last span ended (vs->x, the
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
        return 0;
    if ((v1.flags & PT_VF_BNEG) && !(v0.flags & PT_VF_ANEG)
        && (v0.flags & (PT_VF_BNEG | PT_VF_BZERO)))
        return 0;
    uint8_t sx0 = (v0.flags & PT_VF_ANEG) ? 0 : (v0.flags & PT_VF_BNEG) ? 128 : v0.sx;
    uint8_t sx1 = (v1.flags & PT_VF_BNEG) ? 128 : (v1.flags & PT_VF_ANEG) ? 0 : v1.sx;
    uint8_t xa = sx0 > vs->x ? sx0 : vs->x;
    uint8_t xb = sx1 < vs->xr ? sx1 : vs->xr;
    if (xa >= xb)
        return 0;
    vs->x = xb;
    vs->tcur = piece_at(r, vs->tcur, xa);    // the bounds' pieces at xa: chains
    vs->bcur = piece_at(r, vs->bcur, xa);    // only ever gain pieces, never lose them

    // 2. a soft edge is level both ways and has no lines: it hands the
    // bounds on as they are. A cell already walked over these columns can
    // only be a rounding mishap with the eye on a vertex: it is not entered
    // again.
    if (e->colour == PT_EDGE_SOFT) {
        uint8_t ci = visit_of(r, e->adj);
        pt_visit *cv = &r->visits[ci];
        if (cv->xr > xa)
            return 0;
        cv->tcur = vs->tcur;
        cv->bcur = vs->bcur;
        cv->tmax = vs->tmax;
        cv->bmin = vs->bmin;
        return enter(r, ci, ei, xa, xb, v0);
    }

    pt_edgelines el = {.vis = vi, .v0 = v0, .v1 = v1, .sx0 = sx0, .sx1 = sx1};
    pt_pattern wall = pt_shade((uint8_t)(e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_relh ha_c = rel_h(ca->ceil_z, r->eye_z);
    pt_relh ha_f = rel_h(ca->floor_z, r->eye_z);
    pt_clip cx = {.tmax = vs->tmax, .bmin = vs->bmin, .vis = vi, .shut = ST_NONE};

    if (e->adj == 0) {
        // 3. a solid wall: its ceiling line and floor line, merges only
        plain_line(r, &cx, &el, PT_SLOT_CEIL, ha_c, pt_pattern_xor(ca_ceil, wall), xa, xb);
        plain_line(r, &cx, &el, PT_SLOT_FLOOR, ha_f, pt_pattern_xor(wall, ca_floor), xa, xb);
        return 0;
    }

    const pt_cell *cb = &m->cells[e->adj];
    pt_pattern cb_ceil = pt_shade(cb->ceil);
    pt_pattern cb_floor = pt_shade(cb->floor);
    pt_pattern cc = pt_pattern_xor(ca_ceil, cb_ceil);
    pt_pattern ff = pt_pattern_xor(cb_floor, ca_floor);
    bool level_c = cb->ceil_z == ca->ceil_z;
    bool level_f = cb->floor_z == ca->floor_z;

    // 3. the lines forming no bound go first: merges only, against the
    // bounds intact. Where the two cells' ceilings (or floors) are level,
    // the line there forms no bound either: everything seen past it is
    // farther, so on the screen it's below a level ceiling's line and above
    // a level floor's, and the child just keeps the parent's bound on that
    // side. The line is then only a change of colour, and isn't even made
    // if the shades match too.
    if (cb->ceil_z > ca->ceil_z)
        plain_line(r, &cx, &el, PT_SLOT_CEIL, ha_c, pt_pattern_xor(ca_ceil, wall), xa, xb);
    else if (level_c && (cc.e | cc.o) != 0)
        plain_line(r, &cx, &el, PT_SLOT_CEIL_BOUND, ha_c, cc, xa, xb);
    if (cb->floor_z < ca->floor_z)
        plain_line(r, &cx, &el, PT_SLOT_FLOOR, ha_f, pt_pattern_xor(wall, ca_floor), xa, xb);
    else if (level_f && (ff.e | ff.o) != 0)
        plain_line(r, &cx, &el, PT_SLOT_FLOOR_BOUND, ha_f, ff, xa, xb);

    // 4. the bound-forming lines, built into the child's visit (a scratch
    // record, visit 0, for a cell already walked over these columns: the
    // eye on a vertex). The floor line goes first, building the child's
    // bottom bound against the parent's two; it closes the child where it
    // is above the top. The ceiling line then builds the top bound against
    // the parent's top and the child's bottom, over the columns the floor
    // line left open: where that closed the child, the ceiling line is above
    // the top as well, and merges with nothing evaluated. A level side has
    // no line: the child shares the parent's bound there.
    uint8_t ci = visit_of(r, e->adj);
    uint8_t bv = r->visits[ci].xr > xa ? 0 : ci;
    pt_visit *cv = &r->visits[bv];
    pt_pattern cbfb = pt_pattern_xor(cb_ceil, cb_floor);
    pt_line l;

    uint8_t ol = xa;
    uint8_t orr = xb;
    if (level_f) {
        cv->bcur = vs->bcur;
        cv->bmin = vs->bmin;
    } else {
        cv->bcur = 0;
        cv->bmin = PT_SCREEN_HEIGHT;
        pt_relh h = ha_f;
        pt_pattern col = ff;
        if (cb->floor_z < ca->floor_z) {
            h = rel_h(cb->floor_z, r->eye_z);
            col = pt_pattern_xor(cb_floor, wall);
        }
        cx = (pt_clip) {.tp = vs->tcur, .bp = vs->bcur, .tmax = vs->tmax, .bmin = vs->bmin,
                        .vis = vi, .slot = PT_SLOT_FLOOR_BOUND, .shut = ST_ABOVE, .child = bv,
                        .cbfb = cbfb};
        l = edge_line(r, &el, PT_SLOT_FLOOR_BOUND, h, col);
        clip_line(r, &cx, &l, xa, xb);
        run_end(r, &cx);
        ol = cx.ol;
        orr = cx.orr;
        if (ol >= orr)
            ol = orr = xb;
    }

    if (level_c) {
        cv->tcur = vs->tcur;
        cv->tmax = vs->tmax;
    } else {
        cv->tcur = 0;
        cv->tmax = 0;
        pt_relh h = ha_c;
        pt_pattern col = cc;
        if (cb->ceil_z > ca->ceil_z) {
            h = rel_h(cb->ceil_z, r->eye_z);
            col = pt_pattern_xor(wall, cb_ceil);
        }
        cx = (pt_clip) {.tp = vs->tcur, .bp = cv->bcur, .tmax = vs->tmax, .bmin = cv->bmin,
                        .vis = vi, .slot = PT_SLOT_CEIL_BOUND, .shut = ST_NONE, .child = bv,
                        .cbfb = cbfb};
        l = edge_line(r, &el, PT_SLOT_CEIL_BOUND, h, col);
        merge_above(r, &cx, &l, xa, ol);
        if (ol < orr) {
            cx.shut = ST_BELOW;
            clip_line(r, &cx, &l, ol, orr);
            run_end(r, &cx);
            cx.shut = ST_NONE;
        }
        merge_above(r, &cx, &l, orr, xb);
        ol = cx.ol;
        orr = cx.orr;
    }

    // 5. the child: the run of columns the two lines left open
    if (ol >= orr)
        return 0;
    if (bv == 0) {
        close_columns(r, cv->bcur, ol, orr, cbfb);
        return 0;
    }
    return enter(r, ci, ei, ol, orr, v0);
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
    for (uint8_t k = 0; k <= (c->num + 1); k++) {
        uint8_t n = (uint8_t)(c->first + (k == (c->num + 1) ? 0 : k));
        int16_t rx = (int16_t)(m->verts[m->halves[n].v0].x - r->cam_x);
        int16_t ry = (int16_t)(m->verts[m->halves[n].v0].y - r->cam_y);
        int32_t a = (int32_t)rx * k1 + (int32_t)ry * k2;
        if (a == 0)
            zero = n;
        if (k > 0 && prev < 0 && a >= 0)
            root = (uint8_t)(c->first + k - 1);
        prev = a;
    }
    if (root != 0xFF)
        return root;
    RC_ASSERT(zero != 0xFF);    // neither a transition nor the eye-vertex:
    return zero;                // broken map (falls back to the ring start)
}

// Find the camera cell's first edge (where a goes from <= 0 to > 0) and
// start its visit on the root portal, the whole screen. The searches are
// guarded: with the eye on the cell's boundary the rounded sums can read
// one-sided, and the exact retry decides instead (see root_exact). A ring
// the retry roots at a zero vertex has no closing column, so the walk ends
// by its steps - accepted, not an error.
static uint8_t root_visit(pt_render *r, uint8_t roott, uint8_t rootb)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[r->cam_cell];

    uint8_t n = c->first;
    pt_vv v = transform(r, m->verts[m->halves[n].v0]);
    pt_vv w;    // the root edge's end vertex, handed to the frame
    uint8_t guard = (c->num + 1);
    while (!(v.flags & (PT_VF_ANEG | PT_VF_AZERO))) {    // a > 0
        if (guard-- == 0)
            goto exact;
        n = pt_map_half_next(c, n);
        v = transform(r, m->verts[m->halves[n].v0]);
    }
    guard = (c->num + 1);
    for (;;) {
        uint8_t nn = pt_map_half_next(c, n);
        w = transform(r, m->verts[m->halves[nn].v0]);
        if (!(w.flags & (PT_VF_ANEG | PT_VF_AZERO)))    // a > 0
            goto rooted;
        n = nn;
        v = w;
        if (guard-- == 0)
            goto exact;
    }
exact:
    n = root_exact(r, c);
    v = transform(r, m->verts[m->halves[n].v0]);
    w = transform(r, m->verts[m->halves[pt_map_half_next(c, n)].v0]);
rooted:;

    // the camera's cell is never revisited (every ray starts in it), so its
    // visit is simply begun here, already started on the root edge
    uint8_t vi = visit_of(r, r->cam_cell);
    pt_visit *vs = &r->visits[vi];
    vs->k = (uint8_t)(n - c->first);
    vs->x = 0;
    vs->xr = 128;
    vs->tcur = roott;
    vs->bcur = rootb;
    vs->tmax = 0;
    vs->bmin = PT_SCREEN_HEIGHT;
    vs->v0 = v;
    vs->v1 = w;
    vs->inst = inst_new(r, r->cam_cell);
    return vi;
}

static void walk(pt_render *r, uint8_t vi, uint8_t depth);

// Walk a child one level deeper, or, past the depth cap, close its columns
// instead: the fix-up stands in for its whole stack, so the opening shows
// the child's ceiling colour flat - a draw-distance cutoff rather than a
// glitch.
static void descend(pt_render *r, uint8_t ci, uint8_t depth)
{
    if (depth > PT_MAX_DEPTH) {
        const pt_visit *vs = &r->visits[ci];
        const pt_cell *c = &r->map->cells[vs->cell];
        pt_pattern cbfb = pt_pattern_xor(pt_shade(c->ceil), pt_shade(c->floor));
        close_columns(r, vs->bcur, vs->x, vs->xr, cbfb);
        return;
    }
    walk(r, ci, depth);
}

// Walk a visit (spec 7.5): each edge of its cell from the one its cursor is
// on, until an edge ends at or right of xr. A child through any earlier edge
// is walked recursively, and the visit carries on from its own column when
// it returns; the child through the last edge is walked in this same call
// (a tail call: on the 6502 a JMP, no stack), so the depth grows only with
// portals nested along a line of sight. A healthy ring stops on col_ge
// before its steps run out; a ring rooted at a zero vertex (the eye on it,
// only ever the camera's cell) has no closing column, no stop fires, and
// the walk wraps through empty spans until the steps end it - an accepted
// outcome, not an error.
static void walk(pt_render *r, uint8_t vi, uint8_t depth)
{
    const pt_map *m = r->map;
    if (depth > r->depth_high)
        r->depth_high = depth;
    uint8_t pend = r->num_pend;    // this call's portals' sprites, issued as they pop

    for (;;) {
        pt_visit *vs = &r->visits[vi];
        const pt_cell *c = &m->cells[vs->cell];
        seg_record(r, vi);
        uint8_t steps = (uint8_t)((c->num + 1) + 1);
        for (;;) {
            if (steps-- == 0)
                goto done;

            // the visit holds both vertices of its current edge: each vertex
            // is transformed once, as the end of one edge, and carried on as
            // the start of the next; the last edge leaves the cursor on it
            // for a later visit to resume from
            bool last = col_ge(vs->v1, vs->xr);
            uint8_t ci = process_edge(r, vi);
            if (last) {
                if (ci == 0)
                    goto done;
                vi = ci;    // the tail call
                break;
            }
            if (ci != 0)
                descend(r, ci, (uint8_t)(depth + 1));
            visit_advance(r, vi, c);
        }
    }
done:
    // every portal of this call pops here, the tail call's innermost first
    inst_issue(r, pend);
}

// ---- plotting (spec 8.1) ----

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

// Plot a row of a byte under mask, then move the cursor a row down (up).
static void row_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t mask, pt_pattern pat)
{
    pt_mode5_eor(fb, *cur, mask, pat);
    pt_mode5_down(cur);
}

static void row_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t mask, pt_pattern pat)
{
    pt_mode5_eor(fb, *cur, mask, pat);
    pt_mode5_up(cur);
}

// One whole byte of a falling shallow line, as the 6502 does it: the three
// steps inside the byte decide, carry by carry, which of eight fixed routines
// plots it, each with its row masks as constants (the comments give the
// carries, the step after the leftmost pixel first). The fourth step then
// says whether the next byte starts a row down. (The 6502 takes that step at
// the routine's start and carries it through the plotting in C.)
static void byte_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, pt_pattern pat)
{
    if (!carry_down(f, gm)) {
        if (!carry_down(f, gm)) {
            if (!carry_down(f, gm)) {
                pt_mode5_eor(fb, *cur, 0xFF, pat);    // 000: all four on one row
            } else {
                row_down(fb, cur, 0xEE, pat);         // 001
                pt_mode5_eor(fb, *cur, 0x11, pat);
            }
        } else if (!carry_down(f, gm)) {
            row_down(fb, cur, 0xCC, pat);             // 010
            pt_mode5_eor(fb, *cur, 0x33, pat);
        } else {
            row_down(fb, cur, 0xCC, pat);             // 011
            row_down(fb, cur, 0x22, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!carry_down(f, gm)) {
        if (!carry_down(f, gm)) {
            row_down(fb, cur, 0x88, pat);             // 100
            pt_mode5_eor(fb, *cur, 0x77, pat);
        } else {
            row_down(fb, cur, 0x88, pat);             // 101
            row_down(fb, cur, 0x66, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!carry_down(f, gm)) {
        row_down(fb, cur, 0x88, pat);                 // 110
        row_down(fb, cur, 0x44, pat);
        pt_mode5_eor(fb, *cur, 0x33, pat);
    } else {
        row_down(fb, cur, 0x88, pat);                 // 111: a row per pixel
        row_down(fb, cur, 0x44, pat);
        row_down(fb, cur, 0x22, pat);
        pt_mode5_eor(fb, *cur, 0x11, pat);
    }
    if (carry_down(f, gm))
        pt_mode5_down(cur);
    pt_mode5_next(cur);
}

// And of a rising one: the same tree on the borrows, moving up.
static void byte_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, pt_pattern pat)
{
    if (!borrow_up(f, gm)) {
        if (!borrow_up(f, gm)) {
            if (!borrow_up(f, gm)) {
                pt_mode5_eor(fb, *cur, 0xFF, pat);    // 000
            } else {
                row_up(fb, cur, 0xEE, pat);           // 001
                pt_mode5_eor(fb, *cur, 0x11, pat);
            }
        } else if (!borrow_up(f, gm)) {
            row_up(fb, cur, 0xCC, pat);               // 010
            pt_mode5_eor(fb, *cur, 0x33, pat);
        } else {
            row_up(fb, cur, 0xCC, pat);               // 011
            row_up(fb, cur, 0x22, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!borrow_up(f, gm)) {
        if (!borrow_up(f, gm)) {
            row_up(fb, cur, 0x88, pat);               // 100
            pt_mode5_eor(fb, *cur, 0x77, pat);
        } else {
            row_up(fb, cur, 0x88, pat);               // 101
            row_up(fb, cur, 0x66, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!borrow_up(f, gm)) {
        row_up(fb, cur, 0x88, pat);                   // 110
        row_up(fb, cur, 0x44, pat);
        pt_mode5_eor(fb, *cur, 0x33, pat);
    } else {
        row_up(fb, cur, 0x88, pat);                   // 111
        row_up(fb, cur, 0x44, pat);
        row_up(fb, cur, 0x22, pat);
        pt_mode5_eor(fb, *cur, 0x11, pat);
    }
    if (borrow_up(f, gm))
        pt_mode5_up(cur);
    pt_mode5_next(cur);
}

// A shallow line: its row moves by at most one per column, so it is kept as
// a row byte and a fraction, and each step's carry out of the fraction says
// whether the next column is a row on. Whole bytes go four pixels at a time
// (byte_down, byte_up); the partial bytes at either end go a pixel at a
// time. A flat line has no steps: whole bytes on one row, the end bytes
// masked, with no arithmetic at all (through the tree each byte would still
// take its four steps).
static void plot_shallow(uint8_t *fb, const pt_piece *p, pt_pattern pat)
{
    uint8_t gm = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as piece_y
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
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
            byte_down(fb, &cur, &f, gm, pat);
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
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
            byte_up(fb, &cur, &f, gm, pat);
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
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as piece_y

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
// unsigned DDA. Each starts from its exact row, fraction and all, so a
// piece split off a line plots the same rows the unsplit line would have,
// and chain seams cannot notch the boundary. Nothing is bounds
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

// ---- the sprite pass (spec 9.5) ----

// The piece of a segment's bound list over column c: the last whose x0 is at
// or before it (the first always is).
static uint8_t seg_piece_at(const pt_render *r, const uint8_t *list, uint8_t n, uint8_t c)
{
    uint8_t k = 0;
    while (k + 1 < n && r->pool[list[k + 1]].x0 <= c)
        k++;
    return k;
}

// Move an instance's bound cursors to column c (columns only increase):
// false when no segment covers it (hidden there), else the clip rows, the
// portal's first visible row and the first row below it, 0..H. Within a
// segment the rows step by the pieces' gradients, one piece to the next at
// its x0; entering a segment or a piece costs one evaluation.
static bool inst_bounds(pt_render *r, pt_inst *in, uint8_t c, uint8_t *top, uint8_t *bot)
{
    while (in->cseg != 0 && r->segs[in->cseg + PT_SEG_X1] <= c) {
        in->cseg = r->segs[in->cseg + PT_SEG_NEXT];
        in->cc = 0xFF;
    }
    if (in->cseg == 0 || c < r->segs[in->cseg + PT_SEG_X0])
        return false;

    const uint8_t *sg = &r->segs[in->cseg];
    uint8_t nt = sg[PT_SEG_NT];
    uint8_t nb = sg[PT_SEG_NB];
    const uint8_t *tl = &sg[PT_SEG_PIECES];
    const uint8_t *bl = tl + nt;
    if (in->cc == 0xFF || (uint8_t)(in->cc + 1) != c) {
        in->tk = seg_piece_at(r, tl, nt, c);
        in->bk = seg_piece_at(r, bl, nb, c);
        in->ty = piece_y(r, tl[in->tk], c);
        in->by = piece_y(r, bl[in->bk], c);
    } else {
        if (in->tk + 1 < nt && r->pool[tl[in->tk + 1]].x0 == c)
            in->ty = piece_y(r, tl[++in->tk], c);
        else
            in->ty = step1(in->ty, r->pool[tl[in->tk]].g);
        if (in->bk + 1 < nb && r->pool[bl[in->bk + 1]].x0 == c)
            in->by = piece_y(r, bl[++in->bk], c);
        else
            in->by = step1(in->by, r->pool[bl[in->bk]].g);
    }
    in->cc = c;
    *top = row_of(in->ty);
    *bot = row_of(in->by);
    return true;
}

// The source column at screen column c (columns only increase): the one
// whose left edge rounds to at or before c and whose right edge after it.
static uint8_t inst_col(pt_inst *in, uint8_t c)
{
    while (((in->col_next + 128) >> 8) <= c) {
        in->col++;
        in->col_next += in->scale;
    }
    return in->col;
}

// EOR a toggle byte pair into canvas row `row` of byte column bc.
static void tog_plot(pt_render *r, uint8_t bc, uint8_t row, uint8_t e, uint8_t o)
{
    pt_mode5_eor(r->fb, pt_mode5_at(bc, row), 0xFF, (pt_pattern) {.e = e, .o = o});
}

static void tog_remove(pt_render *r, uint8_t i)
{
    r->num_togs--;
    for (uint8_t k = i; k < r->num_togs; k++)
        r->togs[k] = r->togs[k + 1];
}

// Add a toggle at a row, keeping one entry per row: merged into the entry
// at i - 1 or i if it is that row (and removed if it cancels out), else
// inserted at i; the screen gets it either way. Returns the walk position
// after the row.
static uint8_t tog_put(pt_render *r, uint8_t bc, uint8_t i, uint8_t row, uint8_t e, uint8_t o)
{
    if ((e | o) == 0)
        return i;
    tog_plot(r, bc, row, e, o);
    uint8_t k = i;
    if (i > 0 && r->togs[i - 1].row == row)
        k = (uint8_t)(i - 1);
    else if (!(i < r->num_togs && r->togs[i].row == row))
        k = 0xFF;
    if (k != 0xFF) {
        r->togs[k].e ^= e;
        r->togs[k].o ^= o;
        if ((r->togs[k].e | r->togs[k].o) == 0) {
            tog_remove(r, k);
            return k;
        }
        return (uint8_t)(k + 1);
    }
    RC_PANIC(r->num_togs < PT_MAX_TOGS);
    for (uint8_t q = r->num_togs; q > i; q--)
        r->togs[q] = r->togs[q - 1];
    r->togs[i] = (pt_tog) {.row = row, .e = e, .o = o};
    r->num_togs++;
    return (uint8_t)(i + 1);
}

// Erase this pixel's world toggles at rows up to `last` from walk position
// i on, folding them into acc (the world's value before this sprite). The
// walk position after them.
static uint8_t tog_erase(pt_render *r, uint8_t bc, uint8_t i, uint8_t last, uint8_t m,
                         pt_pattern *acc)
{
    while (i < r->num_togs && r->togs[i].row <= last) {
        pt_tog *t = &r->togs[i];
        uint8_t e = t->e & m;
        uint8_t o = t->o & m;
        if ((e | o) != 0) {
            acc->e ^= e;
            acc->o ^= o;
            tog_plot(r, bc, t->row, e, o);    // annuls the pixel's plotted toggle
            t->e &= (uint8_t)~m;
            t->o &= (uint8_t)~m;
            if ((t->e | t->o) == 0) {
                tog_remove(r, i);
                continue;
            }
        }
        i++;
    }
    return i;
}

// Paint one silhouette run of a pixel column over rows [ya, yb): colours
// from the sprite column's runs, from byte p of the art, each run's rows
// from the instance's row edges. The world's toggles the run covers are
// erased and folded into acc, and the closing toggle at yb returns the
// pixel to what lies below.
static void paint(pt_render *r, const pt_inst *in, const pt_sprite_art *a, uint8_t bc, uint8_t m,
                  const uint8_t *p, uint8_t ya, uint8_t yb)
{
    // the first colour: the last run starting at or above ya
    uint8_t code = *p & 7;
    for (p++; (*p >> 3) != 0 && in->rows[*p >> 3] <= ya; p++)
        code = *p & 7;
    pt_pattern pal = pt_shade(a->pal[code - 1]);

    // the prefix: the world's value at ya
    pt_pattern acc = {0, 0};
    uint8_t i = 0;
    while (i < r->num_togs && r->togs[i].row <= ya) {
        acc.e ^= r->togs[i].e;
        acc.o ^= r->togs[i].o;
        i++;
    }
    i = tog_put(r, bc, i, ya, (uint8_t)((acc.e ^ pal.e) & m), (uint8_t)((acc.o ^ pal.o) & m));

    // each change inside the run: the world's toggles down to it erased, the
    // colour switched
    for (; (*p >> 3) != 0; p++) {
        uint8_t y = in->rows[*p >> 3];
        if (y >= yb || (*p & 7) == 0)
            break;
        pt_pattern np = pt_shade(a->pal[(*p & 7) - 1]);
        i = tog_erase(r, bc, i, y, m, &acc);
        i = tog_put(r, bc, i, y, (uint8_t)((pal.e ^ np.e) & m), (uint8_t)((pal.o ^ np.o) & m));
        pal = np;
    }

    // the end: the world's toggles above yb erased, and the world restored
    // below (a toggle at yb itself is the world's, and stays)
    if (yb > 0)
        i = tog_erase(r, bc, i, (uint8_t)(yb - 1), m, &acc);
    if (yb < PT_SCREEN_HEIGHT)
        tog_put(r, bc, i, yb, (uint8_t)((pal.e ^ acc.e) & m), (uint8_t)((pal.o ^ acc.o) & m));
}

// The sprites, far to near, into the toggle image (spec 9.5): per byte
// column, the plotted toggles are read into a sparse list, and each
// instance's opaque runs over it are painted as toggles, clipped to the
// bounds its segments recorded.
static void sprite_pass(pt_render *r)
{
    if (r->order_head == 0)
        return;
    const pt_map *m = r->map;

    // each instance's row edges and cursors, and the byte columns covered
    uint8_t b0 = 0xFF;
    uint8_t b1 = 0;
    for (uint8_t ii = r->order_head; ii != 0; ii = r->insts[ii].next) {
        pt_inst *in = &r->insts[ii];
        const pt_sprite_art *a = &m->arts[m->sprites[in->sprite].art];
        int32_t y = in->top;
        for (uint8_t k = 0; k <= pt_sprite_height(a); k++, y += in->scale) {
            int32_t row = (y + 128) >> 8;
            in->rows[k] = (uint8_t)(row < 0 ? 0 : row > PT_SCREEN_HEIGHT ? PT_SCREEN_HEIGHT : row);
        }
        in->col = 0;
        in->col_next = in->left + in->scale;
        in->cseg = in->seg;
        in->cc = 0xFF;
        if (in->cx0 >> 2 < b0)
            b0 = in->cx0 >> 2;
        if ((in->cx1 - 1) >> 2 > b1)
            b1 = (uint8_t)((in->cx1 - 1) >> 2);
    }

    for (uint8_t bc = b0; bc <= b1; bc++) {
        uint8_t c0 = (uint8_t)(bc << 2);
        uint8_t c1 = (uint8_t)(c0 + 4);

        // how far down this byte column's sprites reach
        uint8_t ymax = 0;
        for (uint8_t ii = r->order_head; ii != 0; ii = r->insts[ii].next) {
            const pt_inst *in = &r->insts[ii];
            if (in->cx0 < c1 && in->cx1 > c0) {
                uint8_t yb = in->rows[pt_sprite_height(&m->arts[m->sprites[in->sprite].art])];
                if (yb > ymax)
                    ymax = yb;
            }
        }
        if (ymax == 0)
            continue;
        if (ymax >= PT_SCREEN_HEIGHT)
            ymax = PT_SCREEN_HEIGHT - 1;

        // the plotted toggles down to there, top to bottom
        r->num_togs = 0;
        pt_mode5_cur cur = pt_mode5_at(bc, 0);
        for (uint8_t row = 0; row <= ymax; row++, pt_mode5_down(&cur)) {
            uint8_t e = r->fb[cur.blk + cur.y];
            uint8_t o = r->fb[cur.blk + cur.y + 1];
            if ((e | o) != 0) {
                RC_PANIC(r->num_togs < PT_MAX_TOGS);
                r->togs[r->num_togs++] = (pt_tog) {.row = row, .e = e, .o = o};
            }
        }

        for (uint8_t ii = r->order_head; ii != 0; ii = r->insts[ii].next) {
            pt_inst *in = &r->insts[ii];
            const pt_sprite_art *a = &m->arts[m->sprites[in->sprite].art];
            uint8_t cs = in->cx0 > c0 ? in->cx0 : c0;
            uint8_t ce = in->cx1 < c1 ? in->cx1 : c1;
            for (uint8_t c = cs; c < ce; c++) {
                uint8_t top;
                uint8_t bot;
                if (!inst_bounds(r, in, c, &top, &bot) || top >= bot)
                    continue;
                uint8_t j = inst_col(in, c);
                if (j >= pt_sprite_width(a))
                    continue;
                uint8_t pm = pt_mode5_pixmask(c);

                // each silhouette run: consecutive opaque runs, clipped
                const uint8_t *p = &a->data[a->data[2 + j]];
                for (;;) {
                    while ((*p & 7) == 0 && ((*(p + 1) >> 3) != 0))
                        p++;    // transparent: on to the next run
                    if ((*p & 7) == 0)
                        break;    // the column ends transparent
                    const uint8_t *q = p + 1;
                    while ((*q >> 3) != 0 && (*q & 7) != 0)
                        q++;    // the silhouette ends at a transparent run or the column's end
                    uint8_t ya = in->rows[*p >> 3];
                    uint8_t yb = (*q >> 3) != 0 ? in->rows[*q >> 3] : in->rows[pt_sprite_height(a)];
                    if (ya < top)
                        ya = top;
                    if (yb > bot)
                        yb = bot;
                    if (ya < yb)
                        paint(r, in, a, bc, pm, p, ya, yb);
                    if ((*q >> 3) == 0)
                        break;
                    p = q;
                }
            }
        }
        if (r->num_togs > r->togs_high)
            r->togs_high = r->num_togs;
    }
}

void pt_render_init(pt_render *r, const pt_map *m)
{
    *r = (pt_render) {0};
    r->map = m;
    r->fb = r->fbmem + PT_FB_GUARD;
    r->sprites = true;
}

void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell)
{
    const pt_map *m = r->map;
    RC_ASSERT(cell >= 1 && cell < m->num_cells);

    // until the new capture replaces this one (phase 3): the camera's cell
    // comes from the angles of its vertices, which can put the eye a unit
    // or so past the cell's line, while this walk wants the cell that holds
    // the eye exactly
    if (!pt_map_cell_contains(m, cell, cam_x, cam_y)) {
        uint8_t exact = pt_map_find_cell(m, cam_x, cam_y);
        if (exact != 0)
            cell = exact;
    }

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
    r->num_insts = 1;
    r->order_head = 0;
    r->order_tail = 0;
    r->seg_num = 1;
    r->num_pend = 0;

    // the root portal: a plotted top bound seeding the fill with the camera
    // cell's ceiling, and a dummy off-screen bottom bound
    pt_pattern ceil = pt_shade(m->cells[cell].ceil);
    uint8_t roott = pool_alloc(r, 0, 128, row_anchor(0), 0, ceil.e, ceil.o);
    uint8_t rootb = pool_alloc(r, 0, 128, row_anchor(PT_SCREEN_HEIGHT), 0, 0, 0);
    walk(r, root_visit(r, roott, rootb), 1);

    plot(r);
    if (r->sprites)
        sprite_pass(r);
    pt_mode5_fill(r->fb);

    if (r->pool_num > r->pool_high)
        r->pool_high = r->pool_num;
    if (r->num_visits > r->visits_high)
        r->visits_high = r->num_visits;
    if (r->seg_num > r->segs_high)
        r->segs_high = r->seg_num;

    // empty the visit cache: each record clears its own cell's index
    for (uint8_t i = 1; i < r->num_visits; i++)
        r->cell_visit[r->visits[i].cell] = 0;
}
