// sprite.c - spritescale's art format, and its two plotters, the stretch
// engine and the per-pair rebuild, clipped to rows per screen column.
//
// A sprite goes up the screen a strip at a time, a strip being a byte
// column: four pixel slots, each showing one source column (or nothing).
// Each column's runs come one at a time, bottom up, as the pairs reach their
// ends. That much is spritescale. What it doesn't do is clip top and
// bottom, and here every pixel column has its own window, [lo, hi), as the
// portals it's seen through left it: ragged, along a sloping bound, even
// inside one zoomed source column.
//
// So each strip is classed once, from its slots' windows:
//   hidden   no slot shows any of the sprite: not plotted (its columns are
//            still walked);
//   clean    every slot shows all of it: spritescale's own code, as it is;
//   clipped  the rest: the same stretch loops, but the events that drive
//            them include the clip's. A slot opens at the bottom of its
//            window and closes at its top, and while it's shut it's out of
//            the masks, so the loops keep the background there. The strip
//            runs from the lowest pair any slot shows to the highest.
// Rows are counted in pairs up from the screen's bottom (pair p is row
// 63 - p), every source row's edge clamped to the screen, so the screen's
// own edges clip like the portals', and a sprite can stand on a floor below
// the screen and run up past its top.

#include "sprite.h"

#include <stdbool.h>
#include <string.h>

#include "arith.h"
#include "palette.h"
#include "richc/macros.h"

// ---- the format ----

// The crossover: the scale below which the per-pair rebuild costs less
// than the stretch engine. The rebuild pays for every pair of a strip's
// four slots, filled or not; the engine for its columns, their runs' ends
// and the stretches between. So each plotter's work on the art is counted
// at every scale under 1x, as the 6502 does it, the sprite whole on the
// screen at each of a byte's four pixel alignments, and priced in cycles:
// prices fitted to the 6502 plotters on the cycle-exact model (159 arts,
// every eighth scale under 1x, 2026-10-15; within 2% of their cycles on
// average). The crossover is the scale that makes the sum over all the
// scales least, the rebuild under it and the engine from it: 0 if the
// engine always wins, 255 if the rebuild does.

// What a plotter does at a scale, over its strips.
typedef struct pt_sprite_work {
    uint16_t strips;                    // strips, and their columns tracked
    uint16_t tracked;
    uint16_t masked, solid, skipped;    // pairs: some slots keeping the
                                        // background, none, all
    uint16_t fetches, swallows;         // runs taken, and runs shrunk away
    uint16_t calls;                     // the engine's handler calls, times
                                        // its columns
    uint16_t masked_stretches, solid_stretches, skipped_stretches;
} pt_sprite_work;

// Each thing's price in cycles, by plotter: 0 for what a plotter doesn't
// do. The sprite, its rows and its columns are the setup's, which each
// plotter walks on with its own share.
typedef struct pt_sprite_prices {
    uint16_t sprite, row, column;
    uint16_t strip, tracked, masked, solid, skipped, fetch, swallow, call;
    uint16_t masked_stretch, solid_stretch, skipped_stretch;
} pt_sprite_prices;

static const pt_sprite_prices rebuild_prices = {
    .sprite = 262, .row = 35, .column = 37, .strip = 261, .masked = 123, .solid = 108,
    .skipped = 63, .fetch = 66, .swallow = 31};
static const pt_sprite_prices engine_prices = {
    .sprite = 348, .row = 32, .column = 44, .strip = 76, .tracked = 71, .masked = 50, .solid = 31,
    .fetch = 92, .swallow = 82, .call = 15, .masked_stretch = 27, .solid_stretch = 39,
    .skipped_stretch = 12};

static uint32_t price(const pt_sprite_prices *p, const pt_sprite_work *k, uint8_t w, uint8_t h)
{
    return p->sprite + (uint32_t)p->row * h + (uint32_t)p->column * w +
           (uint32_t)p->strip * k->strips + (uint32_t)p->tracked * k->tracked +
           (uint32_t)p->masked * k->masked + (uint32_t)p->solid * k->solid +
           (uint32_t)p->skipped * k->skipped + (uint32_t)p->fetch * k->fetches +
           (uint32_t)p->swallow * k->swallows + (uint32_t)p->call * k->calls +
           (uint32_t)p->masked_stretch * k->masked_stretches +
           (uint32_t)p->solid_stretch * k->solid_stretches +
           (uint32_t)p->skipped_stretch * k->skipped_stretches;
}

