// render.c - the capture walk, its lines clipped against the bounds a
// screen column at a time and toggled straight into the back buffer as
// they're made, and the fill. The walk finds its way by the
// angles of the cells' vertices from the eye (the vertex cache): those
// give every edge its columns, and their logs its lines' rows.
//
// A column's bounds are two rows, top[] and bot[]: the top bound's row (the
// first inside the portal) and the bottom bound's (the first below it). A
// line's toggle in a column goes at its own row clamped between them: on
// or above the top it merges into the top bound's row, on or below the
// bottom into the bottom's (the same, to the fill, as plotting it on the
// bound). A line forming one of a child's bounds writes the child's rows
// over the parent's as it goes: the walk finishes a column's whole subtree
// before moving on, and never needs the parent's rows there again. A bound
// that's the screen's edge all along a visit (its extent 0 or 64) isn't
// kept in the arrays at all.
//
// Every value is held in the type the 6502 will hold it in: pt_u8, pt_u16
// and pt_i16 check a narrowing fits, and a plain cast wraps on purpose.

#include "render.h"

#include <stdbool.h>
#include <string.h>

#include "plot.h"

// A candidate line from make_line, before clipping against the bounds: on
// the screen over [x0, x1), and beyond its edge on the side below says
// either side of that (the screen pre-clip cut it off there).
typedef struct pt_line {
    uint8_t x0;
    uint8_t x1;
    uint16_t y;       // the row at x0, u8.8 with the rounding bias in
    uint16_t g;       // sign bit + u7.8 magnitude
    uint8_t rl;       // a row at or past its last column, towards its end:
                      // with the row at x0, its rows' extremes
    uint8_t below;    // the side it is cut off on: below the screen, or above
    uint8_t ce;
    uint8_t co;
} pt_line;

// ---- row arithmetic ----
//
// Every u8.8 row (the plot DDA) carries the +128 rounding bias in its
// anchor, so rounding to the nearest row is a plain high-byte read
// (row_of): the first row whose middle is below the line. make_line keeps
// a line's own columns on the screen, rows 0 to 64 (the junk row), so its
// rows step with plain adds, the plotter's own: the clip compares exactly
// the rows that are plotted.

#define PT_ROW_MAX ((PT_SCREEN_HEIGHT << 8) + 255)    // the junk row's last 256th, u8.8

// The nearest row to a biased u8.8 value: the high byte.
static uint8_t row_of(uint16_t y)
{
    RC_ASSERT(y <= PT_ROW_MAX);
    return (uint8_t)(y >> 8);
}

// y moved along a line of gradient g by dx columns, forwards (rightwards)
// or back: false where that would leave rows 0..64. A multiply, for a span
// starting part way along a line, and the near clip's ends; the add's carry
// or the subtract's borrow says it left.
static bool row_move(uint16_t y, uint16_t g, uint8_t dx, bool fwd, uint16_t *out)
{
    // nowhere to go, or flat: nothing to multiply
    if (dx == 0 || pt_g_mag(g) == 0) {
        *out = y;
        return true;
    }
    uint16_t d = pt_mulg_u16(pt_g_mag(g), dx);
    if (fwd != (pt_g_isneg(g) != 0)) {
        uint16_t s = (uint16_t)(y + d);
        *out = s;
        return s >= y && s <= PT_ROW_MAX;
    }
    *out = (uint16_t)(y - d);
    return d <= y;
}

// The row of a line at one of its own columns.
static uint16_t line_y(const pt_line *l, uint8_t c)
{
    RC_ASSERT(c >= l->x0 && c < l->x1);
    uint16_t y = l->y;
    bool ok = row_move(l->y, l->g, pt_u8((int32_t)c - l->x0), true, &y);
    RC_ASSERT(ok);
    (void)ok;
    return y;
}

// A row one column on: the plotter's own step, a plain add (for a shallow
// line on the 6502, the fraction's add and ADC #0 into the row byte).
static uint16_t step1(uint16_t y, uint16_t g)
{
    uint16_t s = pt_g_isneg(g) ? pt_u16((int32_t)y - pt_g_mag(g)) : pt_u16((int32_t)y + pt_g_mag(g));
    RC_ASSERT(s <= PT_ROW_MAX);
    return s;
}

// ---- line generation ----

// A height as its lines see it: |z - eye_z| in 8-unit steps, the side (the
// subtract's borrow), and its log, made once a visit.
static pt_height height_of(const pt_render *r, uint8_t z)
{
    pt_height h = {.below = z >= r->eye_z};
    h.m = h.below ? pt_u8((int32_t)z - r->eye_z) : pt_u8((int32_t)r->eye_z - z);
    h.l = h.m != 0 ? pt_i16((int32_t)pt_log2(h.m) + 17 * 256) : 0;
    return h;
}

// An edge's end as its lines see it: its column, and the log of its depth
// (256 log2, in whole units).
typedef struct pt_end {
    uint8_t sx;
    int16_t ld;
} pt_end;

// 2^(l / 256), 0 under 1 and saturating at 0xFFFF.
static uint16_t exp2_sat(int16_t l)
{
    if (l < 0)
        return 0;
    if (l >= 16 * 256)
        return 0xFFFF;
    return pt_exp2((uint16_t)l);
}

// The rows a line's offsets run to: the screen's top or bottom edge, 32 rows
// from its middle, u8.8; and that offset's log, 13 * 256, where its high
// byte reaches 13 (CMP #13: the carry set means at or past the edge).
#define PT_EDGE_OFF (PT_SCREEN_HEIGHT / 2 * 256)
#define PT_EDGE_L (13 * 256)

