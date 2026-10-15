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

// A height as its lines see it: the side (the subtract's borrow), and the
// log of |z - eye_z| in 8-unit steps, made once a cell a frame.
pt_height pt_render_height(uint8_t z, uint8_t eye_z)
{
    pt_height h = {.below = z >= eye_z};
    uint8_t m = h.below ? pt_u8((int32_t)z - eye_z) : pt_u8((int32_t)eye_z - z);
    h.l = m != 0 ? pt_i16((int32_t)pt_log2_8(m) + 17 * 256) : 0;
    return h;
}

static pt_height height_of(const pt_render *r, uint8_t z)
{
    return pt_render_height(z, r->eye_z);
}

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

// The edge's line at height h over its columns [x0, x1), from its ends:
// each end's row is its offset from the screen's middle, 64 * 8h / depth,
// u8.8: 2^(h.l - log depth). The gradient is the offsets' difference over
// the columns', by their logs (the columns' is the edge's, logcolumns, the
// same for all its lines). An end at or past the screen's edge, nearer than
// 2h, cuts the line where it reaches that edge, (32 rows - the far end's
// offset) / g columns on from the far end, and those columns are counted to
// the last one whose row is still on the screen; the columns cut off are
// beyond every bound on that side, and with both ends there the line has no
// columns at all.
static pt_line make_line(uint8_t x0, uint8_t x1, const pt_ends *e, pt_height h, pt_pattern col)
{
    RC_ASSERT(x0 < x1);
    uint16_t mid = (uint16_t)(PT_EDGE_OFF + 128u);    // the middle row, with the rounding half
    pt_line L = {.x0 = x0, .x1 = x0, .rl = h.below ? PT_SCREEN_HEIGHT : 0,
                 .below = h.below, .ce = col.e, .co = col.o};
    if (h.l == 0) {
        // at the eye's height: flat, on the middle row
        L.x1 = x1;
        L.y = mid;
        L.rl = row_of(mid);
        return L;
    }
    int16_t l0 = pt_i16((int32_t)h.l - e->logdepth0);
    int16_t l1 = pt_i16((int32_t)h.l - e->logdepth1);
    bool near0 = l0 >= PT_EDGE_L;
    bool near1 = l1 >= PT_EDGE_L;
    if (near0 && near1)
        return L;
    uint16_t o0 = exp2_sat(l0);
    uint16_t o1 = exp2_sat(l1);

    // the rows run away from the middle as the offset grows, down below the
    // eye and up above it: the offsets' subtract's borrow is the sign
    uint8_t dc = pt_u8((int32_t)x1 - x0);
    bool borrow = o1 < o0;
    uint16_t dr = borrow ? pt_u16((int32_t)o0 - o1) : pt_u16((int32_t)o1 - o0);
    uint8_t neg = (uint8_t)(h.below ? borrow : !borrow);    // (pt_g_make drops a 0's sign)
    int16_t lg = 0;
    uint16_t gm = 0;
    if (near0 || near1) {
        // a near end's offset is 32 rows or more, hundreds even, and the clip
        // below finds where the line crosses the screen's edge from the
        // gradient: the difference in 16 bits, and past 256 rows, where the
        // offset saturates, in whole rows (it's then at least 224 of them)
        if (o0 == 0xFFFF || o1 == 0xFFFF) {
            uint16_t w0 = exp2_sat(pt_i16((int32_t)l0 - 8 * 256));
            uint16_t w1 = exp2_sat(pt_i16((int32_t)l1 - 8 * 256));
            uint16_t dw = w1 > w0 ? pt_u16((int32_t)w1 - w0) : pt_u16((int32_t)w0 - w1);
            lg = pt_i16((int32_t)pt_log2(dw) + 8 * 256 - e->logcolumns);
        } else {
            lg = pt_i16((int32_t)pt_log2(dr) - e->logcolumns);
        }
        gm = exp2_sat(lg);
        gm = gm > 0x7FFF ? (uint16_t)0x7FFF : gm;
    } else {
        // both ends on the screen: each offset is under 32 rows, and so is
        // their difference, a byte in eighths of a row (rounded; 0 under a
        // sixteenth all along, and the line is flat), whose log is a byte's,
        // with no shifts (pt_log2_8). 5 * 256 takes the eighths back to
        // 256ths. The gradient's under 32 rows a column, so its exp never
        // saturates
        uint16_t d = (uint16_t)((dr + 16u) >> 5);
        uint8_t d8 = d > 255 ? (uint8_t)255 : (uint8_t)d;
        if (d8 != 0) {
            lg = pt_i16((int32_t)pt_log2_8(d8) + 5 * 256 - e->logcolumns);
            RC_ASSERT(lg < 13 * 256);
            gm = exp2_sat(lg);
        }
    }
    L.g = pt_g_make(gm, neg);

    // both ends on the screen: rl is the far end's own row, from its depth,
    // and the rows stepped by the rounded gradient can overshoot it by up to
    // 78/256 of a row, into the next row in about 0.5% of lines (fast_inside
    // says what that costs). Stepping to the last column instead would cost
    // a multiply a line
    if (!near0 && !near1) {
        L.x1 = x1;
        L.y = h.below ? pt_u16((int32_t)mid + o0) : pt_u16((int32_t)mid - o0);
        L.rl = row_of(h.below ? pt_u16((int32_t)mid + o1) : pt_u16((int32_t)mid - o1));
        return L;
    }

    // one end too near: the far end's columns towards the crossing, ceil
    // (delta) of them counting its own (CMP #1 on the low byte, ADC #0 into
    // the high). The far end's distance from the edge is under 32 rows, a
    // byte in eighths of a row (rounded; at least one, as the far end's
    // offset is at most 8,176) with a byte's log: 5 * 256 back to 256ths, and
    // 8 * 256 more for delta's fraction.
    //
    // The last column counted is still on the screen. Stepping to it is g
    // (cnt - 1), under g delta, and g delta is the distance again: lg
    // cancels between the two exps, leaving their roundings and the log's
    // (0.5% of under 32 rows), the eighth's rounding (a sixteenth of a row)
    // and g's own (1/512 a column, a quarter of a row over 128). The
    // rounding half leaves half a row past either edge. Never missed in
    // 676,000 near lines over 400,000 poses of both maps
    uint16_t of = near0 ? o1 : o0;
    uint16_t de = (uint16_t)((PT_EDGE_OFF - of + 16u) >> 5);
    uint8_t de8 = de > 255 ? (uint8_t)255 : (uint8_t)de;
    int16_t lq = pt_i16((int32_t)pt_log2_8(de8) + 13 * 256 - lg);
    uint16_t dq = exp2_sat(lq);
    uint16_t n = (uint16_t)((dq >> 8) + ((uint8_t)dq != 0 ? 1u : 0u));
    uint8_t cnt = n > dc ? dc : (uint8_t)n;
    if (cnt == 0)
        return L;
    bool ok;
    if (near1) {
        uint16_t y0 = h.below ? pt_u16((int32_t)mid + o0) : pt_u16((int32_t)mid - o0);
        uint16_t yl;
        ok = row_move(y0, L.g, (uint8_t)(cnt - 1u), true, &yl);
        L.x1 = (uint8_t)(x0 + cnt);
        L.y = y0;
        L.rl = row_of(yl);
    } else if (cnt > 1) {
        // the end's own column isn't the line's: cnt - 1 back from it, the
        // row g (cnt - 1) nearer the screen's edge
        uint16_t y1 = h.below ? pt_u16((int32_t)mid + o1) : pt_u16((int32_t)mid - o1);
        uint8_t back = (uint8_t)(cnt - 1u);
        ok = row_move(y1, L.g, back, false, &L.y);
        L.x0 = (uint8_t)(x1 - back);
        L.x1 = x1;
        L.rl = row_of(y1);
    } else {
        ok = true;    // just the end's own column: none
    }
    RC_ASSERT(ok);
    (void)ok;
    return L;
}