// The work at scale s (under 1x, so a column is a pixel at most), the
// first strip's first x slots padding; false if nothing's on the screen,
// when neither plotter runs. The row edges are setup's, the bottom pair 0;
// the column walk findcol's.
static bool work(const char *const rows[], uint8_t w, uint8_t h, uint8_t s, uint8_t x,
                 pt_sprite_work *k)
{
    uint8_t py[PT_SPRITE_MAX_ROWS + 1];
    for (uint8_t i = 0; i <= h; i++)
        py[i] = (uint8_t)((128u + (uint16_t)(i * s)) >> 8);
    uint8_t top = py[h];
    uint8_t slots[3 + PT_SPRITE_MAX_COLS];    // each slot's column, w for padding
    uint8_t n = 0;
    while (n < x)
        slots[n++] = w;
    uint16_t xacc = 128;
    for (uint8_t c = 0; c < w; c++) {
        uint16_t next = (uint16_t)(xacc + s);
        if ((next >> 8) != (xacc >> 8))
            slots[n++] = c;
        xacc = next;
    }
    if (top == 0 || n == x)
        return false;
    *k = (pt_sprite_work) {0};
    for (uint8_t st = 0; st < n; st = (uint8_t)(st + 4)) {
        uint8_t code[4][PT_SCREEN_HEIGHT] = {{0}};    // each slot's colour by pair, 0 if none
        bool ends[PT_SCREEN_HEIGHT] = {false};         // the run ends: the engine's events
        uint8_t tracked = 0;
        for (uint8_t i = 0; i < 4 && st + i < n; i++) {
            uint8_t c = slots[st + i];
            if (c == w)
                continue;
            tracked++;
            for (uint8_t r = 0, first = 0; r < h; r++) {
                char ch = rows[h - 1 - r][c];
                if (r != h - 1 && ch == rows[h - 2 - r][c])
                    continue;
                if (py[first] == py[r + 1]) {
                    k->swallows++;
                } else {
                    k->fetches++;
                    if (py[r + 1] < top)
                        ends[py[r + 1]] = true;
                }
                memset(&code[i][py[first]], ch - '0', (size_t)(py[r + 1] - py[first]));
                first = (uint8_t)(r + 1);
            }
        }
        k->strips++;
        k->tracked = (uint16_t)(k->tracked + tracked);
        for (uint8_t p = 0; p < top; p++) {
            bool any = code[0][p] || code[1][p] || code[2][p] || code[3][p];
            bool all = code[0][p] && code[1][p] && code[2][p] && code[3][p];
            bool begins = p == 0 || ends[p];
            if (begins)
                k->calls = (uint16_t)(k->calls + tracked);
            if (all) {
                k->solid++;
                k->solid_stretches = (uint16_t)(k->solid_stretches + begins);
            } else if (any) {
                k->masked++;
                k->masked_stretches = (uint16_t)(k->masked_stretches + begins);
            } else {
                k->skipped++;
                k->skipped_stretches = (uint16_t)(k->skipped_stretches + begins);
            }
        }
    }
    return true;
}

static uint8_t crossover(const char *const rows[], uint8_t w, uint8_t h)
{
    uint32_t rebuild[256] = {0}, engine[256] = {0};
    uint32_t total = 0;    // all the engine's: the sum with the crossover 0
    for (unsigned s = 1; s < 256; s++) {
        for (uint8_t x = 0; x < 4; x++) {
            pt_sprite_work k;
            if (!work(rows, w, h, (uint8_t)s, x, &k))
                continue;
            rebuild[s] += price(&rebuild_prices, &k, w, h);
            engine[s] += price(&engine_prices, &k, w, h);
        }
        total += engine[s];
    }
    uint32_t least = total;
    uint8_t t = 0;
    for (unsigned s = 1; s < 256; s++) {
        total = total - engine[s] + rebuild[s];    // the rebuild under s + 1
        if (total < least) {
            least = total;
            t = (uint8_t)(s == 255 ? 255 : s + 1);
        }
    }
    return t;
}