// The edge's line at height h, from its ends: each end's row is its offset
// from the screen's middle, 64 * 8h / depth, u8.8: 2^(h.l - log depth). The
// gradient is the offsets' difference over the columns', by their logs
// (the columns' is the edge's, ldc, the same for all its lines). An end at
// or past the screen's edge, nearer than 2h, cuts the line where it
// reaches that edge, (32 rows - the far end's offset) / g columns on from
// the far end, and those columns are counted to the last one whose row is
// still on the screen; the columns cut off are beyond every bound on that
// side, and with both ends there the line has no columns at all.
static pt_line make_line(const pt_end *e0, const pt_end *e1, int16_t ldc, pt_height h,
                         pt_pattern col)
{
    RC_ASSERT(e0->sx < e1->sx);
    uint16_t mid = (uint16_t)(PT_EDGE_OFF + 128u);    // the middle row, with the rounding half
    pt_line L = {.x0 = e0->sx, .x1 = e0->sx, .rl = h.below ? PT_SCREEN_HEIGHT : 0,
                 .below = h.below, .ce = col.e, .co = col.o};
    if (h.m == 0) {
        // at the eye's height: flat, on the middle row
        L.x1 = e1->sx;
        L.y = mid;
        L.rl = row_of(mid);
        return L;
    }
    int16_t l0 = pt_i16((int32_t)h.l - e0->ld);
    int16_t l1 = pt_i16((int32_t)h.l - e1->ld);
    bool near0 = l0 >= PT_EDGE_L;
    bool near1 = l1 >= PT_EDGE_L;
    if (near0 && near1)
        return L;
    uint16_t o0 = exp2_sat(l0);
    uint16_t o1 = exp2_sat(l1);

    // the rows run away from the middle as the offset grows, down below the
    // eye and up above it: the offsets' subtract's borrow is the sign
    uint8_t dc = pt_u8((int32_t)e1->sx - e0->sx);
    bool borrow = o1 < o0;
    uint16_t dr = borrow ? pt_u16((int32_t)o0 - o1) : pt_u16((int32_t)o1 - o0);
    uint8_t neg = (uint8_t)(h.below ? borrow : !borrow && dr != 0);
    int16_t lg = 0;
    uint16_t gm = 0;
    if (dr != 0) {
        if (o0 == 0xFFFF || o1 == 0xFFFF) {
            // a near end past 256 rows: the difference in whole rows instead
            // (it's then at least 224 of them)
            uint16_t w0 = exp2_sat(pt_i16((int32_t)l0 - 8 * 256));
            uint16_t w1 = exp2_sat(pt_i16((int32_t)l1 - 8 * 256));
            uint16_t dw = w1 > w0 ? pt_u16((int32_t)w1 - w0) : pt_u16((int32_t)w0 - w1);
            lg = pt_i16((int32_t)pt_log2(dw) + 8 * 256 - ldc);
        } else {
            lg = pt_i16((int32_t)pt_log2(dr) - ldc);
        }
        gm = exp2_sat(lg);
        gm = gm > 0x7FFF ? (uint16_t)0x7FFF : gm;
    }
    L.g = pt_g_make(gm, neg);
    if (!near0 && !near1) {
        L.x1 = e1->sx;
        L.y = h.below ? pt_u16((int32_t)mid + o0) : pt_u16((int32_t)mid - o0);
        L.rl = row_of(h.below ? pt_u16((int32_t)mid + o1) : pt_u16((int32_t)mid - o1));
        return L;
    }

    // one end too near: the far end's columns towards the crossing, ceil
    // (delta) of them counting its own (CMP #1 on the low byte, ADC #0 into
    // the high), back to the last whose row is on the screen (delta is
    // logs, and can be a column over)
    uint16_t of = near0 ? o1 : o0;
    int16_t lq = pt_i16((int32_t)pt_log2((uint16_t)(PT_EDGE_OFF - of)) + 8 * 256 - lg);
    uint16_t dq = exp2_sat(lq);
    uint16_t n = (uint16_t)((dq >> 8) + ((uint8_t)dq != 0 ? 1u : 0u));
    uint8_t cnt = n > dc ? dc : (uint8_t)n;
    if (near1) {
        uint16_t y0 = h.below ? pt_u16((int32_t)mid + o0) : pt_u16((int32_t)mid - o0);
        uint16_t yl = y0;
        while (cnt != 0 && !row_move(y0, L.g, (uint8_t)(cnt - 1u), true, &yl))
            cnt--;
        L.x1 = (uint8_t)(e0->sx + cnt);
        L.y = y0;
        L.rl = row_of(yl);
    } else {
        // the end's own column isn't the line's: cnt - 1 back from it, the
        // row g (cnt - 1) nearer the screen's edge
        uint16_t y1 = h.below ? pt_u16((int32_t)mid + o1) : pt_u16((int32_t)mid - o1);
        uint8_t back = cnt == 0 ? 0 : (uint8_t)(cnt - 1u);
        uint16_t ya = y1;
        while (back != 0 && !row_move(y1, L.g, back, false, &ya))
            back--;
        if (back != 0) {
            L.x0 = (uint8_t)(e1->sx - back);
            L.x1 = e1->sx;
            L.y = ya;
            L.rl = row_of(y1);
        }
    }
    return L;
}

// ---- plotting ----

// Toggle a pixel pair in a line's colour (none, for 0).
static void toggle(pt_render *r, uint8_t c, uint8_t row, pt_pattern col)
{
    if ((col.e | col.o) != 0)
        pt_mode5_plot(r->tb, c, row, col);
}

// Record a run about to be plotted, for the unplotter to clear T from
// after the fill: a bound line's even with no colour, plotting nothing,
// since other lines' hidden toggles land on its rows (a step between two
// floors of one shade makes one). Hidden runs and the closing toggles land
// on bound rows, which are recorded runs' rows; or on row 0, which is
// cleared whole; or in the junk rows, never read. So the records cover
// every toggle.
//
// A record also brings its byte columns' last toggled rows down for the
// fill, a byte at a time. A line's rows only run one way, so a byte's
// lowest is its last column's on a falling line and its first's on a
// rising one: n - 1 steps on from the byte's first column, or none, where
// the run has n columns in it, and the next byte's first column is n steps
// on. The 6502 holds g, 2g, 3g and 4g (a shift and an add each), so a byte
// is a 16-bit add, and then, the row in A,
//   CMP lastp1,X : BCC no : ADC #0 : STA lastp1,X    ; C set: the row + 1
// about 30 cycles a byte column. The junk row gives 65, which the fill
// takes as 64.
static void record(pt_render *r, uint8_t x0, uint8_t x1, uint16_t y, uint16_t g)
{
    RC_PANIC(r->num_recs < PT_MAX_RECS);
    r->recs[r->num_recs++] = (pt_rec) {.x0 = x0, .x1 = x1, .y = y, .g = g};
    uint16_t gm = pt_g_mag(g);
    bool up = pt_g_isneg(g) != 0;
    for (uint8_t c = x0;;) {
        uint8_t n = (uint8_t)(4u - (c & 3u));
        if (n > (uint8_t)(x1 - c))
            n = (uint8_t)(x1 - c);
        uint16_t dl = pt_u16((int32_t)(n - 1u) * gm);    // to the byte's last column
        uint8_t row = row_of(up ? y : pt_u16((int32_t)y + dl));
        uint8_t bc = (uint8_t)(c >> 2);
        if (row >= r->lastp1[bc])
            r->lastp1[bc] = pt_u8((int32_t)row + 1);
        c = (uint8_t)(c + n);
        if (c == x1)
            break;
        uint16_t d = pt_u16((int32_t)n * gm);    // to the next byte's first
        y = up ? pt_u16((int32_t)y - d) : pt_u16((int32_t)y + d);
    }
}

