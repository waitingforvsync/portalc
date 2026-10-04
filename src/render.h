// render.h - the portal renderer: the recursive capture walk over an explicit
// portal stack, the per-frame line piece pool, EOR plotting and the fill, all
// into a MODE 5 back buffer. Added and subtracted values are two's
// complement; multiplies and divides run on unsigned magnitudes with the
// sign held in a branch, and gradients pack abs(g) | (is_negative << 15).

#ifndef PT_RENDER_H_
#define PT_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "richc/macros.h"

#define PT_POOL_SIZE 256      // piece 0 is the null link
#define PT_MAX_DEPTH 16      // portals nested along a line of sight (the walk's
                              // recursion, which keeps only a visit index)
#define PT_MAX_VISITS PT_MAX_CELLS    // distinct cells one frame may visit; entry 0 is null
#define PT_CAM_HEIGHT 10      // eye height above the cell's floor, in 8-unit z steps
#define PT_SPRITE_NEAR 32     // sprites nearer than this depth are skipped
#define PT_MAX_INSTS PT_MAX_SPRITES    // sprite instances in a frame; entry 0 is null
#define PT_SEG_POOL 256       // clip segment bytes a frame; offset 0 is null
#define PT_MAX_PEND 32        // walked portals with a sprite, awaiting their pop
#define PT_MAX_TOGS 32        // toggle rows the sprite pass holds for a byte column

// A gradient is a u7.8 magnitude with the sign in bit 15 (not two's
// complement); zero is always stored positive.
static inline uint16_t pt_g_make(uint16_t mag, uint8_t neg)
{
    RC_ASSERT(mag <= 0x7FFF);
    return (neg && mag) ? (uint16_t)(mag | 0x8000u) : mag;
}

static inline uint16_t pt_g_mag(uint16_t g)
{
    return (uint16_t)(g & 0x7FFFu);
}

static inline uint8_t pt_g_isneg(uint16_t g)
{
    return (uint8_t)(g >> 15);
}

// A line piece (9 bytes): a screen-space line over columns [x0, x1), row
// (y0, yf) +- (c - x0) * g, colour (ce, co), chained by pool index.
typedef struct pt_piece {
    uint8_t x0;
    uint8_t x1;
    uint8_t y0;      // the row at x0, u8.8 with the rounding bias in: the row
    uint8_t yf;      // (PT_SCREEN_HEIGHT marks off screen), and the fraction
                     // plus a half (128 for a line placed on a whole row)
    uint16_t g;      // gradient, sign bit + u7.8 magnitude, rows per column
    uint8_t ce;      // colour patterns; ce | co == 0 means unplotted
    uint8_t co;
    uint8_t next;    // next piece in this piece's chain, 0 = end
} pt_piece;

// Frustum classification of a view vertex: the ray sums a = x + y (left ray)
// and b = x - y (right ray) are formed once in transform and only their signs
// kept; the rare clips recompute the magnitudes they need from x and y. The
// negative bits sit at 7 and 6, where the 6502's BIT lands them in N and V.
#define PT_VF_ANEG 0x80u     // a < 0: strictly outside the left ray
#define PT_VF_BNEG 0x40u     // b < 0: strictly outside the right ray
#define PT_VF_AZERO 0x08u    // a == 0: exactly on the left ray
#define PT_VF_BZERO 0x04u    // b == 0: exactly on the right ray

// View data of a transformed vertex; sx and rx are valid only in the
// frustum.
typedef struct pt_vv {
    int16_t x;       // depth
    int16_t y;       // lateral, right positive
    uint8_t sx;      // floor(64 + 64*y/x) when inside both rays
    uint8_t flags;   // PT_VF_*
    pt_recip rx;     // reciprocal of the depth (at least 1), for every
                     // projection at this vertex
} pt_vv;

// The lines an edge can generate, one slot each (a solid wall uses the two
// outer ones): a line keeps its slot on every visit, which is how a visible
// piece finds the piece it continues.
enum {
    PT_SLOT_CEIL,          // at the cell's own ceiling: wall top, or top ceiling
    PT_SLOT_CEIL_BOUND,    // the child's top bound
    PT_SLOT_FLOOR_BOUND,   // the child's bottom bound
    PT_SLOT_FLOOR,         // at the cell's own floor: wall bottom, or bottom floor
    PT_NUM_SLOTS,
};