void pt_sprite_encode(pt_sprite_art *a, const char *const rows[], const uint8_t pal[7])
{
    uint8_t w = (uint8_t)strlen(rows[0]);
    uint8_t h = 0;
    while (h < PT_SPRITE_MAX_ROWS && rows[h] != NULL)
        h++;
    RC_ASSERT(w >= 1 && w <= PT_SPRITE_MAX_COLS && h >= 1);
    RC_ASSERT(h == PT_SPRITE_MAX_ROWS || rows[h] == NULL);
    for (uint8_t r = 0; r < h; r++)
        RC_ASSERT(strlen(rows[r]) == w);

    memcpy(a->pal, pal, sizeof a->pal);
    a->pats[0] = (pt_pattern) {0, 0};
    for (uint8_t i = 1; i < 8; i++)
        a->pats[i] = pt_shade(pal[i - 1]);
    a->data[0] = w;
    a->data[1] = h;
    a->data[2] = crossover(rows, w, h);
    uint16_t n = (uint16_t)(3 + w);
    for (uint8_t c = 0; c < w; c++) {
        RC_PANIC(n < PT_SPRITE_MAX_BYTES);
        a->data[3 + c] = (uint8_t)n;
        // a run ends on every row whose colour differs from the one above,
        // and on the top row
        for (uint8_t i = 0; i < h; i++) {
            char code = rows[h - 1 - i][c];
            RC_ASSERT(code >= '0' && code <= '7');
            if (i == h - 1 || code != rows[h - 2 - i][c]) {
                RC_PANIC(n < PT_SPRITE_MAX_BYTES);
                a->data[n++] = (uint8_t)((i << 3) | (code - '0'));
            }
        }
    }
}

uint8_t pt_sprite_code(const pt_sprite_art *a, uint8_t col, uint8_t row)
{
    RC_ASSERT(col < pt_sprite_width(a) && row < pt_sprite_height(a));
    const uint8_t *p = &a->data[a->data[3 + col]];
    while ((*p >> 3) < row)
        p++;
    return *p & 7;
}

// ---- setting up ----

static const uint8_t slotbits_[4] = {0x88, 0x44, 0x22, 0x11};

// plot_setup's results.
typedef struct pt_sprite_setup {
    const uint8_t *spr;
    const pt_pattern *pal;                 // the art's patterns
    uint8_t wid, hgt;
    uint16_t scale;
    uint8_t py[PT_SPRITE_MAX_ROWS + 1];    // each source row's bottom edge, a pair
    uint8_t ph;                            // py[hgt]: the top edge
    uint8_t k0x8;                          // 8 x the first source row on the screen:
                                           // a run byte under it ends under the screen
    uint8_t sx0;                           // the first on-screen pixel column
    uint8_t strips;                        // byte columns from there to the right edge
    uint8_t c0, rem0;                      // the column walk's start: the first
    uint8_t xf0;                           // column with pixels on screen, those
                                           // pixels, and its edge's fraction
} pt_sprite_setup;

// plot_setup: the row edges, and where the column walk starts; false when
// nothing is on screen. Each edge is spritescale's, round(k * scale) up
// from the bottom pair (the accumulator starts half a pair up), as a pair of
// the screen's and clamped to it, 0..64: rows below the screen end at 0,
// rows above it at 64. On the 6502 the loop goes in three parts: under the
// screen, a three-byte add (the bottom pair is signed, and a close sprite's
// rows run to thousands of pairs) storing 0s, and counting k0; on it, a
// two-byte add, since the top byte is 0 and the pair under 64 so nothing
// carries out, and CMP #64 : BCS top leaves the carry clear for the next
// ADC; past the top, 64s. Only the column edge's fraction is kept, where
// spritescale keeps 16 bits: a column's pixels are the scale's whole part,
// and 1 more when the fraction carries. The walk is spritescale's poscol,
// the pixels hanging off the left 16 bits.
static bool setup(pt_sprite_setup *s, const pt_sprite_art *a, pt_sprite_place p)
{
    s->spr = a->data;
    s->pal = a->pats;
    s->wid = a->data[0];
    s->hgt = a->data[1];
    s->scale = p.scale;

    int32_t acc = (int32_t)p.y * 256 + 128;
    uint8_t k0 = 0;
    for (uint8_t k = 0; k <= s->hgt; k++) {
        int32_t pr = acc >> 8;
        s->py[k] = pr < 0 ? (uint8_t)0 : pr > PT_SCREEN_HEIGHT ? (uint8_t)PT_SCREEN_HEIGHT : (uint8_t)pr;
        if (k != 0 && s->py[k] == 0)
            k0 = k;    // row k - 1 ends under the screen
        acc += p.scale;
    }
    s->k0x8 = (uint8_t)(k0 << 3);
    s->ph = s->py[s->hgt];
    if (s->py[0] == s->ph || p.x >= PT_SCREEN_WIDTH)
        return false;    // wholly above, below or right of the screen

    uint16_t lclip = 0;
    s->sx0 = 0;
    if (p.x < 0)
        lclip = pt_u16(-(int32_t)p.x);
    else
        s->sx0 = (uint8_t)p.x;
    s->strips = (uint8_t)(PT_SCREEN_WIDTH / 4 - (s->sx0 >> 2));

    // the first column with pixels on screen: past the shrunk away, and
    // those hanging off the left, whole columns and then part of one
    uint8_t c = 0;
    uint8_t xf = 128;
    uint8_t rem = 0;
    for (;; c++) {
        if (c == s->wid)
            return false;    // every column shrunk or clipped away
        uint16_t edge = (uint16_t)(xf + (p.scale & 255));
        xf = (uint8_t)edge;
        rem = (uint8_t)((p.scale >> 8) + (edge >> 8));
        if (rem == 0)
            continue;
        if (lclip == 0)
            break;
        if (lclip < rem) {
            rem = (uint8_t)(rem - lclip);
            break;
        }
        lclip = (uint16_t)(lclip - rem);
    }
    s->c0 = c;
    s->rem0 = rem;
    s->xf0 = xf;
    return true;
}