// Plot columns [x0, x1) of a line, wholly visible there, from its row y at
// x0: a byte at a time where it's shallow, a pixel pair a column where it's
// steep.
static void plot_run(pt_render *r, const pt_line *L, uint8_t x0, uint8_t x1, uint16_t y)
{
    record(r, x0, x1, y, L->g);
    pt_pattern pat = {.e = L->ce, .o = L->co};
    if ((pat.e | pat.o) == 0)
        return;
    pt_run p = {.x0 = x0, .x1 = x1, .y0 = (uint8_t)(y >> 8), .yf = (uint8_t)y, .g = L->g};
    if (pt_g_mag(L->g) < 0x100)
        pt_plot_shallow(r->tb, &p, pat);
    else
        pt_plot_steep(r->tb, &p, pat);
}


// ---- the scan ----

// Is a row strictly inside the bounds' extents, below every top and above
// every bottom?
static bool inside(uint8_t row, uint8_t tmax, uint8_t bmin)
{
    return row > tmax && row < bmin;
}

// Is the line inside the bounds' extents all along [xa, xb), every column
// one of its own? Its rows there lie between its row at xa and rl, so two
// compares settle it, and no multiply but the one to start part way along
// it. *y is its row at xa.
static bool fast_inside(const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax, uint8_t bmin,
                        uint16_t *y)
{
    if (xa < L->x0 || xb > L->x1)
        return false;
    *y = line_y(L, xa);
    return inside(row_of(*y), tmax, bmin) && inside(L->rl, tmax, bmin);
}

// The visible stretch a scan is gathering, plotted whole when it ends.
typedef struct pt_vrun {
    uint8_t x0;    // its first column, 0xFF for none
    uint16_t y0;   // the line's row there
} pt_vrun;

static void run_add(pt_vrun *v, uint8_t c, uint16_t y)
{
    if (v->x0 == 0xFF) {
        v->x0 = c;
        v->y0 = y;
    }
}

static void run_end(pt_render *r, const pt_line *L, pt_vrun *v, uint8_t x1)
{
    if (v->x0 != 0xFF)
        plot_run(r, L, v->x0, x1, v->y0);
    v->x0 = 0xFF;
}

// The hidden stretch a scan is gathering: toggles on one row in one
// colour, plotted as a flat run (a byte at a time, the ends masked) when
// the row or colour changes. Nearly all hidden columns are a line running
// off the screen against the root's bounds: off the top they're all on
// row 0, and off the bottom they're in the junk row, where nothing is
// ever seen and the run is dropped.
typedef struct pt_hrun {
    uint8_t x0;     // its first column, 0xFF for none
    uint8_t row;
    pt_pattern pat;
} pt_hrun;

static void hrun_end(pt_render *r, pt_hrun *h, uint8_t x1)
{
    if (h->x0 != 0xFF && h->row < PT_SCREEN_HEIGHT && (h->pat.e | h->pat.o) != 0)
        pt_plot_flat(r->tb, h->x0, x1, h->row, h->pat);
    h->x0 = 0xFF;
}

static void hrun_add(pt_render *r, pt_hrun *h, uint8_t c, uint8_t row, pt_pattern pat)
{
    if (h->x0 != 0xFF && (h->row != row || h->pat.e != pat.e || h->pat.o != pat.o))
        hrun_end(r, h, c);
    if (h->x0 == 0xFF)
        *h = (pt_hrun) {.x0 = c, .row = row, .pat = pat};
}

// A flat run of toggles on row 0, over [x0, x1), if any.
static void flat_top(pt_render *r, uint8_t x0, uint8_t x1, pt_pattern pat)
{
    if (x0 < x1 && (pat.e | pat.o) != 0)
        pt_plot_flat(r->tb, x0, x1, 0, pat);
}

// The part of [xa, xb) the screen's pre-clip left a line, [*l, *rr): empty
// (both xb) when it cut off the lot.
static void on_screen(const pt_line *L, uint8_t xa, uint8_t xb, uint8_t *l, uint8_t *rr)
{
    *l = L->x0 > xa ? L->x0 : xa;
    *rr = L->x1 < xb ? L->x1 : xb;
    if (*l >= *rr)
        *l = *rr = xb;
}

// The child's open columns over an edge's span: all of them, until a bound
// line closes one, and only then kept a column at a time in open[] (filled
// first). Every column open is the common case, and needs no open[] at all.
typedef struct pt_open {
    bool all;
    uint8_t xa, xb;
} pt_open;

static void close_col(pt_render *r, pt_open *op, uint8_t c)
{
    if (op->all) {
        memset(&r->open[op->xa], 1, (size_t)(op->xb - op->xa));
        op->all = false;
    }
    r->open[c] = 0;
}

static bool is_open(const pt_render *r, const pt_open *op, uint8_t c)
{
    return op->all || r->open[c] != 0;
}

// One line's scan: the line, the runs it's gathering, and its bounds.
// Where a visit's extent says a bound is the screen's edge all along (tmax
// 0, bmin 64), its rows are that, and the array isn't read.
typedef struct pt_scan {
    pt_render *r;
    const pt_line *L;
    pt_pattern col;     // the line's colour
    pt_pattern shut;    // and with the child's stack telescoped in, closing it
    bool tz;            // the top is row 0 all along
    bool bz;            // the bottom is the junk row all along
    pt_open *op;        // a bound line's child's open columns
    uint8_t ext;        // the child's bound's extent so far: the floor line's
                        // highest row, the ceiling line's lowest
    pt_vrun v;
    pt_hrun h;
} pt_scan;

static uint8_t top_at(const pt_scan *s, uint8_t c)
{
    return s->tz ? 0 : s->r->top[c];
}

static uint8_t bot_at(const pt_scan *s, uint8_t c)
{
    return s->bz ? (uint8_t)PT_SCREEN_HEIGHT : s->r->bot[c];
}