pt_line pt_render_line(const pt_walk *w, const pt_ends *e, pt_height h)
{
    return make_line(w->c0, w->c1, e, h, (pt_pattern) {0});
}

// ---- plotting ----

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
//   CMP toggle_end,X : BCC no : ADC #0 : STA toggle_end,X    ; C set: the row + 1
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
        if (row >= r->toggle_end[bc])
            r->toggle_end[bc] = pt_u8((int32_t)row + 1);
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

void pt_render_plot_run(pt_render *r, const pt_line *L, uint8_t x0, uint8_t x1, uint16_t y)
{
    plot_run(r, L, x0, x1, y);
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
// it. *y is its row at xa wherever that's one of its columns, for the scan
// to start from if the answer's no.
//
// Near enough: the last few rows can be one past rl (make_line), so one
// can land on a bound. That's harmless here, as visible or hidden it's
// toggled on that row; but the extent a floor or ceiling clip then gives
// its child is a row too generous, and a child's line overshooting its own
// rl in the same column can be drawn a row past its bound: a pixel of the
// wrong colour. Never seen in the sweep, and the cheapest guarantee costs
// the 6502 130 cycles a frame.
static bool fast_inside(const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax, uint8_t bmin,
                        uint16_t *y)
{
    if (xa < L->x0 || xa >= L->x1)
        return false;
    *y = line_y(L, xa);
    return xb <= L->x1 && inside(row_of(*y), tmax, bmin) && inside(L->rl, tmax, bmin);
}

// The screen's edges as bounds' rows, a constant row of each. Where a
// visit's extent says a bound is the screen's edge all along (tmax 0, bmin
// 64), the arrays don't hold it, and its scans read these instead: the 6502
// points the scan's CMP at them, once a scan.
#define PT_ROW8(v) v, v, v, v, v, v, v, v
#define PT_ROW64(v) \
    PT_ROW8(v), PT_ROW8(v), PT_ROW8(v), PT_ROW8(v), PT_ROW8(v), PT_ROW8(v), PT_ROW8(v), PT_ROW8(v)
_Static_assert(PT_SCREEN_WIDTH == 128, "the screen's edges' rows are two 64s");
static const uint8_t screen_top_[PT_SCREEN_WIDTH] = {0};
static const uint8_t screen_bot_[PT_SCREEN_WIDTH] = {PT_ROW64(PT_SCREEN_HEIGHT),
                                                     PT_ROW64(PT_SCREEN_HEIGHT)};

// The visible stretch a scan is gathering, plotted whole when it ends.
typedef struct pt_vrun {
    uint8_t x0;    // its first column, 0xFF for none
    uint16_t y0;   // the line's row there
} pt_vrun;

// The hidden stretch a scan is gathering: toggles on one row, in the line's
// colour, plotted as a flat run (a byte at a time, the ends masked) when the
// row changes. Nearly all hidden columns are a line running off the screen
// against the root's bounds: off the top they're all on row 0, and off the
// bottom they're in the junk row, where nothing is ever seen and the run is
// dropped.
typedef struct pt_hrun {
    uint8_t x0;     // its first column, 0xFF for none
    uint8_t row;
} pt_hrun;

// One line's scan: the line, its bounds' rows, and the runs it's gathering.
typedef struct pt_scan {
    pt_render *r;
    const pt_line *L;
    pt_pattern col;       // the line's colour
    const uint8_t *top;   // the bounds' rows: the arrays, or the screen's
    const uint8_t *bot;   // edges' where the extents say so
    uint8_t ext;          // the child's bound's extent so far: the floor line's
                          // highest row, the ceiling line's lowest
    bool room;            // a bound line has left the child room in a column:
                          // its bottom below its top
    pt_vrun v;
    pt_hrun h;
} pt_scan;

static pt_scan scan_of(pt_render *r, const pt_line *L, uint8_t tmax, uint8_t bmin)
{
    return (pt_scan) {.r = r, .L = L, .col = {.e = L->ce, .o = L->co},
                      .top = tmax == 0 ? screen_top_ : r->top,
                      .bot = bmin == PT_SCREEN_HEIGHT ? screen_bot_ : r->bot,
                      .v = {.x0 = 0xFF}, .h = {.x0 = 0xFF}};
}

static void run_end(pt_scan *s, uint8_t x1)
{
    if (s->v.x0 != 0xFF)
        plot_run(s->r, s->L, s->v.x0, x1, s->v.y0);
    s->v.x0 = 0xFF;
}

static void hrun_end(pt_scan *s, uint8_t x1)
{
    if (s->h.x0 != 0xFF && s->h.row < PT_SCREEN_HEIGHT && (s->col.e | s->col.o) != 0)
        pt_plot_flat(s->r->tb, s->h.x0, x1, s->h.row, s->col);
    s->h.x0 = 0xFF;
}

// Column c visible at row y, or hidden at a bound's row: each ends the
// other's stretch.
static void col_visible(pt_scan *s, uint8_t c, uint16_t y)
{
    hrun_end(s, c);
    if (s->v.x0 == 0xFF)
        s->v = (pt_vrun) {.x0 = c, .y0 = y};
}

static void col_hidden(pt_scan *s, uint8_t c, uint8_t row)
{
    run_end(s, c);
    if (s->h.x0 != 0xFF && s->h.row != row)
        hrun_end(s, c);
    if (s->h.x0 == 0xFF)
        s->h = (pt_hrun) {.x0 = c, .row = row};
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

typedef void (*pt_colfn)(pt_scan *s, uint8_t c, uint16_t y);

// Scan [xa, xb) a column at a time, and plot the stretches it gathered.
// Before the line's own columns and after them the row is beyond the screen
// on its side, the same all along, and between them the line's own rows
// step as the plotter steps them: three loops on the 6502, none comparing
// the column with the line's. ya is the line's row at xa, where that's one
// of its columns (fast_inside's).
static void scan(pt_scan *s, uint8_t xa, uint8_t xb, pt_colfn col, uint16_t ya)
{
    const pt_line *L = s->L;
    uint8_t l, rr;
    on_screen(L, xa, xb, &l, &rr);
    uint16_t beyond = L->below ? (uint16_t)(PT_SCREEN_HEIGHT << 8) : 0;
    for (uint8_t c = xa; c < l; c++)
        col(s, c, beyond);
    if (l < rr) {
        uint16_t y = l == xa ? ya : line_y(L, l);
        for (uint8_t c = l;;) {
            // a line's own rows are on its own side of the middle row (make_line's offsets
            // run from it), so it only meets the screen's edge on that side: the 6502's plain
            // loops count on it
            RC_ASSERT(L->below ? row_of(y) >= PT_SCREEN_HEIGHT / 2
                               : row_of(y) <= PT_SCREEN_HEIGHT / 2);
            col(s, c, y);
            if (++c == rr)
                break;
            y = step1(y, L->g);
        }
    }
    for (uint8_t c = rr; c < xb; c++)
        col(s, c, beyond);
    run_end(s, xb);
    hrun_end(s, xb);
}

// ---- the clips ----

// A line forming no bound: visible strictly between the bounds, else
// toggled at the one it's on or past.
static void plain_col(pt_scan *s, uint8_t c, uint16_t y)
{
    uint8_t row = row_of(y);
    uint8_t t = s->top[c];
    uint8_t b = s->bot[c];
    if (row > t && row < b)
        col_visible(s, c, y);
    else
        col_hidden(s, c, row <= t ? t : b);
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
    pt_scan s = scan_of(r, L, tmax, bmin);
    if ((s.col.e | s.col.o) == 0)
        return;    // a line forming no bound only toggles
    if (L->below ? bmin == PT_SCREEN_HEIGHT : tmax == 0) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        if (!L->below) {
            flat_top(r, xa, l, s.col);
            flat_top(r, rr, xb, s.col);
        }
        if (l == rr)
            return;
        xa = l;
        xb = rr;
    }
    uint16_t y = 0;
    if (fast_inside(L, xa, xb, tmax, bmin, &y)) {
        plot_run(r, L, xa, xb, y);
        return;
    }
    scan(&s, xa, xb, plain_col, y);
}

// The child's bottom: the line, merged into the parent's top where it's on
// or above that, and into the parent's bottom where it's on or below that.
// Where it's merged into the top, the child has no room: its bottom is its
// top, nothing's visible, and every toggle its walk makes there lands on
// that one row, which telescopes to its ceiling ^ floor. Where it lends the
// parent's bottom, that's in bot[] already, unless it's the screen's edge,
// which the array doesn't hold.
static void floor_col(pt_scan *s, uint8_t c, uint16_t y)
{
    uint8_t *bot = s->r->bot;
    uint8_t row = row_of(y);
    uint8_t t = s->top[c];
    uint8_t b = s->bot[c];
    if (row <= t) {
        col_hidden(s, c, t);
        row = t;
        bot[c] = row;
    } else if (row < b) {
        col_visible(s, c, y);
        bot[c] = row;
    } else {
        col_hidden(s, c, b);
        row = b;
        if (s->bot != bot)
            bot[c] = row;
    }
    s->ext = row < s->ext ? row : s->ext;
    s->room = s->room || row > t;
}

static void floor_fast_col(pt_scan *s, uint8_t c, uint16_t y)
{
    s->r->bot[c] = row_of(y);
}

// The line forming the child's bottom, over [xa, xb) (floor_col). Writes
// the child's bottom rows; the child's bottom's extent, its highest row, is
// the result, and *room whether it has room in any column. The columns the
// screen's pre-clip cut off come first, as for clip_plain: against the
// screen's edge, off the bottom they lend the junk row, and off the top the
// child's bottom there is row 0, its top. On the fast path the extent is
// the line's ends', and every column has room.
static uint8_t clip_floor(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                          uint8_t bmin, bool *room)
{
    pt_scan s = scan_of(r, L, tmax, bmin);
    s.ext = PT_SCREEN_HEIGHT;
    if (L->below ? bmin == PT_SCREEN_HEIGHT : tmax == 0) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        if (L->below && l == rr) {
            // all cut off: the child's bottom is the screen's all along, which bot[] needn't
            // hold, as its extent says so
            *room = true;
            return PT_SCREEN_HEIGHT;
        }
        if (L->below) {
            memset(&r->bot[xa], PT_SCREEN_HEIGHT, (size_t)(l - xa));
            memset(&r->bot[rr], PT_SCREEN_HEIGHT, (size_t)(xb - rr));
            s.room = l > xa || rr < xb;
        } else {
            flat_top(r, xa, l, s.col);
            flat_top(r, rr, xb, s.col);
            memset(&r->bot[xa], 0, (size_t)(l - xa));
            memset(&r->bot[rr], 0, (size_t)(xb - rr));
            if (l > xa || rr < xb)
                s.ext = 0;
        }
        if (l == rr) {
            *room = s.room;
            return s.ext;
        }
        xa = l;
        xb = rr;
    }
    uint16_t y = 0;
    if (fast_inside(L, xa, xb, tmax, bmin, &y)) {
        scan(&s, xa, xb, floor_fast_col, y);
        plot_run(r, L, xa, xb, y);
        *room = true;
        uint8_t lo = row_of(y) < L->rl ? row_of(y) : L->rl;
        return lo < s.ext ? lo : s.ext;
    }
    scan(&s, xa, xb, floor_col, y);
    *room = s.room;
    return s.ext;
}