// The column walk's state.
typedef struct pt_sprite_walk {
    uint8_t c;       // the source column being assigned
    uint8_t rem;     // its pixels not yet assigned
    uint8_t xf;      // its right edge's fraction
    uint8_t slot;    // the slot being filled, 0..3
} pt_sprite_walk;

// findcol: column c's width from its edge, a zero-width column (shrunk
// away) passed for the next; false when the sprite runs out first.
static bool findcol(const pt_sprite_setup *s, pt_sprite_walk *w)
{
    for (; w->c != s->wid; w->c++) {
        uint16_t edge = (uint16_t)(w->xf + (s->scale & 255));
        w->xf = (uint8_t)edge;
        w->rem = (uint8_t)((s->scale >> 8) + (edge >> 8));
        if (w->rem != 0)
            return true;
    }
    return false;
}

// A column's first run on the screen: the raw run bytes compared with 8 k0,
// a run's last row being its top five bits, no ends looked up:
//   LDA (spr),Y : CMP k0x8 : BCS done : INY : BNE loop
// 15 cycles a run passed, and only a sprite standing below the screen has
// any. The handlers then never meet a run ending under the strip.
static uint8_t onscreen_run(const pt_sprite_setup *s, uint8_t idx)
{
    while (s->spr[idx] < s->k0x8)
        idx++;
    return idx;
}

// The run over pair p: past those ending at or below it, their ends looked
// up. A clipped engine strip's start.
static uint8_t run_over(const pt_sprite_setup *s, uint8_t idx, uint8_t p)
{
    while (s->py[(s->spr[idx] >> 3) + 1] <= p)
        idx++;
    return idx;
}

// ---- classing a strip ----

#define PT_STRIP_HIDDEN 0
#define PT_STRIP_CLEAN 1
#define PT_STRIP_CLIPPED 2

// A strip's windows: each covered slot's, clamped to the sprite's pairs,
// and the pairs the strip runs over.
typedef struct pt_sprite_win {
    uint8_t lo[4], hi[4];
    uint8_t start, end;
} pt_sprite_win;

// Class a strip. On the 6502, two compares a slot against the sprite's
// ends, and for a clipped strip, the min and max. A sprite whose every
// column shows all of it (the clip's whole) is clean throughout, and isn't
// classed at all.
static uint8_t classify(const pt_sprite_setup *s, pt_sprite_clip clip, uint8_t bc,
                        uint8_t covered, pt_sprite_win *wn)
{
    if (clip.whole) {
        wn->start = s->py[0];
        wn->end = s->ph;
        return PT_STRIP_CLEAN;
    }
    bool clean = true;
    wn->start = PT_SCREEN_HEIGHT;
    wn->end = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if ((covered & slotbits_[i]) == 0)
            continue;
        uint8_t x = (uint8_t)(bc * 4 + i);
        RC_ASSERT(x >= clip.x0 && x < clip.x1);
        uint8_t cl = clip.lo[x - clip.x0];
        uint8_t ch = clip.hi[x - clip.x0];
        uint8_t lo = cl > s->py[0] ? cl : s->py[0];
        uint8_t hi = ch < s->ph ? ch : s->ph;
        wn->lo[i] = lo;
        wn->hi[i] = hi;
        if (lo != s->py[0] || hi != s->ph)
            clean = false;
        if (lo < hi) {
            if (lo < wn->start)
                wn->start = lo;
            if (hi > wn->end)
                wn->end = hi;
        }
    }
    if (wn->start >= wn->end)
        return PT_STRIP_HIDDEN;
    return clean ? PT_STRIP_CLEAN : PT_STRIP_CLIPPED;
}

// ---- the stretch ----