typedef void (*pt_colfn)(pt_scan *s, uint8_t c, uint16_t y);

// Scan [xa, xb) a column at a time. Before the line's own columns and after
// them the row is beyond the screen on its side, the same all along, and
// between them the line's own rows step as the plotter steps them: three
// loops on the 6502, none comparing the column with the line's.
static void scan(pt_scan *s, uint8_t xa, uint8_t xb, pt_colfn col)
{
    const pt_line *L = s->L;
    uint8_t l, rr;
    on_screen(L, xa, xb, &l, &rr);
    uint16_t beyond = L->below ? (uint16_t)(PT_SCREEN_HEIGHT << 8) : 0;
    for (uint8_t c = xa; c < l; c++)
        col(s, c, beyond);
    if (l < rr) {
        uint16_t y = line_y(L, l);
        for (uint8_t c = l;;) {
            col(s, c, y);
            if (++c == rr)
                break;
            y = step1(y, L->g);
        }
    }
    for (uint8_t c = rr; c < xb; c++)
        col(s, c, beyond);
}

// ---- the clips ----

// A line forming no bound: visible strictly between the bounds, else
// toggled at the one it's on or past.
static void plain_col(pt_scan *s, uint8_t c, uint16_t y)
{
    uint8_t row = row_of(y);
    uint8_t t = top_at(s, c);
    uint8_t b = bot_at(s, c);
    if (row > t && row < b) {
        hrun_end(s->r, &s->h, c);
        run_add(&s->v, c, y);
    } else {
        run_end(s->r, s->L, &s->v, c);
        hrun_add(s->r, &s->h, c, row <= t ? t : b, s->col);
    }
}

// A line forming no bound, over [xa, xb). The columns the screen's pre-clip
// cut off come first: where the bound on the line's side is the screen's
// edge all the way (the root's, and any visit that kept it), they're all
// hidden on row 0, a flat run, or all in the junk row, nothing. Wholly
// inside the bounds' extents, the rest goes to the plotters whole.
// Otherwise a scan, a step and two compares a column, gathers its visible
// stretches for the plotters, and its hidden ones, toggled at their bound's
// row, for the flat plotter.
static void clip_plain(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                       uint8_t bmin)
{
    pt_pattern col = {.e = L->ce, .o = L->co};
    if ((col.e | col.o) == 0)
        return;    // a line forming no bound only toggles
    pt_scan s = {.r = r, .L = L, .col = col, .tz = tmax == 0, .bz = bmin == PT_SCREEN_HEIGHT,
                 .v = {.x0 = 0xFF}, .h = {.x0 = 0xFF}};
    if (L->below ? s.bz : s.tz) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        if (!L->below) {
            flat_top(r, xa, l, col);
            flat_top(r, rr, xb, col);
        }
        if (l == rr)
            return;
        xa = l;
        xb = rr;
    }
    uint16_t y;
    if (fast_inside(L, xa, xb, tmax, bmin, &y)) {
        plot_run(r, L, xa, xb, y);
        return;
    }
    scan(&s, xa, xb, plain_col);
    run_end(r, L, &s.v, xb);
    hrun_end(r, &s.h, xb);
}

// The child's bottom: it closes the child where it's on or above the top
// (the child's stack, telescoped to its ceiling ^ floor, cbfb, goes in with
// it), lends the parent's bottom where it's on or below that, and is the
// bottom where it's between. Either way the child's bottom row is written:
// the parent's may be the junk row it never kept.
static void floor_col(pt_scan *s, uint8_t c, uint16_t y)
{
    pt_render *r = s->r;
    uint8_t row = row_of(y);
    uint8_t t = top_at(s, c);
    uint8_t b = bot_at(s, c);
    if (row <= t) {
        run_end(r, s->L, &s->v, c);
        hrun_add(r, &s->h, c, t, s->shut);
        close_col(r, s->op, c);
        return;
    }
    if (row >= b) {
        run_end(r, s->L, &s->v, c);
        hrun_add(r, &s->h, c, b, s->col);
        row = b;
    } else {
        hrun_end(r, &s->h, c);
        run_add(&s->v, c, y);
    }
    r->bot[c] = row;
    s->ext = row < s->ext ? row : s->ext;
}

static void floor_fast_col(pt_scan *s, uint8_t c, uint16_t y)
{
    s->r->bot[c] = row_of(y);
}

// The line forming the child's bottom, over [xa, xb) (floor_col). Writes
// the child's bottom rows and closes columns in op; the child's bottom's
// extent, its highest row over the columns left open, is the result. The
// columns the screen's pre-clip cut off come first, as for clip_plain:
// against the screen's edge, off the bottom they lend the junk row and stay
// open, and off the top they close the child, on row 0. On the fast path
// the extent is the line's ends'.
static uint8_t clip_floor(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                          uint8_t bmin, pt_pattern cbfb, pt_open *op)
{
    pt_pattern col = {.e = L->ce, .o = L->co};
    pt_scan s = {.r = r, .L = L, .col = col, .shut = pt_pattern_xor(col, cbfb), .tz = tmax == 0,
                 .bz = bmin == PT_SCREEN_HEIGHT, .op = op, .ext = PT_SCREEN_HEIGHT,
                 .v = {.x0 = 0xFF}, .h = {.x0 = 0xFF}};
    if (L->below ? s.bz : s.tz) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        if (L->below) {
            memset(&r->bot[xa], PT_SCREEN_HEIGHT, (size_t)(l - xa));
            memset(&r->bot[rr], PT_SCREEN_HEIGHT, (size_t)(xb - rr));
        } else {
            flat_top(r, xa, l, s.shut);
            flat_top(r, rr, xb, s.shut);
            for (uint8_t c = xa; c < l; c++)
                close_col(r, op, c);
            for (uint8_t c = rr; c < xb; c++)
                close_col(r, op, c);
        }
        if (l == rr)
            return s.ext;
        xa = l;
        xb = rr;
    }
    uint16_t y;
    if (fast_inside(L, xa, xb, tmax, bmin, &y)) {
        scan(&s, xa, xb, floor_fast_col);
        plot_run(r, L, xa, xb, y);
        uint8_t lo = row_of(y) < L->rl ? row_of(y) : L->rl;
        return lo < s.ext ? lo : s.ext;
    }
    scan(&s, xa, xb, floor_col);
    run_end(r, L, &s.v, xb);
    hrun_end(r, &s.h, xb);
    return s.ext;
}