// The child's top, after the floor line: the line, merged into the parent's
// top where it's on or above that (lending it, as floor_col lends the
// bottom), and into the child's bottom where it's on or below that, so the
// child has no room there (as for floor_col).
static void ceil_col(pt_scan *s, uint8_t c, uint16_t y)
{
    uint8_t *top = s->r->top;
    uint8_t row = row_of(y);
    uint8_t t = s->top[c];
    uint8_t b = s->bot[c];
    if (row <= t) {
        col_hidden(s, c, t);
        row = t;
        if (s->top != top)
            top[c] = row;
    } else if (row < b) {
        col_visible(s, c, y);
        top[c] = row;
    } else {
        col_hidden(s, c, b);
        row = b;
        top[c] = row;
    }
    s->ext = row > s->ext ? row : s->ext;
    s->room = s->room || row < b;
}

static void ceil_fast_col(pt_scan *s, uint8_t c, uint16_t y)
{
    s->r->top[c] = row_of(y);
}

// The line forming the child's top, over [xa, xb) (ceil_col), against the
// parent's top (tmax its extent) and the child's bottom (cbmin). Writes the
// child's top rows; the child's top's extent, its lowest row, is the result,
// and *room whether the child has room in any column, its bottom below its
// top. The columns the screen's pre-clip cut off the top come first, as for
// clip_plain: against the screen's top they merge into row 0, the child's
// top there. The fast path is inside the child's bottom all along: room.
static uint8_t clip_ceil(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                         uint8_t cbmin, bool *room)
{
    pt_scan s = scan_of(r, L, tmax, cbmin);
    s.ext = 0;
    if (!L->below && tmax == 0) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        flat_top(r, xa, l, s.col);
        flat_top(r, rr, xb, s.col);
        memset(&r->top[xa], 0, (size_t)(l - xa));
        memset(&r->top[rr], 0, (size_t)(xb - rr));
        for (uint8_t c = xa; c < l; c++)
            s.room = s.room || s.bot[c] > 0;
        for (uint8_t c = rr; c < xb; c++)
            s.room = s.room || s.bot[c] > 0;
        if (l == rr) {
            *room = s.room;
            return s.ext;
        }
        xa = l;
        xb = rr;
    }
    uint16_t y = 0;
    if (fast_inside(L, xa, xb, tmax, cbmin, &y)) {
        scan(&s, xa, xb, ceil_fast_col, y);
        plot_run(r, L, xa, xb, y);
        *room = true;
        uint8_t hi = row_of(y) > L->rl ? row_of(y) : L->rl;
        return hi > s.ext ? hi : s.ext;
    }
    scan(&s, xa, xb, ceil_col, y);
    *room = s.room;
    return s.ext;
}