// A visited cell, made when an edge into it is first clipped and shared by
// every later visit that frame. Visits of one cell run in increasing x and
// never nest (a ray crosses a convex cell once), so the visit record holds
// the portal being walked as well as the ring cursor: a later visit resumes
// from the edge the previous one ended on, and a line continuing across a
// contiguous seam extends its piece. The walk recurses on the index alone.
typedef struct pt_visit {
    uint8_t cell;         // back-reference, to clear the cell's index at frame end
    uint8_t k;            // the current edge, as a ring index 0..num
    uint8_t x;            // where the next edge's span starts: the end of the
                          // last one (the portal's left column to begin with)
    uint8_t xr;           // the portal's right column; 0 until first walked
    uint8_t tcur, bcur;   // the top and bottom bounds: chains of pieces, each
                          // cursor at or before column x (the chains' heads
                          // as the parent's clip built them)
    uint8_t tmax, bmin;   // conservative row extents of the bounds
    pt_vv v0;             // view data of the current edge's start vertex
    pt_vv v1;             // and of its end vertex
    uint8_t inst;         // the cell's sprite instance this frame, 0 = none
    uint8_t active[PT_NUM_SLOTS];    // the last piece of each of edge k's
                                     // lines (0 = none yet), cleared when the
                                     // cursor moves on: visible or not (a
                                     // hidden piece has no colour), it is the
                                     // line itself for any later visit (a
                                     // piece's x0, row and g never change)
} pt_visit;

// A sprite projected for the frame (spec 9.2), on its cell's first visit.
// Its screen extent is kept as its top-left corner and the scale, from
// which the sprite pass steps every source row and column edge; the
// corner's coordinates can lie well off the screen, so they take 24 bits
// on the 6502.
typedef struct pt_inst {
    uint8_t sprite;       // the map's sprite
    uint8_t cx0, cx1;     // the screen columns it covers, [cx0, cx1)
    uint8_t seg;          // its clip segments in x order (offsets into segs, 0 = none)
    uint8_t seg_tail;
    uint8_t prev, next;   // the draw order, far to near (0 = none)
    uint16_t scale;       // screen pixels per source pixel, u8.8
    int32_t left;         // the left edge's column, u8.8
    int32_t top;          // the top edge's row, u8.8

    // the sprite pass's state, stepped along the instance's columns
    uint8_t rows[PT_SPRITE_MAX_ROWS + 1];    // each source row edge's screen row, 0..H
    uint8_t col;          // the source column at the current screen column
    int32_t col_next;     // its right edge, u8.8
    uint8_t cseg;         // the segment the bound cursors are in, 0 = none yet
    uint8_t cc;           // the column they are at, 0xFF = none
    uint8_t tk, bk;       // their pieces, as positions in the segment's lists
    uint16_t ty, by;      // and those pieces' rows there, biased u8.8
} pt_inst;

// A byte column's toggle row in the sprite pass (spec 9.5): the plotted
// byte pair, exactly as the plotters and earlier sprites left it.
typedef struct pt_tog {
    uint8_t row;
    uint8_t e, o;
} pt_tog;

// A clip segment in the segment pool (spec 9.3): the columns [x0, x1) of an
// instance one visit of its cell sees, with the bound pieces over them.
enum {
    PT_SEG_X0,
    PT_SEG_X1,
    PT_SEG_NEXT,    // the instance's next segment, 0 = none
    PT_SEG_NT,      // then nt top chain pieces and nb bottom chain pieces, in
    PT_SEG_NB,      // x order: each runs to the next one's x0, the last to x1
    PT_SEG_PIECES,
};

typedef struct pt_render {
    const pt_map *map;
    uint8_t fbmem[PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD];    // the back buffer, with the
                                                               // junk rows either side
    uint8_t *fb;             // the back buffer proper, within fbmem

    // camera for the frame being rendered
    uint16_t cam_x, cam_y;
    uint8_t eye_z;           // in 8-unit z steps, like the cell heights
    uint8_t angle;           // facing, in 256ths of a turn
    uint8_t cam_cell;

    pt_piece pool[PT_POOL_SIZE];
    uint16_t pool_num;
    uint16_t pool_high;          // high-water mark across frames, for stats

    uint8_t depth_high;          // deepest walk nesting across frames, for stats

    pt_visit visits[PT_MAX_VISITS];
    uint8_t num_visits;
    uint8_t visits_high;             // high-water mark across frames, for stats
    uint8_t cell_visit[PT_MAX_CELLS];    // each cell's visit record, 0 = none

    pt_inst insts[PT_MAX_INSTS];
    uint8_t num_insts;
    uint8_t order_head, order_tail;  // the sprites' draw order, far to near
    uint8_t segs[PT_SEG_POOL];
    uint16_t seg_num;
    uint16_t segs_high;              // high-water mark across frames, for stats
    uint8_t pend[PT_MAX_PEND];       // instances of the walked portals, by nesting
    uint8_t num_pend;
    uint8_t pend_high;               // high-water mark across frames, for stats

    bool sprites;                    // run the sprite pass (on; off for comparisons)
    pt_tog togs[PT_MAX_TOGS];        // the sprite pass's byte column
    uint8_t num_togs;
    uint8_t togs_high;               // high-water mark across frames, for stats
} pt_render;

// Bind the renderer to a map.
void pt_render_init(pt_render *r, const pt_map *m);

// Render one frame into r->fb: clear, capture walk, plot, fill.
void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell);

#endif