// The child's top, after the floor line: it lends the parent's top where
// it's on or above that, closes the child where it's on or below the
// child's bottom, and is the top between, the child's top row written
// either way. Where the floor line closed the child, this line is above the
// top as well, and merges into it with nothing worked out.
static void ceil_col(pt_scan *s, uint8_t c, uint16_t y)
{
    pt_render *r = s->r;
    uint8_t t = top_at(s, c);
    if (!is_open(r, s->op, c)) {
        run_end(r, s->L, &s->v, c);
        hrun_add(r, &s->h, c, t, s->col);
        return;
    }
    uint8_t row = row_of(y);
    if (row <= t) {
        run_end(r, s->L, &s->v, c);
        hrun_add(r, &s->h, c, t, s->col);
        row = t;
    } else if (row >= bot_at(s, c)) {
        run_end(r, s->L, &s->v, c);
        hrun_add(r, &s->h, c, bot_at(s, c), s->shut);
        close_col(r, s->op, c);
        return;
    } else {
        hrun_end(r, &s->h, c);
        run_add(&s->v, c, y);
    }
    r->top[c] = row;
    s->ext = row > s->ext ? row : s->ext;
}

static void ceil_fast_col(pt_scan *s, uint8_t c, uint16_t y)
{
    s->r->top[c] = row_of(y);
}

// The line forming the child's top, over [xa, xb) (ceil_col), against the
// parent's top (tmax its extent) and the child's bottom (cbmin). Writes the
// child's top rows and closes columns in op; the child's top's extent, its
// lowest row over the columns left open, is the result. The columns the
// screen's pre-clip cut off the top come first, as for clip_plain: against
// the screen's top, open or closed, they merge into row 0, the child's top
// there. The fast path wants no column closed yet.
static uint8_t clip_ceil(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                         uint8_t cbmin, pt_pattern cbfb, pt_open *op)
{
    pt_pattern col = {.e = L->ce, .o = L->co};
    pt_scan s = {.r = r, .L = L, .col = col, .shut = pt_pattern_xor(col, cbfb), .tz = tmax == 0,
                 .bz = cbmin == PT_SCREEN_HEIGHT, .op = op, .ext = 0,
                 .v = {.x0 = 0xFF}, .h = {.x0 = 0xFF}};
    if (!L->below && s.tz) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        flat_top(r, xa, l, col);
        flat_top(r, rr, xb, col);
        memset(&r->top[xa], 0, (size_t)(l - xa));
        memset(&r->top[rr], 0, (size_t)(xb - rr));
        if (l == rr)
            return s.ext;
        xa = l;
        xb = rr;
    }
    uint16_t y;
    if (op->all && fast_inside(L, xa, xb, tmax, cbmin, &y)) {
        scan(&s, xa, xb, ceil_fast_col);
        plot_run(r, L, xa, xb, y);
        uint8_t hi = row_of(y) > L->rl ? row_of(y) : L->rl;
        return hi > s.ext ? hi : s.ext;
    }
    scan(&s, xa, xb, ceil_col);
    run_end(r, L, &s.v, xb);
    hrun_end(r, &s.h, xb);
    return s.ext;
}

// Close columns [l, rr) of a child after the fact: its whole stack there
// telescopes to its ceiling ^ floor, toggled at its bottom's row, as the
// closing line's own toggle does. A bottom that's the junk row all along
// (bz) is nothing to toggle.
static void close_columns(pt_render *r, uint8_t l, uint8_t rr, pt_pattern cbfb, bool bz)
{
    if (bz)
        return;
    for (uint8_t c = l; c < rr; c++)
        toggle(r, c, r->bot[c], cbfb);
}

// The child: the first run of open columns, [*ol, *orr), empty when none.
// Every portal opening is convex on the screen, so there should be one run;
// a crossing's rounding could pinch a sliver-thin opening shut in the
// middle, and any later run is closed after the fact. With nothing closed,
// it's the edge's whole span, with no scan.
static void open_run(pt_render *r, const pt_open *op, pt_pattern cbfb, bool bz, uint8_t *ol,
                     uint8_t *orr)
{
    if (op->all) {
        *ol = op->xa;
        *orr = op->xb;
        return;
    }
    uint8_t c = op->xa;
    while (c < op->xb && !r->open[c])
        c++;
    *ol = c;
    while (c < op->xb && r->open[c])
        c++;
    *orr = c;
    for (; c < op->xb; c++)
        if (r->open[c])
            close_columns(r, c, (uint8_t)(c + 1u), cbfb, bz);
}

// ---- angles and columns ----
//
// A visit's cell is read in a window, a whole turn from lo: a vertex's
// place in it is t = (angle - lo) & 1023, one 10-bit subtract. The window
// holds the cell in one piece: a cell seen through a portal spans under a
// half turn, the portal's middle among it, and its window is centred
// there; the eye's own cell spans the whole turn, and its window starts
// where the walk does (root_visit). So clockwise along the sides facing
// the eye the places only rise, even round the back of the view. The view
// is the 256 from tl: left of it is column 0, right of it 128, and in it
// the view angle is the byte t + lo8.

// Vertex v's place in visit vs's window.
static uint16_t win_t(const pt_render *r, const pt_visit *vs, uint8_t v)
{
    uint16_t a = pt_vcache_vert(r->vc, r->map, v)->angle;
    return (uint16_t)((uint16_t)(a - vs->lo) & 1023u);
}

// The view angle of an in-view place: a byte.
static int8_t view8(const pt_visit *vs, uint16_t t)
{
    return (int8_t)(uint8_t)((uint8_t)t + vs->lo8);
}

// The column of a place: in view, where its step's middle projects; left
// of the view, 0; right of it, 128.
static uint8_t win_col(const pt_visit *vs, uint16_t t)
{
    if (t < vs->tl)
        return 0;
    if (t >= (uint16_t)(vs->tl + 256u))
        return PT_SCREEN_WIDTH;
    return pt_view_col(view8(vs, t));
}