// Close columns [l, rr) of a child after the fact: its whole stack there
// telescopes to its ceiling ^ floor, toggled at its bottom's row, as the
// closing line's own toggle does. A bottom in the junk row, all along (bz)
// or in a column, is nothing to toggle, and nor is no colour.
static void close_columns(pt_render *r, uint8_t l, uint8_t rr, pt_pattern cbfb, bool bz)
{
    if (bz || (cbfb.e | cbfb.o) == 0)
        return;
    for (uint8_t c = l; c < rr; c++)
        if (r->bot[c] < PT_SCREEN_HEIGHT)
            pt_mode5_plot(r->tb, c, r->bot[c], cbfb);
}

void pt_render_clip_plain(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                          uint8_t bmin)
{
    clip_plain(r, L, xa, xb, tmax, bmin);
}

uint8_t pt_render_clip_floor(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb,
                             uint8_t tmax, uint8_t bmin, bool *room)
{
    return clip_floor(r, L, xa, xb, tmax, bmin, room);
}

uint8_t pt_render_clip_ceil(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                            uint8_t cbmin, bool *room)
{
    return clip_ceil(r, L, xa, xb, tmax, cbmin, room);
}

void pt_render_close_columns(pt_render *r, uint8_t l, uint8_t rr, pt_pattern cbfb, bool bz)
{
    close_columns(r, l, rr, cbfb, bz);
}

// ---- angles and columns ----
//
// A vertex's screen angle is its angle from the view's left edge, (angle -
// vl) & 1023: one 10-bit subtract, the same for every walk. The view is
// screen angles 0 to 255, and there a screen angle is the column table's
// index. All a walk adds is where its turn splits: its cell is read in a
// window of one turn, the screen angles from split_angle - 1024 to
// split_angle - 1 (one at or past split_angle counts a turn less, left of
// the view), which holds the cell in one piece. A cell seen through a
// portal spans under a half turn, the portal's middle among it, and its
// window is centred there; the eye's own cell spans the whole turn, and its
// window starts where the walk does (root_walk). So clockwise along the
// sides facing the eye the screen angles only rise, even round the back of
// the view. Left of the view, a screen angle below 0, is column 0, and right
// of it, 256 on, 128.
//
// A window's made when a walk first enters the cell, centred on the portal
// it comes in by. Any portal into a cell has its middle within the cell's
// span, so any such window holds the cell in one piece, and gives each
// vertex the same column: a later visit, through another portal, keeps the
// first one's, and sees the sides just as it did.

// Vertex v's screen angle in the walk's window.
static int16_t vert_to_screen_angle(const pt_render *r, const pt_walk *w, uint8_t v)
{
    uint16_t a = pt_vcache_vert(r->vc, r->map, v)->angle;
    uint16_t p = (uint16_t)((uint16_t)(a - r->vl) & 1023u);
    return p >= w->split_angle ? pt_i16((int32_t)p - 1024) : (int16_t)p;
}

// Do two windows give the cell's vertices the same screen angles? They do
// if no vertex lies between their split angles.
static bool same_window(const pt_render *r, const pt_cell *c, uint16_t split_angle0,
                        uint16_t split_angle1)
{
    for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
        uint16_t a = pt_vcache_vert(r->vc, r->map, pt_map_side(r->map, s)->v)->angle;
        uint16_t p = (uint16_t)((uint16_t)(a - r->vl) & 1023u);
        if ((p >= split_angle0) != (p >= split_angle1))
            return false;
    }
    return true;
}

// The view angle of an in-view screen angle, a byte: u - 128.
static int8_t view8(int16_t u)
{
    return (int8_t)(uint8_t)((uint8_t)u ^ 0x80u);
}

// The column of a screen angle: in view, where its step's middle projects;
// left of the view, 0; right of it, 128. On the 6502 its high byte says
// which: negative, not zero, or zero, and then the low byte is the index.
static uint8_t screen_angle_to_column(int16_t u)
{
    if (u < 0)
        return 0;
    if (u >= 256)
        return PT_SCREEN_WIDTH;
    return pt_view_col((uint8_t)u);
}

// The current side's end. A side facing the eye turns clockwise by up to
// a half turn, and its end is its start's screen angle plus that: in a
// child's window that's where the end is anyway, and in the eye's own cell,
// whose window is a whole turn, the last side runs on round to the first
// one's start, a turn up. One facing away (a cell seen through a portal has a
// chain of them, the portal among them) turns back, its end at or left of
// its start in the window, and spans nothing. So the end is where it is in
// the window, unless that's half a turn or more left of the start: then
// it's a turn on (on the 6502, a compare, and rarely an add).
static void side_end(const pt_render *r, pt_walk *w)
{
    const pt_visit *vs = &r->visits[w->vi];
    const pt_cell *c = &r->map->cells[vs->cell];
    int16_t u = vert_to_screen_angle(r, w, pt_map_side_v1(r->map, c, vs->side));
    w->u1 = u < w->u0 && w->u0 - u >= 512 ? pt_i16((int32_t)u + 1024) : u;
    w->c1 = screen_angle_to_column(w->u1);
}

// Begin on the record's side: its start's screen angle and column, and its
// end's.
static void side_start(const pt_render *r, pt_walk *w)
{
    const pt_visit *vs = &r->visits[w->vi];
    w->u0 = vert_to_screen_angle(r, w, pt_map_side(r->map, vs->side)->v);
    w->c0 = screen_angle_to_column(w->u0);
    side_end(r, w);
}

// On to the next side clockwise: its start is this one's end, so a side
// costs one vertex's screen angle and column. Any lines kept were the last
// side's.
// The walk only moves on from a side ending left of its portal's right
// column, so never from the eye's own cell's last, a turn on.
static void side_next(pt_render *r, pt_walk *w)
{
    pt_visit *vs = &r->visits[w->vi];
    const pt_cell *c = &r->map->cells[vs->cell];
    vs->side = pt_map_ring_next(c, vs->side);
    vs->kept = 0;
    RC_ASSERT(w->u1 < 256);
    w->u0 = w->u1;
    w->c0 = w->c1;
    side_end(r, w);
}

void pt_render_side_next(pt_render *r, pt_walk *w)
{
    side_next(r, w);
}

// ---- sprites ----