// Pairs [from, to) of byte column bc from the composed state, bottom up:
// with every slot opaque, a plain store (solidloop); with none, nothing
// (skiploop); else read, mask, or, write (pairloop). Two 6502 savings on
// spritescale's loops: a solid stretch whose even and odd bytes match (a
// colour of one shade, black or red) stores one byte twice,
//   STA (scr),Y : DEY : STA (scr),Y : DEY : BMI adjust : DEX : BNE
// 23 cycles a pair against 29, picked once a stretch (LDA data_odd : CMP
// data_even : BNE solidloop); and a skip needn't step the pointer at all,
// as pairs are counted from the screen's bottom: the next pair's cursor
// comes from the pair, its odd scanline s = 127 - 2p at s AND 7 in block
// s >> 3 (ASL A : EOR #127 : TAX : AND #7 : TAY : LDA shr3,X : ORA base),
// about 23 cycles against skiploop's 41, the same placing each strip's
// start.
static void stretch(uint8_t *fb, uint8_t bc, uint8_t from, uint8_t to, uint8_t mask, uint8_t de,
                    uint8_t dodd)
{
    if (mask == 0xFF || from == to)
        return;
    pt_mode5_cur cur = pt_mode5_at(bc, (uint8_t)(PT_SCREEN_HEIGHT - 1 - from));
    for (uint8_t p = from; p != to; p++) {
        int16_t a = (int16_t)(cur.blk + cur.y);
        RC_ASSERT(a >= 0 && a < PT_FB_BYTES - 1);
        if (mask == 0) {
            fb[a] = de;
            fb[a + 1] = dodd;
        } else {
            fb[a] = (uint8_t)((fb[a] & mask) | de);
            fb[a + 1] = (uint8_t)((fb[a + 1] & mask) | dodd);
        }
        pt_mode5_up(&cur);
    }
}

// ---- the stretch engine ----

// A tracked column.
typedef struct pt_sprite_track {
    uint8_t idx;        // its next run byte
    uint8_t cp;         // the pair its current run ends at
    uint8_t colmask;    // the slots it shows, less any the clip has shut
} pt_sprite_track;

typedef struct pt_sprite_engine {
    const pt_sprite_setup *s;
    pt_sprite_track t[4];
    uint8_t n;          // tracked columns
    uint8_t own[4];     // each slot's tracked column
    uint8_t mask;       // the composed state: the slots keeping the background,
    uint8_t de, dodd;   // and the even and odd scanlines' data
    uint8_t cnt;        // the pair reached
} pt_sprite_engine;

// Write a colour into slots cm of the composed state, as spritescale's
// changecol: x = x EOR ((x EOR new) AND cm) into the data, and the mask's
// slots cleared; a transparent one clears the data and keeps the
// background. (The 6502's one-column handlers skip the merges: with no
// other column to keep, data = pattern AND cm, and mask = the strip's
// uncovered slots, or &FF.)
static void merge(pt_sprite_engine *e, uint8_t cm, uint8_t code)
{
    if (code == 0) {
        e->de = (uint8_t)((e->de | cm) ^ cm);
        e->dodd = (uint8_t)((e->dodd | cm) ^ cm);
        e->mask |= cm;
    } else {
        pt_pattern pat = e->s->pal[code];
        e->de = (uint8_t)(e->de ^ ((e->de ^ pat.e) & cm));
        e->dodd = (uint8_t)(e->dodd ^ ((e->dodd ^ pat.o) & cm));
        e->mask = (uint8_t)((e->mask | cm) ^ cm);
    }
}

// Column k's next run, merged in under the slots it shows. Every run a
// strip fetches ends above the pair it's fetched at, except, under 1x, one
// shrunk away (ending at it), which the shrink handlers swallow: CMP cnt :
// BEQ fetch. The zoom handlers have none to swallow.
static void fetch(pt_sprite_engine *e, uint8_t k)
{
    pt_sprite_track *t = &e->t[k];
    uint8_t b;
    do {
        b = e->s->spr[t->idx];
        t->idx = (uint8_t)(t->idx + 1u);
        t->cp = e->s->py[(b >> 3) + 1];
    } while (e->s->scale < 256 && t->cp == e->cnt);
    RC_ASSERT(t->cp > e->cnt);
    merge(e, t->colmask, (uint8_t)(b & 7));
}

// The nearest event: the lowest run end, at most lim.
static uint8_t nearest(const pt_sprite_engine *e, uint8_t lim)
{
    for (uint8_t k = 0; k < e->n; k++)
        if (e->t[k].cp < lim)
            lim = e->t[k].cp;
    return lim;
}

