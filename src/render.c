// render.c - the capture walk (spec section 7), its lines clipped against
// the bounds a screen column at a time and toggled straight into the back
// buffer as they're made, and the fill. The walk finds its way by the
// angles of the cells' vertices from the eye (the vertex cache): those
// give every edge its columns, and their logs its lines' rows.
//
// A column's bounds are two rows, top[] and bot[]: the top bound's row (the
// first inside the portal) and the bottom bound's (the first below it). A
// line's toggle in a column goes at its own row clamped between them: on
// or above the top it merges into the top bound's row, on or below the
// bottom into the bottom's (spec 7.9.1: the same as plotting it on the
// bound). A line forming one of a child's bounds writes the child's rows
// over the parent's as it goes: the walk finishes a column's whole subtree
// before moving on, and never needs the parent's rows there again.

#include "render.h"

#include <stdbool.h>
#include <string.h>

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

// ---- row arithmetic ----
//
// Every u8.8 row (line_y, the plot DDA) carries the +128 rounding bias in
// its anchor, so rounding to the nearest row is a plain high-byte read
// (row_of): the first row whose middle is below the line. The clip steps a line's rows exactly as the plotter does, so it compares
// exactly the rows that are plotted.

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

// Row of a line at column c in u8.8. Where it was cut off at the screen's
// edge it is beyond every bound on that side: the furthest row there is.
static uint16_t line_y(const pt_line *l, uint8_t c)
{
    if (c < l->x0 || c >= l->x1)
        return l->below ? 0xFFFF : 0;
    return row_step(l->y, l->g, (uint8_t)(c - l->x0));
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

// An edge's end as its lines see it: its column, and the log of its depth
// (256 log2, in whole units).
typedef struct pt_end {
    uint8_t sx;
    int16_t ld;
} pt_end;

// 2^(l / 256), 0 under 1 and saturating at 0xFFFF.
static uint16_t exp2_sat(int32_t l)
{
    if (l < 0)
        return 0;
    if (l >= 16 * 256)
        return 0xFFFF;
    return pt_exp2((uint16_t)l);
}

// The rows a line's offsets run to: the screen's top or bottom edge, 32 rows
// from its middle, u8.8.
#define PT_EDGE_OFF (PT_SCREEN_HEIGHT / 2 * 256)

// The edge's line at relative height h, from its ends: each end's row is
// its offset from the screen's middle, 64 * 8h / depth, u8.8: 2^(17 + log
// h - log depth) in 256ths. The gradient is the offsets' difference over
// the columns', by their logs. An end at or past the screen's edge, nearer
// than 2h, cuts the line where it reaches that edge, (32 rows - the far
// end's offset) / g columns on from the far end; the columns cut off are
// beyond every bound on that side (line_y), and with both ends there the
// line has no columns at all.
static pt_line make_line(pt_end e0, pt_end e1, pt_relh h, pt_pattern col)
{
    RC_ASSERT(e0.sx < e1.sx);
    pt_line L = {.x0 = e0.sx, .x1 = e0.sx, .below = h.below, .ce = col.e, .co = col.o};
    int32_t l0 = -1;    // the offsets' logs, -1 for none (at the eye's height)
    int32_t l1 = -1;
    if (h.m != 0) {
        int32_t lh = (int32_t)pt_log2(h.m) + 17 * 256;
        l0 = lh - e0.ld;
        l1 = lh - e1.ld;
    }
    uint16_t o0 = exp2_sat(l0);
    uint16_t o1 = exp2_sat(l1);
    bool near0 = o0 >= PT_EDGE_OFF;
    bool near1 = o1 >= PT_EDGE_OFF;
    if (near0 && near1)
        return L;

    // the rows run away from the middle as the offset grows: down below the
    // eye, up above it
    uint8_t dc = (uint8_t)(e1.sx - e0.sx);
    uint16_t dr = o1 > o0 ? (uint16_t)(o1 - o0) : (uint16_t)(o0 - o1);
    uint8_t neg = h.below ? o1 < o0 : o1 > o0;
    int32_t lg = 0;
    uint16_t gm = 0;
    if (dr != 0) {
        if (o0 == 0xFFFF || o1 == 0xFFFF) {
            // a near end past 256 rows: the difference in whole rows instead
            // (it's then at least 224 of them)
            uint16_t w0 = exp2_sat(l0 - 8 * 256);
            uint16_t w1 = exp2_sat(l1 - 8 * 256);
            uint16_t dw = w1 > w0 ? (uint16_t)(w1 - w0) : (uint16_t)(w0 - w1);
            lg = (int32_t)pt_log2(dw) + 8 * 256 - pt_log2(dc);
        } else {
            lg = (int32_t)pt_log2(dr) - pt_log2(dc);
        }
        gm = exp2_sat(lg);
        gm = gm > 0x7FFF ? 0x7FFF : gm;
    }
    L.g = pt_g_make(gm, neg);
    uint16_t mid = PT_EDGE_OFF + 128;    // the middle row, with the rounding half
    uint16_t y0 = h.below ? (uint16_t)(mid + o0) : (uint16_t)(mid - o0);
    if (!near0 && !near1) {
        L.x1 = e1.sx;
        L.y = y0;
        return L;
    }

    // one end too near: the columns from the far end before the crossing
    // are on the screen, ceil(delta) of them counting the far end's own
    uint16_t of = near0 ? o1 : o0;
    uint16_t dq = exp2_sat((int32_t)pt_log2((uint16_t)(PT_EDGE_OFF - of)) + 8 * 256 - lg);
    uint16_t n = (uint16_t)((dq + 255u) >> 8);
    n = n > dc ? dc : n;
    if (near1) {
        L.x1 = (uint8_t)(e0.sx + n);
        L.y = y0;
    } else if (n > 1) {
        // the end's own column isn't the line's: n - 1 back from it, the row
        // g (n - 1) nearer the screen's edge
        uint8_t back = (uint8_t)(n - 1);
        uint16_t y1 = h.below ? (uint16_t)(mid + o1) : (uint16_t)(mid - o1);
        uint16_t d = pt_mulg_u16(gm, back);
        L.x0 = (uint8_t)(e1.sx - back);
        L.x1 = e1.sx;
        L.y = h.below ? (uint16_t)(y1 + d < y1 ? 0xFFFF : y1 + d) : (uint16_t)(d > y1 ? 0 : y1 - d);
    }
    return L;
}

// ---- the clip, a column at a time (spec 7.9) ----

// A stretch of a line for the plotters: columns [x0, x1) from the row at x0
// (y0, and its fraction yf with the rounding bias), stepping by g.
typedef struct pt_run {
    uint8_t x0;
    uint8_t x1;
    uint8_t y0;
    uint8_t yf;
    uint16_t g;
} pt_run;

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
static void plot_shallow(uint8_t *fb, const pt_run *p, pt_pattern pat)
{
    uint8_t gm = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as line_y
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
static void plot_steep(uint8_t *fb, const pt_run *p, pt_pattern pat)
{
    uint8_t gi = (uint8_t)(pt_g_mag(p->g) >> 8);
    uint8_t gf = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as line_y

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

// Toggle a pixel pair in a line's colour (none, for 0).
static void toggle(pt_render *r, uint8_t c, uint8_t row, pt_pattern col)
{
    if ((col.e | col.o) != 0)
        pt_mode5_plot(r->fb, c, row, col);
}

// Plot columns [x0, x1) of a line, wholly visible there, from its row y at
// x0: a byte at a time where it's shallow, a pixel pair a column where it's
// steep.
static void plot_run(pt_render *r, const pt_line *L, uint8_t x0, uint8_t x1, uint16_t y)
{
    pt_pattern pat = {.e = L->ce, .o = L->co};
    if ((pat.e | pat.o) == 0)
        return;
    pt_run p = {.x0 = x0, .x1 = x1, .y0 = (uint8_t)(y >> 8), .yf = (uint8_t)y, .g = L->g};
    if (pt_g_mag(L->g) < 0x100)
        plot_shallow(r->fb, &p, pat);
    else
        plot_steep(r->fb, &p, pat);
}

// The line's row one column on from c, whose row is y: the plotter's own
// step between two of the line's columns, else worked out (its cut-off
// columns read as past the screen's edge).
static uint16_t row_next(const pt_line *L, uint8_t c, uint16_t y)
{
    uint8_t n = (uint8_t)(c + 1);
    if (c >= L->x0 && n < L->x1)
        return step1(y, L->g);
    return line_y(L, n);
}

// Is a row strictly inside the bounds' extents, below every top and above
// every bottom? A line with both its ends so is visible all the way.
static bool inside(uint16_t y, uint8_t tmax, uint8_t bmin)
{
    uint8_t row = row_of(y);
    return row > tmax && row < bmin;
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
    if (h->x0 != 0xFF && h->row < PT_SCREEN_HEIGHT && (h->pat.e | h->pat.o) != 0) {
        pt_run p = {.x0 = h->x0, .x1 = x1, .y0 = h->row, .yf = 128, .g = 0};
        plot_shallow(r->fb, &p, h->pat);
    }
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
    if (x0 < x1 && (pat.e | pat.o) != 0) {
        pt_run p = {.x0 = x0, .x1 = x1, .y0 = 0, .yf = 128, .g = 0};
        plot_shallow(r->fb, &p, pat);
    }
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
    if (L->below ? bmin == PT_SCREEN_HEIGHT : tmax == 0) {
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
    uint16_t y = line_y(L, xa);
    if (inside(y, tmax, bmin) && inside(line_y(L, (uint8_t)(xb - 1)), tmax, bmin)) {
        plot_run(r, L, xa, xb, y);
        return;
    }
    pt_vrun v = {.x0 = 0xFF};
    pt_hrun h = {.x0 = 0xFF};
    for (uint8_t c = xa;;) {
        uint8_t row = row_of(y);
        uint8_t t = r->top[c];
        uint8_t b = r->bot[c];
        if (row > t && row < b) {
            hrun_end(r, &h, c);
            run_add(&v, c, y);
        } else {
            run_end(r, L, &v, c);
            hrun_add(r, &h, c, row <= t ? t : b, col);
        }
        if (++c == xb)
            break;
        y = row_next(L, (uint8_t)(c - 1), y);
    }
    run_end(r, L, &v, xb);
    hrun_end(r, &h, xb);
}

// The line forming the child's bottom, over [xa, xb): it closes the child
// where it's on or above the top (the child's stack, telescoped to its
// ceiling ^ floor, cbfb, goes in with it), lends the parent's bottom where
// it's on or below that, and is the bottom where it's between. Writes the
// child's bottom rows and open columns; *cbmin is the bottom's highest row.
// True when no column closed. The columns the screen's pre-clip cut off
// come first, as for clip_plain: against the screen's edge, off the bottom
// they lend the junk row and stay open, and off the top they close the
// child, on row 0.
static bool clip_floor(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                       uint8_t bmin, pt_pattern cbfb, uint8_t *cbmin)
{
    pt_pattern col = {.e = L->ce, .o = L->co};
    pt_pattern shut = pt_pattern_xor(col, cbfb);
    bool all = true;
    uint8_t lo = PT_SCREEN_HEIGHT;
    if (L->below ? bmin == PT_SCREEN_HEIGHT : tmax == 0) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        memset(&r->open[xa], L->below, (size_t)(l - xa));
        memset(&r->open[rr], L->below, (size_t)(xb - rr));
        if (!L->below && (xa < l || rr < xb)) {
            flat_top(r, xa, l, shut);
            flat_top(r, rr, xb, shut);
            all = false;
        }
        if (l == rr) {
            *cbmin = lo;
            return all;
        }
        xa = l;
        xb = rr;
    }
    uint16_t y0 = line_y(L, xa);
    uint16_t y = y0;
    bool fast = inside(y, tmax, bmin) && inside(line_y(L, (uint8_t)(xb - 1)), tmax, bmin);
    pt_vrun v = {.x0 = 0xFF};
    pt_hrun h = {.x0 = 0xFF};
    for (uint8_t c = xa;;) {
        uint8_t row = row_of(y);
        r->open[c] = 1;
        if (fast) {
            r->bot[c] = row;
        } else {
            uint8_t t = r->top[c];
            uint8_t b = r->bot[c];
            if (row <= t) {
                run_end(r, L, &v, c);
                hrun_add(r, &h, c, t, shut);
                r->open[c] = 0;
                all = false;
            } else if (row >= b) {
                run_end(r, L, &v, c);
                hrun_add(r, &h, c, b, col);
            } else {
                hrun_end(r, &h, c);
                run_add(&v, c, y);
                r->bot[c] = row;
            }
        }
        if (r->open[c] && r->bot[c] < lo)
            lo = r->bot[c];
        if (++c == xb)
            break;
        y = row_next(L, (uint8_t)(c - 1), y);
    }
    if (fast) {
        plot_run(r, L, xa, xb, y0);
    } else {
        run_end(r, L, &v, xb);
        hrun_end(r, &h, xb);
    }
    *cbmin = lo;
    return all;
}

// The line forming the child's top, over [xa, xb), after the floor line:
// it lends the parent's top where it's on or above that, closes the child
// where it's on or below the child's bottom, and is the top between. Where
// the floor line closed the child, this line is above the top as well, and
// merges into it with nothing worked out. all_open and cbmin are the floor
// line's, for the fast path. The columns the screen's pre-clip cut off the
// top come first, as for clip_plain: against the screen's top, open or
// closed, they merge into row 0.
static void clip_ceil(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                      uint8_t cbmin, bool all_open, pt_pattern cbfb)
{
    pt_pattern col = {.e = L->ce, .o = L->co};
    pt_pattern shut = pt_pattern_xor(col, cbfb);
    if (!L->below && tmax == 0) {
        uint8_t l, rr;
        on_screen(L, xa, xb, &l, &rr);
        flat_top(r, xa, l, col);
        flat_top(r, rr, xb, col);
        if (l == rr)
            return;
        xa = l;
        xb = rr;
    }
    uint16_t y0 = line_y(L, xa);
    uint16_t y = y0;
    bool fast = all_open && inside(y, tmax, cbmin)
             && inside(line_y(L, (uint8_t)(xb - 1)), tmax, cbmin);
    pt_vrun v = {.x0 = 0xFF};
    pt_hrun h = {.x0 = 0xFF};
    for (uint8_t c = xa;;) {
        uint8_t row = row_of(y);
        if (fast) {
            r->top[c] = row;
        } else if (!r->open[c]) {
            run_end(r, L, &v, c);
            hrun_add(r, &h, c, r->top[c], col);
        } else {
            uint8_t t = r->top[c];
            uint8_t b = r->bot[c];
            if (row <= t) {
                run_end(r, L, &v, c);
                hrun_add(r, &h, c, t, col);
            } else if (row >= b) {
                run_end(r, L, &v, c);
                hrun_add(r, &h, c, b, shut);
                r->open[c] = 0;
            } else {
                hrun_end(r, &h, c);
                run_add(&v, c, y);
                r->top[c] = row;
            }
        }
        if (++c == xb)
            break;
        y = row_next(L, (uint8_t)(c - 1), y);
    }
    if (fast) {
        plot_run(r, L, xa, xb, y0);
    } else {
        run_end(r, L, &v, xb);
        hrun_end(r, &h, xb);
    }
}

// Close columns [l, rr) of a child after the fact: its whole stack there
// telescopes to its ceiling ^ floor, toggled at its bottom's row, as the
// closing line's own toggle does.
static void close_columns(pt_render *r, uint8_t l, uint8_t rr, pt_pattern cbfb)
{
    for (uint8_t c = l; c < rr; c++)
        toggle(r, c, r->bot[c], cbfb);
}

// The child: the first run of open columns in [xa, xb), [*ol, *orr), empty
// when none. Every portal opening is convex on the screen (spec 7.9.4), so
// there should be one run; a crossing's rounding could pinch a sliver-thin
// opening shut in the middle, and any later run is closed after the fact.
static void open_run(pt_render *r, uint8_t xa, uint8_t xb, pt_pattern cbfb, uint8_t *ol,
                     uint8_t *orr)
{
    uint8_t c = xa;
    while (c < xb && !r->open[c])
        c++;
    *ol = c;
    while (c < xb && r->open[c])
        c++;
    *orr = c;
    for (; c < xb; c++)
        if (r->open[c])
            close_columns(r, c, (uint8_t)(c + 1), cbfb);
}

// The bounds' extents over [l, rr): the lowest top row and the highest
// bottom row, for a visit's fast path.
static void extents(const pt_render *r, uint8_t l, uint8_t rr, uint8_t *tmax, uint8_t *bmin)
{
    uint8_t hi = 0;
    uint8_t lo = PT_SCREEN_HEIGHT;
    for (uint8_t c = l; c < rr; c++) {
        hi = r->top[c] > hi ? r->top[c] : hi;
        lo = r->bot[c] < lo ? r->bot[c] : lo;
    }
    *tmax = hi;
    *bmin = lo;
}

// ---- angles and columns ----

// A world angle as a view angle in a visit's window, [lo, lo + 1024). The
// window holds the visit's cell in one piece: a cell seen through a portal
// spans under a half turn, the portal's middle among it, and its window is
// centred there; the eye's own cell spans the whole turn, and its window
// starts where the walk does (root_visit). So clockwise along the sides
// facing the eye the angles only rise, even round the back of the view.
// (On the 6502, a 10-bit subtract from the window's start, as a world
// angle, and two compares against the view's edges, both per visit.)
static int16_t window_angle(const pt_render *r, int16_t lo, uint16_t a)
{
    return (int16_t)(lo + (int16_t)((uint16_t)(a - r->cam4 - (uint16_t)lo) & 1023));
}

// The column of a view angle: in view, where its step's middle projects;
// left of the view, 0; right of it, 128.
static uint8_t col_of(int16_t u)
{
    if (u < -128)
        return 0;
    if (u >= 128)
        return PT_SCREEN_WIDTH;
    return pt_view_col((int8_t)u);
}

// An edge's ends' view angles in its visit's window, and their columns. A
// side facing the eye turns clockwise by up to a half turn, and its end is
// its start's angle plus that: in a child's window that's where the end is
// anyway, and in the eye's own cell, whose window is a whole turn, the
// last edge runs on round to the first one's start, a turn up. One facing
// away (a cell seen through a portal has a chain of them, the portal among
// them) turns back, its end at or left of its start in the window, and
// spans nothing.
typedef struct pt_cols {
    int16_t u0, u1;
    uint8_t c0, c1;
} pt_cols;

static pt_cols edge_cols(const pt_render *r, const pt_visit *vs, uint8_t k)
{
    const pt_cell *c = &r->map->cells[vs->cell];
    const pt_polar *pv = pt_vcache_cell(r->vc, r->map, vs->cell);
    uint16_t a0 = pv[k].angle;
    uint16_t a1 = pv[pt_map_ring_next(c, k)].angle;
    uint16_t d = (uint16_t)(a1 - a0) & 1023;
    int16_t u0 = window_angle(r, vs->lo, a0);
    int16_t u1 = d <= 512 ? (int16_t)(u0 + (int16_t)d) : window_angle(r, vs->lo, a1);
    return (pt_cols) {.u0 = u0, .u1 = u1, .c0 = col_of(u0), .c1 = col_of(u1)};
}

// The window of a cell seen through an edge from u0 to u1: centred on the
// edge's middle.
static int16_t child_lo(pt_cols cols)
{
    return (int16_t)(((cols.u0 + cols.u1) >> 1) - 512);
}

// ---- visits ----

// The cell's visit record, made on first need: when an edge into it is
// first clipped.
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

// The column the visit's current edge ends at.
static uint8_t end_col(const pt_render *r, const pt_visit *vs)
{
    return edge_cols(r, vs, vs->k).c1;
}

// Enter visit vi over [xl, xr), its bounds already written: the cursor goes
// to the first edge whose end is strictly right of xl (spec 7.4). A first
// walk looks from the edge after the twin (the side the walk came in by,
// at ring position twin), and sets the cell's window, lo; a later one
// resumes from the edge its last walk ended on - every earlier edge ends
// left of that walk's xr, which is at or left of xl. 0 for a degenerate
// ring (not entered).
static uint8_t enter(pt_render *r, uint8_t vi, uint8_t twin, int16_t lo, uint8_t xl, uint8_t xr,
                     uint8_t tmax, uint8_t bmin)
{
    pt_visit *vs = &r->visits[vi];
    const pt_cell *c = &r->map->cells[vs->cell];
    if (vs->xr == 0) {
        vs->k = pt_map_ring_next(c, twin);
        vs->lo = lo;
    }
    uint8_t guard = c->num;
    while (end_col(r, vs) <= xl) {
        if (guard-- == 0)
            return 0;    // degenerate ring: skip the child rather than spin
        vs->k = pt_map_ring_next(c, vs->k);
    }
    vs->x = xl;
    vs->xr = xr;
    vs->tmax = tmax;
    vs->bmin = bmin;
    return vi;
}

// ---- one edge of the walk (spec 7.8) ----

// An edge's lines are made from its ends, worked out once for all of them.
// An end in the view is its vertex: its column by angle, and its depth's
// log, the length's less lsec of its view angle. An end out of it, in
// front or behind, is where the edge crosses the view's edge, column 0 or
// 128, by Doom's clipper: the eye's distance from the edge's line, d = r
// |sin(a - w)| from an end in view (a its angle, w the edge's direction),
// over |sin| of the edge ray's angle to the line, is the distance along the
// ray, and cos 45 degrees (-128 in the log) makes it a depth. All logs: two
// log sine lookups and some adds.
typedef struct pt_edgelines {
    const pt_side *sd;
    uint8_t v1;                  // the end's vertex (the start's is the side's)
    const pt_polar *p0, *p1;     // their polar forms from the eye
    pt_cols cols;                // their view angles in the window, and columns
    bool have;
    pt_end e0, e1;
} pt_edgelines;

static void edge_ends(const pt_render *r, pt_edgelines *el)
{
    const pt_map *m = r->map;
    int16_t u0 = el->cols.u0;
    int16_t u1 = el->cols.u1;
    bool in0 = u0 >= -128;
    bool in1 = u1 < 128;
    if (in0)
        el->e0 = (pt_end) {.sx = el->cols.c0,
                           .ld = (int16_t)((int16_t)el->p0->lr - pt_lsec((int8_t)u0))};
    if (in1)
        el->e1 = (pt_end) {.sx = el->cols.c1,
                           .ld = (int16_t)((int16_t)el->p1->lr - pt_lsec((int8_t)u1))};
    if (!in0 || !in1) {
        // the edge's direction: the quadrant from its vertices, and its byte
        pt_vertex a = m->verts[el->sd->v];
        pt_vertex b = m->verts[el->v1];
        uint8_t q = pt_quadrant((int16_t)(b.x - a.x), (int16_t)(b.y - a.y));
        uint16_t w = (uint16_t)(q << 8 | m->edges[el->sd->edge].angle);
        const pt_polar *pk = in1 ? el->p1 : el->p0;
        uint16_t aw = (uint16_t)(pk->angle - w);
        int16_t ls = (int16_t)((pt_logsin((uint16_t)(aw - 1)) + pt_logsin(aw)) >> 1);
        int16_t ld = (int16_t)((int16_t)pk->lr + ls - 128);
        if (!in0)
            el->e0 = (pt_end) {.sx = 0,
                               .ld = (int16_t)(ld - pt_logsin((uint16_t)(r->cam4 - 129 - w)))};
        if (!in1)
            el->e1 = (pt_end) {.sx = PT_SCREEN_WIDTH,
                               .ld = (int16_t)(ld - pt_logsin((uint16_t)(r->cam4 + 127 - w)))};
    }
    el->have = true;
}

static pt_line edge_line(const pt_render *r, pt_edgelines *el, pt_relh h, pt_pattern col)
{
    if (!el->have)
        edge_ends(r, el);
    if (el->e0.sx >= el->e1.sx)    // edge-on
        return (pt_line) {.x0 = el->e0.sx, .x1 = el->e0.sx, .below = h.below, .ce = col.e, .co = col.o};
    return make_line(el->e0, el->e1, h, col);
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
    pt_cols cols = edge_cols(r, vs, vs->k);
    uint8_t xa = cols.c0 > vs->x ? cols.c0 : vs->x;
    uint8_t xb = cols.c1 < vs->xr ? cols.c1 : vs->xr;
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
        return enter(r, ci, sd->twin, child_lo(cols), xa, xb, vs->tmax, vs->bmin);
    }

    const pt_polar *pv = pt_vcache_cell(r->vc, m, vs->cell);
    pt_edgelines el = {.sd = sd, .v1 = pt_map_side_v1(m, ca, vs->k), .p0 = &pv[vs->k],
                       .p1 = &pv[pt_map_ring_next(ca, vs->k)], .cols = cols};
    pt_pattern wall = pt_shade((uint8_t)(e->colour - 1));
    pt_pattern ca_ceil = pt_shade(ca->ceil);
    pt_pattern ca_floor = pt_shade(ca->floor);
    pt_relh ha_c = rel_h(ca->ceil_z, r->eye_z);
    pt_relh ha_f = rel_h(ca->floor_z, r->eye_z);
    pt_line l;

    if (sd->adj == 0) {
        // 3. a solid wall: its ceiling line and floor line, toggles only
        l = edge_line(r, &el, ha_c, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
        l = edge_line(r, &el, ha_f, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
        return 0;
    }

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
        l = edge_line(r, &el, ha_c, pt_pattern_xor(ca_ceil, wall));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    } else if (level_c && (cc.e | cc.o) != 0) {
        l = edge_line(r, &el, ha_c, cc);
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    }
    if (cb->floor_z < ca->floor_z) {
        l = edge_line(r, &el, ha_f, pt_pattern_xor(wall, ca_floor));
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    } else if (level_f && (ff.e | ff.o) != 0) {
        l = edge_line(r, &el, ha_f, ff);
        clip_plain(r, &l, xa, xb, vs->tmax, vs->bmin);
    }

    // 4. the bound-forming lines write the child's bounds over the
    // parent's. The floor line goes first, building the child's bottom
    // against the parent's two; it closes the child where it is above the
    // top. The ceiling line then builds the top against the parent's top and
    // the child's bottom; where the floor line closed the child, it merges
    // into the top with nothing worked out. A level side has no line: the
    // child keeps the parent's bound there
    pt_pattern cbfb = pt_pattern_xor(cb_ceil, cb_floor);
    bool all_open = true;
    uint8_t cbmin = vs->bmin;
    if (level_f) {
        memset(&r->open[xa], 1, (size_t)(xb - xa));
    } else {
        pt_relh h = ha_f;
        pt_pattern col = ff;
        if (cb->floor_z < ca->floor_z) {
            h = rel_h(cb->floor_z, r->eye_z);
            col = pt_pattern_xor(cb_floor, wall);
        }
        l = edge_line(r, &el, h, col);
        all_open = clip_floor(r, &l, xa, xb, vs->tmax, vs->bmin, cbfb, &cbmin);
    }
    if (!level_c) {
        pt_relh h = ha_c;
        pt_pattern col = cc;
        if (cb->ceil_z > ca->ceil_z) {
            h = rel_h(cb->ceil_z, r->eye_z);
            col = pt_pattern_xor(wall, cb_ceil);
        }
        l = edge_line(r, &el, h, col);
        clip_ceil(r, &l, xa, xb, vs->tmax, cbmin, all_open, cbfb);
    }

    // 5. the child: the run of columns the two lines left open. A cell
    // already walked over them (the eye on a vertex) isn't entered again,
    // and its run is closed instead
    uint8_t ol, orr;
    open_run(r, xa, xb, cbfb, &ol, &orr);
    if (ol >= orr)
        return 0;
    uint8_t ci = visit_of(r, sd->adj);
    if (r->visits[ci].xr > ol) {
        close_columns(r, ol, orr, cbfb);
        return 0;
    }
    uint8_t tmax, bmin;
    extents(r, ol, orr, &tmax, &bmin);
    return enter(r, ci, sd->twin, child_lo(cols), ol, orr, tmax, bmin);
}

// ---- the walk (spec 7.5) ----

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
        close_columns(r, vs->x, vs->xr, pt_pattern_xor(pt_shade(c->ceil), pt_shade(c->floor)));
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
        uint8_t steps = (uint8_t)(c->num + 2);
        for (;;) {
            if (steps-- == 0)
                return;
            bool last = end_col(r, vs) >= vs->xr;
            uint8_t ci = process_edge(r, vi);
            if (last) {
                if (ci == 0)
                    return;
                vi = ci;    // the tail call
                break;
            }
            if (ci != 0)
                descend(r, ci, (uint8_t)(depth + 1));
            vs->k = pt_map_ring_next(c, vs->k);
        }
    }
}

// ---- the frame (spec 7.1, 7.4) ----

// The camera's cell's visit, begun on the root portal, the whole screen: its
// first edge is the side whose angles straddle the view's left edge (-128),
// starting at or before it and ending after it, and its window starts
// there. The angles put the eye inside the cell, so its sides run all the
// way round it and one does; should rounding leave none, the walk starts
// from the ring's first side in the plain view window, and ends by its
// steps.
static uint8_t root_visit(pt_render *r)
{
    const pt_map *m = r->map;
    const pt_cell *c = &m->cells[r->cam_cell];
    const pt_polar *pv = pt_vcache_cell(r->vc, m, r->cam_cell);
    uint8_t root = 0;
    int16_t lo = -512;
    uint16_t left = (uint16_t)(r->cam4 - 128);
    for (uint8_t k = 0; k <= c->num; k++) {
        uint16_t a0 = pv[k].angle;
        uint16_t a1 = pv[pt_map_ring_next(c, k)].angle;
        // the turn from the start to the view's left edge, within the side's
        uint16_t off = (uint16_t)(left - a0) & 1023;
        if (off < ((uint16_t)(a1 - a0) & 1023)) {
            root = k;
            lo = (int16_t)(-128 - (int16_t)off);
            break;
        }
    }
    uint8_t vi = visit_of(r, r->cam_cell);
    pt_visit *vs = &r->visits[vi];
    vs->k = root;
    vs->lo = lo;
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

    // the cell the angles put the eye in (the camera's own already is)
    cell = pt_vcache_locate(r->vc, m, cell);
    r->cam_cell = cell;
    r->cam4 = (uint16_t)(angle << 2);
    RC_ASSERT(m->cells[cell].floor_z >= PT_CAM_HEIGHT);
    r->eye_z = (uint8_t)(m->cells[cell].floor_z - PT_CAM_HEIGHT);

    pt_mode5_clear(r->fb);
    r->num_visits = 1;

    // the root portal's bounds: the top at row 0, under the strip of the
    // camera cell's ceiling colour that seeds the fill, and the bottom off
    // the screen, in the junk row below it
    memset(r->top, 0, sizeof r->top);
    memset(r->bot, PT_SCREEN_HEIGHT, sizeof r->bot);
    pt_run strip = {.x0 = 0, .x1 = PT_SCREEN_WIDTH, .y0 = 0, .yf = 128, .g = 0};
    plot_shallow(r->fb, &strip, pt_shade(m->cells[cell].ceil));

    walk(r, root_visit(r), 1);
    pt_mode5_fill(r->fb);

    if (r->num_visits > r->visits_high)
        r->visits_high = r->num_visits;

    // empty the visit cache: each record clears its own cell's index
    for (uint8_t i = 1; i < r->num_visits; i++)
        r->cell_visit[r->visits[i].cell] = 0;
}

void pt_render_init(pt_render *r, const pt_map *m)
{
    *r = (pt_render) {0};
    r->map = m;
    r->fb = r->fbmem + PT_FB_GUARD;
    r->sprites = true;
    pt_vcache_reset(&r->own_vc, 0, 0);
}

void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell)
{
    pt_vcache_move(&r->own_vc, cam_x, cam_y);
    r->vc = &r->own_vc;
    r->cam_x = cam_x;
    r->cam_y = cam_y;
    frame(r, angle, cell);
}

void pt_render_view(pt_render *r, pt_vcache *vc, uint8_t angle, uint8_t cell)
{
    r->vc = vc;
    r->cam_x = vc->x;
    r->cam_y = vc->y;
    frame(r, angle, cell);
}