// A cell's sprite as it's seen, at the cell's first visit: spritescale's
// place for it, in logs as the lines are; 0 when it can't be seen (none,
// beside or behind the eye, too near, too small, off the screen, or no room
// left in the clip pool). The base's centre is a vertex, so its polar form
// is the vertex cache's, and its depth's log is the length's less lsec of
// its view angle.
//   - The scale, 64 size / (h depth) in 8.8, is 2^(14 + log size - log h -
//     log depth), the first three the sprite's own, made with the map.
//   - The centre's column is 64 + 64 tan v: tan by log sine and lsec, so a
//     centre off the view (where a wide sprite's edge can still show) needs
//     no table past it. Out to 255 pixels each side; further, the sprite is
//     nearer than PT_SPRITE_NEAR or too far to the side to show anyway. Its
//     left column is half the plotted width (spritescale's, by the
//     accumulator) less.
//   - Its bottom is the floor's row at that depth, as the floor's lines have
//     it (make_line's offset from the middle, from the cell's floor height,
//     in whole rows past 255), and the pair above is its first.
// The instance goes into the draw order by depth, far first. Sprites stand
// inside their cells, so where two overlap on the screen one's cell is seen
// through the other's, and the nearer is the shallower. Its clip rows take
// two bytes a column from the pool, and only the tops are set: a top of 0
// hides a column whatever its bottom.
static uint8_t inst_new(pt_render *r, uint8_t cell, pt_height hf)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[cell];
    if (c->sprite == 0 || !r->sprites)
        return 0;
    const pt_sprite *sp = &m->sprites[c->sprite];
    const pt_sprite_art *a = &m->arts[sp->art];

    const pt_polar *pp = pt_vcache_vert(r->vc, m, sp->v);
    uint16_t va = (uint16_t)((uint16_t)(pp->angle - r->cam4) & 1023u);
    if (va >= 256 && va < 768)
        return 0;    // beside or behind
    bool left = va >= 512;
    uint8_t i = left ? (uint8_t)~(uint8_t)va : (uint8_t)va;    // steps off straight ahead
    int16_t ld = pt_i16((int32_t)pp->lr - pt_lsec16(i));
    if (ld < PT_SPRITE_NEAR_L)
        return 0;

    uint8_t w = pt_sprite_width(a);
    uint8_t h = pt_sprite_height(a);
    int32_t ls = (int32_t)sp->ls - ld;
    if (ls < 0)
        return 0;
    uint16_t scale = pt_exp2(pt_u16(ls));

    // the left column
    int32_t lt = 14 * 256 + (int32_t)pt_log_sine(va) + pt_lsec16(i);
    uint16_t off = lt < 0 ? (uint16_t)0 : pt_exp2(pt_u16(lt));
    int32_t centre = left ? 64 * 256 - (int32_t)off : 64 * 256 + (int32_t)off;
    uint16_t pw = pt_u16((int32_t)((128u + (uint32_t)w * scale) >> 8));
    int16_t sx = pt_i16((centre - (int32_t)pw * 128 + 128) >> 8);

    // the bottom pair
    int32_t o = 0;
    if (hf.l != 0) {
        int32_t lo = (int32_t)hf.l - ld;
        if (lo >= 16 * 256)
            o = (int32_t)pt_exp2(pt_u16(lo - 8 * 256)) << 8;
        else if (lo >= 0)
            o = pt_exp2(pt_u16(lo));
    }
    int32_t y = PT_EDGE_OFF + 128 + (hf.below ? o : -o);
    int16_t sy = pt_i16(PT_SCREEN_HEIGHT - (y >> 8));

    uint16_t ph = pt_u16((int32_t)((128u + (uint32_t)h * scale) >> 8));
    int32_t cx0 = sx < 0 ? 0 : sx;
    int32_t cx1 = (int32_t)sx + pw > PT_SCREEN_WIDTH ? PT_SCREEN_WIDTH : (int32_t)sx + pw;
    int32_t p1 = (int32_t)sy + ph;
    if (sy >= PT_SCREEN_HEIGHT || p1 <= 0 || cx0 >= cx1)
        return 0;
    uint8_t width = (uint8_t)(cx1 - cx0);
    if (r->clip_used + 2u * width > PT_CLIP_POOL)
        return 0;

    RC_PANIC(r->num_insts < PT_MAX_INSTS);
    uint8_t ii = r->num_insts++;
    pt_inst *in = &r->insts[ii];
    in->sprite = c->sprite;
    in->place = (pt_sprite_place) {.scale = scale, .x = sx, .y = sy};
    in->ld = ld;
    in->cx0 = (uint8_t)cx0;
    in->cx1 = (uint8_t)cx1;
    in->p0 = sy < 0 ? (uint8_t)0 : (uint8_t)sy;
    in->p1 = p1 > PT_SCREEN_HEIGHT ? (uint8_t)PT_SCREEN_HEIGHT : (uint8_t)p1;
    in->clip = r->clip_used;
    r->clip_used = (uint16_t)(r->clip_used + 2u * width);
    memset(&r->clipmem[in->clip + width], 0, width);    // shown by no visit yet
    in->cols = 0;
    in->whole = true;
    uint8_t k = r->num_order++;
    for (; k > 0 && r->insts[r->order[k - 1]].ld < ld; k--)
        r->order[k] = r->order[k - 1];
    r->order[k] = ii;
    return ii;
}

// A visit to a sprite's cell over [xl, xr): the rows its portal shows in
// the sprite's columns there, as pairs up from the screen's bottom. The
// bounds are in top[] and bot[] (or are the screen's edges, as the extents
// say, and filled in whole), and the visit's children haven't written over
// them yet. On the 6502 each is LDA #64 : SBC bot,X with the carry set once
// for the loop (a bound is at most 64, so it never borrows). The extents
// also say whether every column here shows all of the sprite: if each
// visit's do, and together they show every column, no strip of it needs
// classing.
static void inst_clip(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr, uint8_t tmax, uint8_t bmin)
{
    pt_inst *in = &r->insts[ii];
    uint8_t c0 = xl > in->cx0 ? xl : in->cx0;
    uint8_t c1 = xr < in->cx1 ? xr : in->cx1;
    if (c0 >= c1)
        return;
    uint8_t width = (uint8_t)(in->cx1 - in->cx0);
    uint8_t n = (uint8_t)(c1 - c0);
    uint8_t *lo = &r->clipmem[in->clip + (uint8_t)(c0 - in->cx0)];
    uint8_t *hi = lo + width;
    if (bmin == PT_SCREEN_HEIGHT)
        memset(lo, 0, n);
    else
        for (uint8_t i = 0; i < n; i++)
            lo[i] = (uint8_t)(PT_SCREEN_HEIGHT - r->bot[c0 + i]);
    if (tmax == 0)
        memset(hi, PT_SCREEN_HEIGHT, n);
    else
        for (uint8_t i = 0; i < n; i++)
            hi[i] = (uint8_t)(PT_SCREEN_HEIGHT - r->top[c0 + i]);
    in->cols = (uint8_t)(in->cols + n);
    if (PT_SCREEN_HEIGHT - bmin > in->p0 || PT_SCREEN_HEIGHT - tmax < in->p1)
        in->whole = false;
}

// Hide a sprite over [xl, xr): a visit the walk won't follow after all.
static void inst_hide(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr)
{
    pt_inst *in = &r->insts[ii];
    uint8_t c0 = xl > in->cx0 ? xl : in->cx0;
    uint8_t c1 = xr < in->cx1 ? xr : in->cx1;
    if (c0 >= c1)
        return;
    uint8_t width = (uint8_t)(in->cx1 - in->cx0);
    memset(&r->clipmem[in->clip + width + (uint8_t)(c0 - in->cx0)], 0, (size_t)(c1 - c0));
    in->whole = false;
}