// A clipped strip's next event: the nearest slot's, the last of those at
// the same pair (the 6502's CMP : BCC), or the strip's end if none.
static uint8_t nearest_event(const uint8_t ev[4], uint8_t end, uint8_t *slot)
{
    for (uint8_t i = 0; i < 4; i++) {
        if (ev[i] <= end) {
            end = ev[i];
            *slot = i;
        }
    }
    return end;
}

// The composed state empty, and every column's first run fetched at the
// strip's first pair (its cp there).
static void strip_begin(pt_sprite_engine *e, uint8_t start)
{
    e->cnt = start;
    e->mask = 0xFF;
    e->de = 0;
    e->dodd = 0;
    for (uint8_t k = 0; k < e->n; k++)
        e->t[k].cp = start;
}

// A clean strip, spritescale's: each change applied, then a stretch of
// pairs to the next, until the top. On the 6502 the change handler is
// specialised by the tracked count (changes1..4), the shrink set (schanges)
// swallowing. A sprite standing below the screen has its columns' runs
// under it passed by first.
static void strip_engine(uint8_t *fb, pt_sprite_engine *e, uint8_t bc, const pt_sprite_win *wn)
{
    for (uint8_t k = 0; k < e->n; k++)
        e->t[k].idx = onscreen_run(e->s, e->t[k].idx);
    strip_begin(e, wn->start);
    for (;;) {
        for (uint8_t k = 0; k < e->n; k++)
            if (e->t[k].cp == e->cnt)
                fetch(e, k);
        uint8_t next = nearest(e, wn->end);
        stretch(fb, bc, e->cnt, next, e->mask, e->de, e->dodd);
        e->cnt = next;
        if (next == wn->end)
            return;
    }
}

// A clipped strip: a clean strip's handlers, with the clip's events among
// the runs' ends. Each column starts on the run over the strip's first pair
// (run_over). Its slots open, their columns' current colours merged in, at
// the bottoms of their windows, and close, blanked, at the tops; those shut
// to begin with are taken out of their columns' masks, so the runs never
// write them. Each slot keeps its next event (its opening, then its
// closing, but not one at the strip's end, which would never come and
// would only slow the 6502's search), and the nearest is a fifth value in
// the handler's min, the strip's end its sentinel; at stretchdone, CMP
// nextev rather than ph. An event goes before a run change at the same pair
// (they commute), and an opening reads its column's colour from the run
// byte before idx: no colour kept a column.
static void strip_engine_clip(uint8_t *fb, pt_sprite_engine *e, uint8_t bc, uint8_t covered,
                              const pt_sprite_win *wn)
{
    uint8_t ev[4];    // each slot's next event's pair, 0xFF if none
    for (uint8_t i = 0; i < 4; i++) {
        ev[i] = 0xFF;
        if ((covered & slotbits_[i]) == 0)
            continue;
        uint8_t lo = wn->lo[i];
        uint8_t hi = wn->hi[i];
        if (lo >= hi || lo > wn->start)
            e->t[e->own[i]].colmask &= (uint8_t)~slotbits_[i];    // shut to begin with
        if (lo >= hi)
            continue;    // and for good
        if (lo > wn->start)
            ev[i] = lo;
        else if (hi < wn->end)
            ev[i] = hi;
    }

    for (uint8_t k = 0; k < e->n; k++)
        e->t[k].idx = run_over(e->s, e->t[k].idx, wn->start);
    strip_begin(e, wn->start);
    uint8_t slot = 0;
    uint8_t nextev = nearest_event(ev, wn->end, &slot);
    for (;;) {
        for (uint8_t k = 0; k < e->n; k++)
            if (e->t[k].cp == e->cnt)
                fetch(e, k);
        uint8_t next = nearest(e, nextev);
        stretch(fb, bc, e->cnt, next, e->mask, e->de, e->dodd);
        e->cnt = next;
        while (e->cnt == nextev) {
            if (nextev == wn->end)
                return;
            uint8_t bit = slotbits_[slot];
            pt_sprite_track *t = &e->t[e->own[slot]];
            if (ev[slot] == wn->lo[slot]) {
                ev[slot] = wn->hi[slot] < wn->end ? wn->hi[slot] : (uint8_t)0xFF;
                t->colmask |= bit;
                merge(e, bit, (uint8_t)(e->s->spr[(uint8_t)(t->idx - 1u)] & 7));
            } else {
                ev[slot] = 0xFF;
                t->colmask &= (uint8_t)~bit;
                merge(e, bit, 0);
            }
            nextev = nearest_event(ev, wn->end, &slot);
        }
    }
}