// The current side's end. A side facing the eye turns clockwise by up to
// a half turn, and its end is its start's place plus that: in a child's
// window that's where the end is anyway, and in the eye's own cell, whose
// window is a whole turn, the last side runs on round to the first one's
// start, a turn up. One facing away (a cell seen through a portal has a
// chain of them, the portal among them) turns back, its end at or left of
// its start in the window, and spans nothing.
static void side_end(const pt_render *r, pt_visit *vs)
{
    const pt_cell *c = &r->map->cells[vs->cell];
    uint16_t t = win_t(r, vs, pt_map_side_v1(r->map, c, vs->k));
    uint16_t d = (uint16_t)((uint16_t)(t - vs->t0) & 1023u);
    vs->t1 = d <= 512 ? pt_u16((int32_t)vs->t0 + d) : t;
    vs->c1 = win_col(vs, vs->t1);
}

// Begin on side k: its start's place and column, and its end's.
static void side_start(const pt_render *r, pt_visit *vs)
{
    const pt_cell *c = &r->map->cells[vs->cell];
    vs->t0 = win_t(r, vs, pt_map_side(r->map, c, vs->k)->v);
    vs->c0 = win_col(vs, vs->t0);
    side_end(r, vs);
}

// On to the next side clockwise: its start is this one's end, so a side
// costs one vertex's place and column.
static void side_next(const pt_render *r, pt_visit *vs)
{
    const pt_cell *c = &r->map->cells[vs->cell];
    vs->k = pt_map_ring_next(c, vs->k);
    uint16_t t = (uint16_t)(vs->t1 & 1023u);
    vs->c0 = t == vs->t1 ? vs->c1 : win_col(vs, t);
    vs->t0 = t;
    side_end(r, vs);
}

// ---- visits ----

// The cell's visit record, made on first need, with its heights' logs.
static uint8_t visit_of(pt_render *r, uint8_t cell)
{
    uint8_t vi = r->cell_visit[cell];
    if (vi == 0) {
        RC_PANIC(r->num_visits < PT_MAX_VISITS);
        vi = r->num_visits++;
        const pt_cell *c = &r->map->cells[cell];
        r->visits[vi] = (pt_visit) {.cell = cell,
                                    .hc = height_of(r, c->ceil_z),
                                    .hf = height_of(r, c->floor_z)};
        r->cell_visit[cell] = vi;
    }
    return vi;
}

// Enter visit vi over [xl, xr), its bounds already written: the cursor goes
// to the first edge whose end is strictly right of xl. A first walk looks
// from the edge after the twin (the side the walk came in by, at ring
// position twin), in a window centred on the middle of the portal (from's
// current side); a later one resumes from the edge its last walk ended on
// - every earlier edge ends left of that walk's xr, which is at or left of
// xl. 0 for a degenerate ring (not entered).
static uint8_t enter(pt_render *r, uint8_t vi, uint8_t twin, const pt_visit *from, uint8_t xl,
                     uint8_t xr, uint8_t tmax, uint8_t bmin)
{
    pt_visit *vs = &r->visits[vi];
    const pt_cell *c = &r->map->cells[vs->cell];
    if (vs->xr == 0) {
        uint16_t mid = (uint16_t)(pt_u16((int32_t)from->t0 + from->t1) >> 1);
        vs->lo = (uint16_t)((uint16_t)(from->lo + mid - 512u) & 1023u);
        vs->tl = pt_u16((int32_t)from->tl - mid + 512);
        RC_ASSERT(vs->tl <= 768);
        vs->lo8 = (uint8_t)(from->lo8 + (uint8_t)mid);
        vs->k = pt_map_ring_next(c, twin);
        side_start(r, vs);
    }
    uint8_t guard = c->num;
    while (vs->c1 <= xl) {
        if (guard-- == 0)
            return 0;    // degenerate ring: skip the child rather than spin
        side_next(r, vs);
    }
    vs->x = xl;
    vs->xr = xr;
    vs->tmax = tmax;
    vs->bmin = bmin;
    return vi;
}

// ---- one edge of the walk ----

// An edge's lines are made from its ends, worked out once for all of them.
// An end in the view is its vertex: its column by angle, and its depth's
// log, the length's less lsec of its view angle. An end out of it, in
// front or behind, is where the edge crosses the view's edge, column 0 or
// 128, by Doom's clipper: the eye's distance from the edge's line, d = r
// |sin(a - w)| from an end in view (a its angle, w the edge's direction),
// over |sin| of the edge ray's angle to the line, is the distance along the
// ray, and cos 45 degrees (-128 in the log) makes it a depth. All logs: log
// sine lookups and some adds. The columns' log is the edge's too.
typedef struct pt_edgelines {
    const pt_visit *vs;    // the visit: its window, and the side's ends' places
    const pt_side *sd;
    uint8_t v1;            // the end's vertex (the start's is the side's)
    bool have;
    pt_end e0, e1;
    int16_t ldc;           // 256 log2 of the columns between them
} pt_edgelines;

static void edge_ends(const pt_render *r, pt_edgelines *el)
{
    const pt_map *m = r->map;
    const pt_visit *vs = el->vs;
    const pt_polar *p0 = pt_vcache_vert(r->vc, m, el->sd->v);
    const pt_polar *p1 = pt_vcache_vert(r->vc, m, el->v1);
    bool in0 = vs->t0 >= vs->tl;
    bool in1 = vs->t1 < (uint16_t)(vs->tl + 256u);
    if (in0)
        el->e0 = (pt_end) {.sx = vs->c0, .ld = pt_i16((int32_t)p0->lr - pt_lsec(view8(vs, vs->t0)))};
    if (in1)
        el->e1 = (pt_end) {.sx = vs->c1, .ld = pt_i16((int32_t)p1->lr - pt_lsec(view8(vs, vs->t1)))};
    if (!in0 || !in1) {
        // the edge's direction, its quadrant stored with the side
        uint16_t w = pt_map_side_angle(m, el->sd);
        const pt_polar *pk = in1 ? p1 : p0;
        uint16_t aw = (uint16_t)(pk->angle - w);
        int16_t ls = pt_i16(((int32_t)pt_logsin((uint16_t)(aw - 1u)) + pt_logsin(aw)) >> 1);
        int16_t ld = pt_i16((int32_t)pk->lr + ls - 128);
        if (!in0)
            el->e0 = (pt_end) {.sx = 0,
                               .ld = pt_i16((int32_t)ld - pt_logsin((uint16_t)(r->cam4 - 129u - w)))};
        if (!in1)
            el->e1 = (pt_end) {.sx = PT_SCREEN_WIDTH,
                               .ld = pt_i16((int32_t)ld - pt_logsin((uint16_t)(r->cam4 + 127u - w)))};
    }
    el->ldc = el->e0.sx < el->e1.sx ? pt_i16(pt_log2(pt_u8((int32_t)el->e1.sx - el->e0.sx))) : 0;
    el->have = true;
}