uint8_t pt_render_inst_new(pt_render *r, uint8_t cell, pt_height hf)
{
    return inst_new(r, cell, hf);
}

void pt_render_inst_clip(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr, uint8_t tmax,
                         uint8_t bmin)
{
    inst_clip(r, ii, xl, xr, tmax, bmin);
}

void pt_render_inst_hide(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr)
{
    inst_hide(r, ii, xl, xr);
}

// ---- visits ----

// The cell's visit record, made on first need, with the cell's heights as
// its lines see them from the frame's eye.
static uint8_t visit_of(pt_render *r, uint8_t cell)
{
    uint8_t vi = r->cell_visit[cell];
    if (vi == 0) {
        RC_PANIC(r->num_visits <= PT_MAX_VISITS);
        vi = r->num_visits++;
        const pt_cell *c = &r->map->cells[cell];
        r->visits[vi] = (pt_visit) {.cell = cell, .hc = height_of(r, c->ceil_z),
                                    .hf = height_of(r, c->floor_z)};
        r->cell_visit[cell] = vi;
    }
    return vi;
}

// Enter visit vi over [xl, xr), its bounds already written, from the walk
// whose current side is the portal (from): the child's walk, *cw, in the
// visit's window. A first visit makes it, centred on the portal's middle.
// From's screen angles are the child's too, both measured from the view's
// left edge, so only the split moves, to half a turn past the middle; and as
// any portal into the cell has its middle within the cell's span, a later
// visit's portal would give the cell's vertices the same angles, so it
// keeps the first one's. cw may be from itself: the tail call builds the
// child over its parent, which is done with. A first walk looks from the
// edge after the twin (the side the walk came in by); a later one resumes
// from the edge its last walk stopped on - every earlier edge ends left of
// that walk's xr, which is at or left of xl.
// Either way the cursor then goes to the first edge whose end is strictly
// right of xl, at the latest the one ending at the portal's right end.
static void enter(pt_render *r, uint8_t vi, uint8_t twin, const pt_walk *from, uint8_t xl,
                  uint8_t xr, uint8_t tmax, uint8_t bmin, pt_walk *cw)
{
    pt_visit *vs = &r->visits[vi];
    const pt_cell *c = &r->map->cells[vs->cell];
    int16_t u0 = from->u0;    // (cw may be from itself)
    uint8_t c0 = from->c0;
    int16_t mid = pt_i16(((int32_t)from->u0 + from->u1) >> 1);
    uint16_t split_angle = pt_u16((int32_t)mid + 512);
    RC_ASSERT(split_angle >= 256 && split_angle <= 1024);
    bool first = vs->xr == 0;
    if (first) {
        vs->split_angle = split_angle;
        vs->side = pt_map_ring_next(c, twin);
    }
    RC_ASSERT(same_window(r, c, vs->split_angle, split_angle));
    *cw = (pt_walk) {.vi = vi, .x = xl, .tmax = tmax, .bmin = bmin,
                     .split_angle = vs->split_angle};
    if (first) {
        // the side after the twin starts where the portal does, and that vertex keeps its screen
        // angle in the new window, which splits half a turn from the portal's middle: only the
        // side's end is new (on the 6502, the parent's walk_u0, walk_e0 and walk_c0 stay)
        cw->u0 = u0;
        cw->c0 = c0;
        RC_ASSERT(vert_to_screen_angle(r, cw, pt_map_side(r->map, vs->side)->v) == u0);
        side_end(r, cw);
        vs->inst = inst_new(r, vs->cell, vs->hf);
    } else {
        side_start(r, cw);
    }
    for (uint8_t steps = pt_u8((int32_t)pt_map_ring_end(c) - c->first - 1); cw->c1 <= xl; steps--) {
        RC_ASSERT(steps != 0);
        side_next(r, cw);
    }
    vs->xr = xr;
    if (vs->inst != 0)
        inst_clip(r, vs->inst, xl, xr, tmax, bmin);
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
//
// Each line covers the edge's whole span in the view, whatever the portal's,
// so a later visit that resumes on the side can use it as it is: a walk
// that stops on a side whose end is right of its portal's (no later visit
// could resume on any other) keeps the side's lines in the pool, and the
// visit record points at them. Then the next visit works out no ends and
// makes no lines at all, only rows partway along them.
typedef struct pt_edgelines {
    const pt_walk *w;      // the walk: the side's ends' screen angles and
                           // columns
    const pt_side *sd;
    uint8_t v1;            // the end's vertex (the start's is the side's)
    bool have;             // e is worked out
    uint8_t kept;          // the side's lines in the pool, 0 for none:
    bool use;              // the last visit's, to use, or to keep as they're made
    pt_ends e;
} pt_edgelines;

// The kept lines' slots, by height: an edge makes at most one line at each
enum { PT_K_CEIL, PT_K_CEIL_ADJ, PT_K_FLOOR, PT_K_FLOOR_ADJ };

static void edge_ends(const pt_render *r, pt_edgelines *el)
{
    const pt_map *m = r->map;
    const pt_walk *w = el->w;
    const pt_polar *p0 = pt_vcache_vert(r->vc, m, el->sd->v);
    const pt_polar *p1 = pt_vcache_vert(r->vc, m, el->v1);
    bool in0 = w->u0 >= 0;
    bool in1 = w->u1 < 256;
    if (in0)
        el->e.logdepth0 = pt_i16((int32_t)p0->lr - pt_log_secant(view8(w->u0)));
    if (in1)
        el->e.logdepth1 = pt_i16((int32_t)p1->lr - pt_log_secant(view8(w->u1)));
    if (!in0 || !in1) {
        // the edge's direction, its quadrant stored with the side; the view's
        // edges' rays, at vl and vl + 256, are a half step off its steps'
        // middles
        uint16_t w = pt_map_side_angle(m, el->sd);
        const pt_polar *pk = in1 ? p1 : p0;
        uint16_t aw = (uint16_t)(pk->angle - w);
        int16_t ls = pt_i16(((int32_t)pt_log_sine((uint16_t)(aw - 1u)) + pt_log_sine(aw)) >> 1);
        int16_t ld = pt_i16((int32_t)pk->lr + ls - 128);
        if (!in0)
            el->e.logdepth0 = pt_i16((int32_t)ld - pt_log_sine((uint16_t)(r->vl - 1u - w)));
        if (!in1)
            el->e.logdepth1 = pt_i16((int32_t)ld - pt_log_sine((uint16_t)(r->vl + 255u - w)));
    }
    RC_ASSERT(w->c0 < w->c1);    // the edge has columns in its portal
    el->e.logcolumns = pt_i16(pt_log2_8(pt_u8((int32_t)w->c1 - w->c0)));
    el->have = true;
}

pt_ends pt_render_ends(const pt_render *r, const pt_walk *w)
{
    const pt_map *m = r->map;
    const pt_visit *vs = &r->visits[w->vi];
    pt_edgelines el = {.w = w, .sd = pt_map_side(m, vs->side),
                       .v1 = pt_map_side_v1(m, &m->cells[vs->cell], vs->side)};
    edge_ends(r, &el);
    return el.e;
}

// The side's line at height h (in slot k of its kept lines), in colour col:
// the one kept, or made from the ends, and kept if the side's to be
static pt_line edge_line(pt_render *r, pt_edgelines *el, uint8_t k, pt_height h, pt_pattern col)
{
    pt_kept *kl = &r->kept[el->kept][k];
    if (el->use)
        return (pt_line) {.x0 = kl->x0, .x1 = kl->x1, .y = kl->y, .g = kl->g, .rl = kl->rl,
                          .below = h.below, .ce = col.e, .co = col.o};
    if (!el->have)
        edge_ends(r, el);
    pt_line L = make_line(el->w->c0, el->w->c1, &el->e, h, col);
    if (el->kept != 0)
        *kl = (pt_kept) {.x0 = L.x0, .x1 = L.x1, .y = L.y, .g = L.g, .rl = L.rl};
    return L;
}

// Process the walk's current edge: its lines, and the child through it, if
// any (entered, its walk in *cw, which may be w itself for the tail call:
// true; false for none).
static bool process_edge(pt_render *r, pt_walk *w, pt_walk *cw)
{
    const pt_map *m = r->map;
    pt_visit *vs = &r->visits[w->vi];
    const pt_cell *ca = &m->cells[vs->cell];
    const pt_side *sd = pt_map_side(m, vs->side);

    // 1. the column span: from where the visit's last span ended (the
    // portal's left column for its first edge) to the edge's end, within the
    // portal. The edge never starts right of that: a walk's first side starts
    // at or left of its portal's left column, and the next where this one
    // ends, which is where its span ends if it has columns, and at or left of
    // where the last one ended if not
    RC_ASSERT(w->c0 <= w->x);
    uint8_t xa = w->x;
    uint8_t xb = w->c1 < vs->xr ? w->c1 : vs->xr;
    RC_ASSERT(xa < vs->xr);    // the last side's span is never empty: the last one ended left of xr
    RC_ASSERT(xa <= xb);       // the walk only meets sides facing the eye
    if (xa >= xb)
        return false;
    w->x = xb;

    // 2. a soft edge: no lines, and the child keeps both bounds. No cell is
    // ever met twice over the same columns: visits of a cell run left to
    // right
    const pt_edge *e = &m->edges[sd->edge];
    if (e->colour == PT_EDGE_SOFT) {
        uint8_t ci = visit_of(r, sd->adj);
        RC_ASSERT(r->visits[ci].xr <= xa);
        enter(r, ci, pt_map_twin(m, vs->side), w, xa, xb, w->tmax, w->bmin, cw);
        return true;
    }

    // the side's lines: the last visit's, to use, or, where the side runs on
    // past the portal and the pool has room, kept as they're made for the
    // next (a wall always makes its ceiling line, a portal any of them)
    uint8_t ki = vs->kept;
    bool use = ki != 0;
    if (!use && w->c1 > vs->xr && r->num_kept < PT_MAX_KEPT) {
        ki = r->num_kept++;
        vs->kept = ki;
    }
    pt_edgelines el = {.w = w, .sd = sd, .v1 = pt_map_side_v1(m, ca, vs->side), .kept = ki,
                       .use = use};
    pt_pattern wall = pt_shade(pt_u8((int32_t)e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_line l;

    if (sd->adj == 0) {
        // 3. a solid wall: its ceiling line and floor line, toggles only
        l = edge_line(r, &el, PT_K_CEIL, vs->hc, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
        l = edge_line(r, &el, PT_K_FLOOR, vs->hf, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
        return false;
    }

    uint8_t ci = visit_of(r, sd->adj);
    const pt_visit *cv = &r->visits[ci];
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
        l = edge_line(r, &el, PT_K_CEIL, vs->hc, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
    } else if (level_c && (cc.e | cc.o) != 0) {
        l = edge_line(r, &el, PT_K_CEIL, vs->hc, cc);
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
    }
    if (cb->floor_z < ca->floor_z) {
        l = edge_line(r, &el, PT_K_FLOOR, vs->hf, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
    } else if (level_f && (ff.e | ff.o) != 0) {
        l = edge_line(r, &el, PT_K_FLOOR, vs->hf, ff);
        clip_plain(r, &l, xa, xb, w->tmax, w->bmin);
    }

    // 4. the bound-forming lines write the child's bounds over the
    // parent's. The floor line goes first, building the child's bottom
    // against the parent's two; where it's above the top, the child's bottom
    // is its top. The ceiling line then builds the top against the parent's
    // top and the child's bottom, and where it's below that, the child's top
    // is its bottom. A level side has no line: the child keeps the parent's
    // bound there, and its extent. Each line gives the child its bound's
    // extent, and says whether it left the child any room
    bool room = true;    // both level: the parent's
    uint8_t cbmin = w->bmin;
    if (!level_f) {
        uint8_t k = PT_K_FLOOR;
        pt_height h = vs->hf;
        pt_pattern col = ff;
        if (cb->floor_z < ca->floor_z) {
            k = PT_K_FLOOR_ADJ;
            h = cv->hf;
            col = pt_pattern_xor(cb_floor, wall);
        }
        l = edge_line(r, &el, k, h, col);
        cbmin = clip_floor(r, &l, xa, xb, w->tmax, w->bmin, &room);
    }
    uint8_t ctmax = w->tmax;
    if (!level_c) {
        uint8_t k = PT_K_CEIL;
        pt_height h = vs->hc;
        pt_pattern col = cc;
        if (cb->ceil_z > ca->ceil_z) {
            k = PT_K_CEIL_ADJ;
            h = cv->hc;
            col = pt_pattern_xor(wall, cb_ceil);
        }
        l = edge_line(r, &el, k, h, col);
        ctmax = clip_ceil(r, &l, xa, xb, w->tmax, cbmin, &room);
    }

    // 5. the child, over the edge's whole span: where it has no room, its
    // toggles all land on its bottom row, which is its top. With no room
    // anywhere it isn't walked at all, and its stack goes in telescoped to
    // its ceiling ^ floor on the bottom rows, as a walk's toggles would
    if (!room) {
        close_columns(r, xa, xb, pt_pattern_xor(cb_ceil, cb_floor), cbmin == PT_SCREEN_HEIGHT);
        return false;
    }
    RC_ASSERT(cv->xr <= xa);    // (as for a soft edge)
    enter(r, ci, pt_map_twin(m, vs->side), w, xa, xb, ctmax, cbmin, cw);
    return true;
}

// ---- the walk ----

static void walk(pt_render *r, pt_walk *w, uint8_t depth);

// Walk a child one level deeper, or, past the depth cap, close its columns
// instead: the fix-up stands in for its whole stack, so the opening shows
// the child's ceiling colour flat - a draw-distance cutoff rather than a
// glitch.
static void descend(pt_render *r, pt_walk *cw, uint8_t depth)
{
    if (depth > PT_MAX_DEPTH) {
        const pt_visit *vs = &r->visits[cw->vi];
        const pt_cell *c = &r->map->cells[vs->cell];
        close_columns(r, cw->x, vs->xr, pt_pattern_xor(pt_shade(c->ceil), pt_shade(c->floor)),
                      cw->bmin == PT_SCREEN_HEIGHT);
        if (vs->inst != 0)
            inst_hide(r, vs->inst, cw->x, vs->xr);
        return;
    }
    walk(r, cw, depth);
}

// Walk a visit: each edge of its cell from the one its cursor is on, until
// an edge ends at or right of xr, which it does within a turn of the ring.
// A child through any earlier edge is walked recursively, and the visit
// carries on from its own column when it returns; the child through the
// last edge is walked in this same call, its walk made over this one, which
// is done with (a tail call: on the 6502 a JMP, no stack and no copy), so
// the depth grows only with portals nested along a line of sight. Over a
// child the 6502 keeps only vi, tmax and bmin: x is then the side's end's
// column, the window is the visit's, and the side's end comes back from the
// side, as a later visit's would (asserted here).
static void walk(pt_render *r, pt_walk *w, uint8_t depth)
{
    if (depth > r->depth_high)
        r->depth_high = depth;
    for (;;) {
        const pt_visit *vs = &r->visits[w->vi];
        const pt_cell *c = &r->map->cells[vs->cell];
        uint8_t sides = pt_u8((int32_t)pt_map_ring_end(c) - c->first);
        for (uint8_t steps = sides; w->c1 < vs->xr; steps--) {
            RC_ASSERT(steps != 0);
            pt_walk cw;
            if (process_edge(r, w, &cw)) {
                descend(r, &cw, pt_u8((int32_t)depth + 1));
                RC_ASSERT(w->x == w->c1);
                RC_ASSERT(w->split_angle == r->visits[w->vi].split_angle);
                RC_ASSERT(vert_to_screen_angle(r, w, pt_map_side_v1(r->map, c, vs->side)) == w->u1);
            }
            side_next(r, w);
        }
        if (!process_edge(r, w, w))
            return;
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

// Vertex v's screen angle measured from one past the view's left edge: a - vl
// - 1, in 10 bits (on the 6502, the subtract with C clear).
static uint16_t root_screen_angle(pt_vcache *vc, const pt_map *m, uint8_t v, uint16_t vl)
{
    uint16_t a = pt_vcache_vert(vc, m, v)->angle;
    return (uint16_t)((uint16_t)(a - vl - 1u) & 1023u);
}

pt_root_side pt_render_root_side(pt_vcache *vc, const pt_map *m, uint8_t cell, uint16_t vl)
{
    // the angles put the eye inside the cell (the camera found the cell by
    // them), so its sides run all the way round it, clockwise, each screen
    // angle on from the last but at the edge, where they wrap: the side the
    // edge lies on is the first whose end's screen angle is below its
    // start's. Measuring from one past the edge puts a start on the edge at
    // 1023, so it's that side, and a side of no width never is. A side's end
    // is the next one's start, and the last's is the first's
    const pt_cell *c = &m->cells[cell];
    uint8_t s = c->first;
    uint16_t start = root_screen_angle(vc, m, pt_map_side(m, s)->v, vl);
    uint16_t first = start;
    for (;; s++) {
        RC_ASSERT(s < pt_map_ring_end(c));
        uint16_t end = pt_map_ring_next(c, s) == c->first
                           ? first
                           : root_screen_angle(vc, m, pt_map_side_v1(m, c, s), vl);
        if (end < start)
            break;
        start = end;
    }
    // the window starts at the side's start: the split is its angle from the
    // edge itself, a whole turn for one on it
    return (pt_root_side) {.side = s, .split_angle = pt_u16((int32_t)start + 1)};
}

// The camera's cell's walk, begun on the root portal, the whole screen, from
// the side pt_render_root_side finds. Its bounds are the screen's edges.
static void root_walk(pt_render *r, pt_walk *w)
{
    pt_root_side rs = pt_render_root_side(r->vc, r->map, r->cam_cell, r->vl);
    uint8_t vi = visit_of(r, r->cam_cell);
    pt_visit *vs = &r->visits[vi];
    vs->side = rs.side;
    vs->xr = PT_SCREEN_WIDTH;
    vs->split_angle = rs.split_angle;
    *w = (pt_walk) {.vi = vi, .x = 0, .tmax = 0, .bmin = PT_SCREEN_HEIGHT,
                    .split_angle = rs.split_angle};
    side_start(r, w);
    vs->inst = inst_new(r, vs->cell, vs->hf);
    if (vs->inst != 0)
        inst_clip(r, vs->inst, 0, PT_SCREEN_WIDTH, 0, PT_SCREEN_HEIGHT);
}

pt_walk pt_render_root(pt_render *r, uint8_t angle, uint8_t cell)
{
    const pt_map *m = r->map;
    RC_ASSERT(cell >= 1 && cell < m->num_cells);

    // the visit cache is empty: the last capture emptied it
    r->num_visits = 1;

    r->cam_cell = cell;
    r->cam4 = (uint16_t)(angle << 2);
    r->vl = (uint16_t)((uint16_t)(r->cam4 - 128u) & 1023u);
    RC_ASSERT(m->cells[cell].floor_z >= PT_CAM_HEIGHT);
    r->eye_z = pt_u8((int32_t)m->cells[cell].floor_z - PT_CAM_HEIGHT);

    // the records, and every byte column's last toggled row: row 0 to begin
    // with, as lines running off the top toggle it unrecorded
    r->num_recs = 0;
    memset(r->toggle_end, 1, sizeof r->toggle_end);
    r->num_kept = 1;
    r->num_insts = 1;
    r->num_order = 0;
    r->clip_used = 0;

    // the root portal's bounds are the screen's edges, the top at row 0 and
    // the bottom in the junk row below: its extents say so, and top[] and
    // bot[] needn't hold them. Above row 0 is the camera cell's ceiling: the
    // fill starts each column from its colour
    pt_walk w;
    root_walk(r, &w);
    return w;
}

void pt_render_capture(pt_render *r, uint8_t angle, uint8_t cell)
{
    pt_walk w = pt_render_root(r, angle, cell);
    walk(r, &w, 1);

    // empty the visit cache while the visits are still there (on the 6502
    // they go with the capture's workspace): each clears its own cell's
    // index, so only the cells visited are touched
    for (uint8_t i = 1; i < r->num_visits; i++)
        r->cell_visit[r->visits[i].cell] = 0;
}

void pt_render_unplot(pt_render *r)
{
    unplot(r);
}

static void frame(pt_render *r, uint8_t angle, uint8_t cell)
{
    const pt_map *m = r->map;
    pt_render_capture(r, angle, cell);
    pt_mode5_fill(r->fb, r->tb, pt_shade(m->cells[cell].ceil), r->toggle_end);
    for (uint8_t k = 0; k < r->num_order; k++) {
        const pt_inst *in = &r->insts[r->order[k]];
        uint8_t width = (uint8_t)(in->cx1 - in->cx0);
        pt_sprite_clip clip = {.lo = &r->clipmem[in->clip], .hi = &r->clipmem[in->clip + width],
                               .x0 = in->cx0, .x1 = in->cx1,
                               .whole = in->whole && in->cols == width};
        pt_sprite_plot(r->fb, &m->arts[m->sprites[in->sprite].art], in->place, clip);
    }
    unplot(r);

    if (r->num_visits > r->visits_high)
        r->visits_high = r->num_visits;
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