// The strip's slots, assigned to columns as spritescale's striploop does: a
// column takes slots while its pixels last, the next surviving one carries
// on in the next slot, and one wider than the strip spills into the next
// strip (tracked again there, from its first run). The slots past the
// sprite's last column stay unused.
static void assign(const pt_sprite_setup *s, pt_sprite_walk *w, pt_sprite_engine *e)
{
    e->n = 0;
    for (;;) {
        uint8_t k = e->n++;
        e->t[k] = (pt_sprite_track) {.idx = s->spr[3 + w->c], .colmask = slotbits_[w->slot]};
        e->own[w->slot] = k;
        for (;;) {
            if (--w->rem == 0)
                break;
            if (++w->slot == 4) {
                w->slot = 0;
                return;
            }
            e->t[k].colmask |= slotbits_[w->slot];
            e->own[w->slot] = k;
        }
        w->c++;
        if (!findcol(s, w) || ++w->slot == 4) {
            w->slot = 0;
            return;
        }
    }
}

void pt_sprite_plot_engine(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p,
                           pt_sprite_clip clip)
{
    pt_sprite_setup s;
    if (!setup(&s, a, p))
        return;
    pt_sprite_engine e = {.s = &s};
    pt_sprite_walk w = {.c = s.c0, .rem = s.rem0, .xf = s.xf0, .slot = (uint8_t)(s.sx0 & 3)};
    uint8_t bc = (uint8_t)(s.sx0 >> 2);
    for (uint8_t strips = s.strips;;) {
        assign(&s, &w, &e);
        uint8_t covered = 0;
        for (uint8_t k = 0; k < e.n; k++)
            covered |= e.t[k].colmask;
        pt_sprite_win wn;
        uint8_t kind = classify(&s, clip, bc, covered, &wn);
        if (kind == PT_STRIP_CLEAN)
            strip_engine(fb, &e, bc, &wn);
        else if (kind == PT_STRIP_CLIPPED)
            strip_engine_clip(fb, &e, bc, covered, &wn);
        bc++;
        if (--strips == 0 || w.c == s.wid)
            return;
    }
}

// ---- the per-pair rebuild ----

// A slot of the rebuild's: one surviving source column, or a dummy
// (padding before the sprite, or past it).
typedef struct pt_sprite_slot {
    uint8_t idx;       // its next run byte
    uint8_t ev;        // the pair of its next event: its run's end (&FF on a
                       // dummy); in a clipped strip, the nearer of that and
                       // its window's opening or closing
    uint8_t ce, co;    // its run's even and odd patterns, masked to the slot
    uint8_t cm;        // the slot's bit if it shows the background, else 0
} pt_sprite_slot;

static void dummy(pt_sprite_slot *sl, uint8_t i)
{
    sl->ev = 0xFF;
    sl->ce = 0;
    sl->co = 0;
    sl->cm = slotbits_[i];
}

// afetch: slot i's next run, and its patterns; the run's end. Under 1x
// runs shrink away to nothing, ending at the pair they're fetched at, and
// are swallowed (CMP acnt : BEQ fetch); a clipped strip's swallow those
// below it too (a BCC beside the BEQ), which at a slot's opening are the
// runs under its window. The patterns are the art's, masked to the slot:
// with the slot a constant in each unrolled copy, LDA (pal),Y : AND
// #slotmask costs what spritescale's pre-masked tables' indexing does,
// without the tables.
static uint8_t afetch(const pt_sprite_setup *s, pt_sprite_slot *sl, uint8_t i, uint8_t x,
                      bool under)
{
    uint8_t b;
    uint8_t end;
    do {
        b = s->spr[sl->idx];
        sl->idx = (uint8_t)(sl->idx + 1u);
        end = s->py[(b >> 3) + 1];
    } while (end == x || (under && end < x));
    RC_ASSERT(end > x);
    uint8_t code = (uint8_t)(b & 7);
    if (code == 0) {
        sl->ce = 0;
        sl->co = 0;
        sl->cm = slotbits_[i];
    } else {
        sl->ce = (uint8_t)(s->pal[code].e & slotbits_[i]);
        sl->co = (uint8_t)(s->pal[code].o & slotbits_[i]);
        sl->cm = 0;
    }
    return end;
}

// One rebuilt pair at x: the four slots ORed together.
static void rebuild_pair(uint8_t *fb, uint8_t bc, uint8_t x, const pt_sprite_slot sl[4])
{
    uint8_t m = (uint8_t)(sl[0].cm | sl[1].cm | sl[2].cm | sl[3].cm);
    uint8_t de = (uint8_t)(sl[0].ce | sl[1].ce | sl[2].ce | sl[3].ce);
    uint8_t dodd = (uint8_t)(sl[0].co | sl[1].co | sl[2].co | sl[3].co);
    stretch(fb, bc, x, (uint8_t)(x + 1), m, de, dodd);
}