static pt_line edge_line(const pt_render *r, pt_edgelines *el, pt_height h, pt_pattern col)
{
    if (!el->have)
        edge_ends(r, el);
    if (el->e0.sx >= el->e1.sx)    // edge-on
        return (pt_line) {.x0 = el->e0.sx, .x1 = el->e0.sx,
                          .rl = h.below ? PT_SCREEN_HEIGHT : 0, .below = h.below,
                          .ce = col.e, .co = col.o};
    return make_line(&el->e0, &el->e1, el->ldc, h, col);
}

// Process the visit's current edge: its lines, and the child through it, if
// any (its visit, entered; 0 for none).
static uint8_t process_edge(pt_render *r, uint8_t vi)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[vi];
    const pt_cell *ca = &m->cells[vs->cell];
    const pt_side *sd = pt_map_side(m, ca, vs->k);

    // 1. the column span: from where the visit's last span ended (the
    // portal's left column for its first edge), or the edge's start if
    // that's further right, to its end, within the portal
    uint8_t xa = vs->c0 > vs->x ? vs->c0 : vs->x;
    uint8_t xb = vs->c1 < vs->xr ? vs->c1 : vs->xr;
    if (xa >= xb)
        return 0;
    vs->x = xb;

    // 2. a soft edge: no lines, and the child keeps both bounds. A cell
    // already walked over these columns (the eye on a vertex) isn't entered
    // again
    const pt_edge *e = &m->edges[sd->edge];
    if (e->colour == PT_EDGE_SOFT) {
        uint8_t ci = visit_of(r, sd->adj);
        if (r->visits[ci].xr > xa)
            return 0;
        return enter(r, ci, pt_side_twin(sd), vs, xa, xb, vs->tmax, vs->bmin);
    }

    pt_edgelines el = {.vs = vs, .sd = sd, .v1 = pt_map_side_v1(m, ca, vs->k)};
    pt_pattern wall = pt_shade(pt_u8((int32_t)e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_line l;

    if (sd->adj == 0) {
        // 3. a solid wall: its ceiling line and floor line, toggles only
        l = edge_line(r, &el, vs->hc, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
        l = edge_line(r, &el, vs->hf, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
        return 0;
    }

    uint8_t ci = visit_of(r, sd->adj);
    const pt_visit *vb = &r->visits[ci];
    const pt_cell *cb = &m->cells[sd->adj];
    pt_pattern cb_ceil = pt_shade(cb->ceil);
    pt_pattern cb_floor = pt_shade(cb->floor);
    pt_pattern cc = pt_pattern_xor(ca_ceil, cb_ceil);
    pt_pattern ff = pt_pattern_xor(cb_floor, ca_floor);
    bool level_c = cb->ceil_z == ca->ceil_z;
    bool level_f = cb->floor_z == ca->floor_z;

    // 3. the lines forming no bound go first, against the bounds intact.
    // Where the two cells' ceilings (or floors) are level, the line there
    // forms no bound either: everything seen past it is farther, so on the
    // screen it's below a level ceiling's line and above a level floor's,
    // and the child just keeps the parent's bound on that side. The line is
    // then only a change of colour, and isn't even made if the shades match
    if (cb->ceil_z > ca->ceil_z) {
        l = edge_line(r, &el, vs->hc, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    } else if (level_c && (cc.e | cc.o) != 0) {
        l = edge_line(r, &el, vs->hc, cc);
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    }
    if (cb->floor_z < ca->floor_z) {
        l = edge_line(r, &el, vs->hf, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    } else if (level_f && (ff.e | ff.o) != 0) {
        l = edge_line(r, &el, vs->hf, ff);
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    }

    // 4. the bound-forming lines write the child's bounds over the
    // parent's. The floor line goes first, building the child's bottom
    // against the parent's two; it closes the child where it is above the
    // top. The ceiling line then builds the top against the parent's top and
    // the child's bottom; where the floor line closed the child, it merges
    // into the top with nothing worked out. A level side has no line: the
    // child keeps the parent's bound there, and its extent. Each line gives
    // the child its bound's extent
    pt_pattern cbfb = pt_pattern_xor(cb_ceil, cb_floor);
    pt_open op = {.all = true, .xa = xa, .xb = xb};
    uint8_t cbmin = vs->bmin;
    if (!level_f) {
        pt_height h = vs->hf;
        pt_pattern col = ff;
        if (cb->floor_z < ca->floor_z) {
            h = vb->hf;
            col = pt_pattern_xor(cb_floor, wall);
        }
        l = edge_line(r, &el, h, col);
        cbmin = clip_floor(r, &l, xa, xb, vs->tmax, vs->bmin, cbfb, &op);
    }
    uint8_t ctmax = vs->tmax;
    if (!level_c) {
        pt_height h = vs->hc;
        pt_pattern col = cc;
        if (cb->ceil_z > ca->ceil_z) {
            h = vb->hc;
            col = pt_pattern_xor(wall, cb_ceil);
        }
        l = edge_line(r, &el, h, col);
        ctmax = clip_ceil(r, &l, xa, xb, vs->tmax, cbmin, cbfb, &op);
    }

    // 5. the child: the run of columns the two lines left open. A cell
    // already walked over them (the eye on a vertex) isn't entered again,
    // and its run is closed instead
    bool bz = cbmin == PT_SCREEN_HEIGHT;
    uint8_t ol, orr;
    open_run(r, &op, cbfb, bz, &ol, &orr);
    if (ol >= orr)
        return 0;
    if (r->visits[ci].xr > ol) {
        close_columns(r, ol, orr, cbfb, bz);
        return 0;
    }
    return enter(r, ci, pt_side_twin(sd), vs, ol, orr, ctmax, cbmin);
}

// ---- the walk ----

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
        close_columns(r, vs->x, vs->xr, pt_pattern_xor(pt_shade(c->ceil), pt_shade(c->floor)),
                      vs->bmin == PT_SCREEN_HEIGHT);
        return;
    }
    walk(r, ci, depth);
}

// Walk a visit: each edge of its cell from the one its cursor is on, until
// an edge ends at or right of xr. A child through any earlier edge is
// walked recursively, and the visit carries on from its own column when it
// returns; the child through the last edge is walked in this same call (a
// tail call: on the 6502 a JMP, no stack), so the depth grows only with
// portals nested along a line of sight. The steps end a degenerate ring.
static void walk(pt_render *r, uint8_t vi, uint8_t depth)
{
    if (depth > r->depth_high)
        r->depth_high = depth;
    for (;;) {
        pt_visit *vs = &r->visits[vi];
        const pt_cell *c = &r->map->cells[vs->cell];
        uint8_t steps = pt_u8((int32_t)c->num + 2);
        for (;;) {
            if (steps-- == 0)
                return;
            bool last = vs->c1 >= vs->xr;
            uint8_t ci = process_edge(r, vi);
            if (last) {
                if (ci == 0)
                    return;
                vi = ci;    // the tail call
                break;
            }
            if (ci != 0)
                descend(r, ci, pt_u8((int32_t)depth + 1));
            side_next(r, vs);
        }
    }
}

// ---- the unplotter ----

// After the fill, clear T from the records: each run stepped again as the
// plotter stepped it, and every byte it toggled zeroed whole, both
// scanlines, with no read and no mask: a shallow run a byte at a time, a
// steep one a row a column (plot.c). Then row 0, whole, which lines running
// off the top toggle in flat runs that aren't recorded: a flat run's
// unplotting, 32 bytes. Clearing T instead, even just above each column's
// last toggle, would cost more in all but a handful of frames (NOTES.md).
static void unplot(pt_render *r)
{
    for (uint8_t i = 0; i < r->num_recs; i++) {
        const pt_rec *q = &r->recs[i];
        pt_run p = {.x0 = q->x0, .x1 = q->x1, .y0 = (uint8_t)(q->y >> 8), .yf = (uint8_t)q->y,
                    .g = q->g};
        if (pt_g_mag(q->g) < 0x100)
            pt_unplot_shallow(r->tb, &p);
        else
            pt_unplot_steep(r->tb, &p);
    }
    pt_run row0 = {.x0 = 0, .x1 = PT_SCREEN_WIDTH, .y0 = 0, .yf = 128, .g = 0};
    pt_unplot_shallow(r->tb, &row0);
#ifndef NDEBUG
    for (uint16_t i = 0; i < PT_FB_BYTES; i++)
        RC_ASSERT(r->tb[i] == 0);    // the records covered every toggle
#endif
}

// ---- the frame ----

// The camera's cell's visit, begun on the root portal, the whole screen: its
// first edge is the side whose angles straddle the view's left edge (-128),
// starting at or before it and ending after it, and its window starts
// there, so the left edge's place is how far round that is. The angles put
// the eye inside the cell, so its sides run all the way round it and one
// does; should rounding leave none, the walk starts from the ring's first
// side in the plain view window, from -512, and ends by its steps. Its
// bounds are the screen's edges.
static uint8_t root_visit(pt_render *r)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[r->cam_cell];
    uint8_t root = 0;
    uint16_t lo = (uint16_t)((uint16_t)(r->cam4 - 512u) & 1023u);
    uint16_t tl = 384;
    uint16_t left = (uint16_t)((uint16_t)(r->cam4 - 128u) & 1023u);
    for (uint8_t k = 0; k <= c->num; k++) {
        uint16_t a0 = pt_vcache_vert(r->vc, m, pt_map_side(m, c, k)->v)->angle;
        uint16_t a1 = pt_vcache_vert(r->vc, m, pt_map_side_v1(m, c, k))->angle;
        // the turn from the start to the view's left edge, within the side's
        uint16_t off = (uint16_t)((uint16_t)(left - a0) & 1023u);
        if (off < (uint16_t)((uint16_t)(a1 - a0) & 1023u)) {
            root = k;
            lo = a0;
            tl = off;
            break;
        }
    }
    uint8_t vi = visit_of(r, r->cam_cell);
    pt_visit *vs = &r->visits[vi];
    vs->k = root;
    vs->lo = lo;
    vs->tl = tl;
    vs->lo8 = (uint8_t)(128u - tl);    // the window start's view angle, -128 - tl
    side_start(r, vs);
    vs->x = 0;
    vs->xr = PT_SCREEN_WIDTH;
    vs->tmax = 0;
    vs->bmin = PT_SCREEN_HEIGHT;
    return vi;
}

static void frame(pt_render *r, uint8_t angle, uint8_t cell)
{
    const pt_map *m = r->map;
    RC_ASSERT(cell >= 1 && cell < m->num_cells);

    // empty the last frame's visit cache: each record clears its own cell's
    // index
    for (uint8_t i = 1; i < r->num_visits; i++)
        r->cell_visit[r->visits[i].cell] = 0;
    r->num_visits = 1;

    r->cam_cell = cell;
    r->cam4 = (uint16_t)(angle << 2);
    RC_ASSERT(m->cells[cell].floor_z >= PT_CAM_HEIGHT);
    r->eye_z = pt_u8((int32_t)m->cells[cell].floor_z - PT_CAM_HEIGHT);

    // the records, and every byte column's last toggled row: row 0 to begin
    // with, as lines running off the top toggle it unrecorded
    r->num_recs = 0;
    memset(r->lastp1, 1, sizeof r->lastp1);

    // the root portal's bounds are the screen's edges, the top at row 0 and
    // the bottom in the junk row below: its extents say so, and top[] and
    // bot[] needn't hold them. Above row 0 is the camera cell's ceiling: the
    // fill starts each column from its colour
    walk(r, root_visit(r), 1);
    pt_mode5_fill(r->fb, r->tb, pt_shade(m->cells[cell].ceil), r->lastp1);
    unplot(r);

    if (r->num_visits > r->visits_high)
        r->visits_high = r->num_visits;
    if (r->num_recs > r->recs_high)
        r->recs_high = r->num_recs;
}

void pt_render_init(pt_render *r, const pt_map *m, pt_vcache *vc)
{
    *r = (pt_render) {0};
    r->map = m;
    r->vc = vc;
    r->tb = r->tmem + PT_FB_GUARD;
    r->sprites = true;
}

void pt_render_view(pt_render *r, uint8_t angle, uint8_t cell)
{
    frame(r, angle, cell);
}

void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell)
{
    pt_vcache_move(r->vc, cam_x, cam_y);
    frame(r, angle, pt_vcache_locate(r->vc, r->map, cell));
}
