// render.h - the portal renderer: the recursive capture walk over an explicit
// portal stack, the per-frame line piece pool, EOR plotting and the fill, all
// into a MODE 5 back buffer. Added and subtracted values are two's
// complement; multiplies and divides run on unsigned magnitudes with the
// sign held in a branch, and gradients pack abs(g) | (is_negative << 15).

#ifndef PT_RENDER_H_
#define PT_RENDER_H_

#include <stdint.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "richc/macros.h"

#define PT_POOL_SIZE 256      // piece 0 is the null link
#define PT_MAX_DEPTH 24
#define PT_MAX_CLOSED 8
#define PT_MAX_VISITS PT_MAX_CELLS    // distinct cells one frame may visit; entry 0 is null
#define PT_CAM_HEIGHT 10      // eye height above the cell's floor, in 8-unit z steps

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

// A line piece (8 bytes): a screen-space line over columns [x0, x1), row
// y0 +- (c - x0) * g, colour (ce, co), chained by pool index.
typedef struct pt_piece {
    uint8_t x0;
    uint8_t x1;
    uint8_t y0;      // row at x0; PT_SCREEN_HEIGHT marks off screen (flat floor markers)
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

// A visited cell, created when its first visit is pushed and shared by
// every later visit that frame. Visits of one cell run in increasing x and never
// nest (a ray crosses a convex cell once), so the ring cursor lives here
// rather than in the frame: a later visit resumes from the edge the
// previous one ended on, and a line continuing across a contiguous seam
// extends its piece.
typedef struct pt_visit {
    uint8_t cell;         // back-reference, to clear the cell's index at frame end
    uint8_t k;            // the current edge, as a ring index 0..num_edges-1
    uint8_t xr;           // right column of the latest visit pushed or ended
    pt_vv v0;             // view data of the current edge's start vertex
    pt_vv v1;             // and of its end vertex
    uint8_t active[PT_NUM_SLOTS];    // last visible piece of each of edge k's
                                     // lines, cleared when the cursor moves on
} pt_visit;

// A portal frame on the explicit walk stack, pushed ready to walk: its
// cell's record already holds the cursor on its first edge. Its left column
// is not stored: it is where its bound chains begin (the cursors'
// frontier). A frame is popped as its last edge starts, so that edge's
// children take its slot.
typedef struct pt_pframe {
    uint8_t cell;
    uint8_t xr;           // right column bound
    uint8_t tcur, bcur;   // current piece of the top / bottom bound chain
    uint8_t tmax, bmin;   // conservative row extents over the span
    uint8_t steps;        // safety: edges left before the ring must have ended
} pt_pframe;

typedef struct pt_render {
    const pt_map *map;
    uint8_t fb[PT_FB_BYTES];

    // camera for the frame being rendered
    uint16_t cam_x, cam_y;
    uint8_t eye_z;           // in 8-unit z steps, like the cell heights
    uint8_t angle;           // facing, in 256ths of a turn
    uint8_t cam_cell;

    pt_piece pool[PT_POOL_SIZE];
    uint16_t pool_num;
    uint16_t pool_high;          // high-water mark across frames, for stats

    pt_pframe stack[PT_MAX_DEPTH];
    uint8_t depth;

    pt_visit visits[PT_MAX_VISITS];
    uint8_t num_visits;
    uint8_t visits_high;             // high-water mark across frames, for stats
    uint8_t cell_visit[PT_MAX_CELLS];    // each cell's visit record, 0 = none
} pt_render;

// Bind the renderer to a map.
void pt_render_init(pt_render *r, const pt_map *m);

// Render one frame into r->fb: clear, capture walk, plot, fill.
void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell);

#endif