// A clean strip, spritescale's: each pair, the slots whose runs end there
// fetched, then rebuilt. A sprite standing below the screen has its slots'
// runs under it passed by first.
static void strip_rebuild(uint8_t *fb, const pt_sprite_setup *s, pt_sprite_slot sl[4], uint8_t bc,
                          uint8_t covered, const pt_sprite_win *wn)
{
    for (uint8_t i = 0; i < 4; i++) {
        if (covered & slotbits_[i]) {
            sl[i].idx = onscreen_run(s, sl[i].idx);
            sl[i].ev = wn->start;    // fetches at the first pair
        }
    }
    for (uint8_t x = wn->start; x != wn->end; x++) {
        for (uint8_t i = 0; i < 4; i++)
            if (x == sl[i].ev)
                sl[i].ev = afetch(s, &sl[i], i, x, false);
        rebuild_pair(fb, bc, x, sl);
    }
}

// A clipped strip: each slot is one pixel column, so its window's opening
// and closing are just more of its own events, and a slot shut is a dummy.
// Its first event is the opening, at the window's bottom, where it fetches
// the run over it; later ones are its runs' ends and, last, the window's
// top, where it's a dummy for good. The pair loop is the clean strips'
// (CPX ev,n per slot); only the fetch knows the difference.
static void strip_rebuild_clip(uint8_t *fb, const pt_sprite_setup *s, pt_sprite_slot sl[4],
                               uint8_t bc, uint8_t covered, const pt_sprite_win *wn)
{
    for (uint8_t i = 0; i < 4; i++) {
        if ((covered & slotbits_[i]) == 0)
            continue;
        uint8_t idx = sl[i].idx;
        dummy(&sl[i], i);
        sl[i].idx = idx;
        if (wn->lo[i] < wn->hi[i])
            sl[i].ev = wn->lo[i];
    }
    for (uint8_t x = wn->start; x != wn->end; x++) {
        for (uint8_t i = 0; i < 4; i++) {
            pt_sprite_slot *q = &sl[i];
            if (x != q->ev)
                continue;
            if (x == wn->hi[i]) {
                dummy(q, i);
                continue;
            }
            uint8_t end = afetch(s, q, i, x, true);
            q->ev = end < wn->hi[i] ? end : wn->hi[i];
        }
        rebuild_pair(fb, bc, x, sl);
    }
}

void pt_sprite_plot_rebuild(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p,
                            pt_sprite_clip clip)
{
    RC_ASSERT(p.scale < 256);
    pt_sprite_setup s;
    if (!setup(&s, a, p))
        return;
    pt_sprite_walk w = {.c = s.c0, .xf = s.xf0};
    uint8_t lead = (uint8_t)(s.sx0 & 3);    // the first strip's padding slots
    bool exhausted = false;
    uint8_t bc = (uint8_t)(s.sx0 >> 2);
    for (uint8_t strips = s.strips;;) {
        // the four slots: padding, then a surviving column each (under 1x
        // a column is a pixel at most), then past the end
        pt_sprite_slot sl[4];
        uint8_t covered = 0;
        for (uint8_t i = 0; i < 4; i++) {
            if (lead != 0) {
                lead--;
                dummy(&sl[i], i);
            } else if (exhausted) {
                dummy(&sl[i], i);
            } else {
                sl[i] = (pt_sprite_slot) {.idx = s.spr[3 + w.c]};
                covered |= slotbits_[i];
                w.c++;
                exhausted = !findcol(&s, &w);
            }
        }
        pt_sprite_win wn;
        uint8_t kind = classify(&s, clip, bc, covered, &wn);
        if (kind == PT_STRIP_CLEAN)
            strip_rebuild(fb, &s, sl, bc, covered, &wn);
        else if (kind == PT_STRIP_CLIPPED)
            strip_rebuild_clip(fb, &s, sl, bc, covered, &wn);
        bc++;
        if (--strips == 0 || exhausted)
            return;
    }
}

void pt_sprite_plot(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p, pt_sprite_clip clip)
{
    if (p.scale >= 256 || (uint8_t)p.scale >= pt_sprite_crossover(a))
        pt_sprite_plot_engine(fb, a, p, clip);
    else
        pt_sprite_plot_rebuild(fb, a, p, clip);
}
