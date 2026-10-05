// render.h - the portal renderer: the capture walk over the cells, which
// finds its way by the angles of their vertices from the eye (the vertex
// cache), its lines clipped against bounds held a row per screen column and
// toggled straight into the back buffer as they're made, and the EOR fill.
// Added and subtracted values are two's complement; multiplies and divides
// run on unsigned magnitudes with the sign held in a branch, and gradients
// pack abs(g) | (is_negative << 15).

#ifndef PT_RENDER_H_
#define PT_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "richc/macros.h"
#include "vcache.h"

#define PT_MAX_DEPTH 16      // portals nested along a line of sight (the walk's
                              // recursion, which keeps only a visit index)
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

// A visited cell, made when an edge into it is first clipped and shared by
// every later visit that frame. Visits of one cell run in increasing x and
// never nest (a ray crosses a convex cell once), so the visit record holds
// the portal being walked as well as the ring cursor: a later visit resumes
// from the edge the previous one ended on. The portal's bounds themselves
// are the renderer's per-column rows, over its columns. The walk recurses
// on the index alone.
typedef struct pt_visit {
    uint8_t cell;         // back-reference, to clear the cell's index at frame end
    uint8_t k;            // the current side's ring position, 0..num
    uint8_t x;            // where the next edge's span starts: the end of the
                          // last one (the portal's left column to begin with)
    uint8_t xr;           // the portal's right column; 0 until first walked
    uint8_t tmax, bmin;   // the bounds' extents over the portal: the lowest
                          // top row and the highest bottom row
    int16_t lo;           // the cell's window: its vertices' view angles are
                          // read in [lo, lo + 1024), set when first walked
} pt_visit;

typedef struct pt_render {
    const pt_map *map;
    uint8_t fbmem[PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD];    // the back buffer, with the
                                                               // junk rows either side
    uint8_t *fb;             // the back buffer proper, within fbmem

    // camera for the frame being rendered
    uint16_t cam_x, cam_y;   // whole units
    uint8_t eye_z;           // in 8-unit z steps, like the cell heights
    uint16_t cam4;           // facing, in 1024ths of a turn (the camera's 256ths, x4)
    uint8_t cam_cell;
    pt_vcache *vc;           // the vertices' polar forms from the eye, this frame
    pt_vcache own_vc;        // the cache for a frame from a bare pose

    // the bounds, a row per screen column: the top bound's row (the first
    // row inside) and the bottom bound's (the first row below). The walk
    // overwrites a portal's own with a child's over the child's columns,
    // never needing the portal's there again
    uint8_t top[PT_SCREEN_WIDTH];
    uint8_t bot[PT_SCREEN_WIDTH];
    uint8_t open[PT_SCREEN_WIDTH];    // an edge's scratch: the child's open columns

    pt_visit visits[PT_MAX_VISITS];
    uint8_t num_visits;
    uint8_t visits_high;             // high-water mark across frames, for stats
    uint8_t cell_visit[PT_MAX_CELLS];    // each cell's visit record, 0 = none
    uint8_t depth_high;              // deepest walk nesting across frames, for stats

    bool sprites;                    // draw sprites (none yet: phase 5)
} pt_render;

// Bind the renderer to a map.
void pt_render_init(pt_render *r, const pt_map *m);

// Render one frame into r->fb from a bare pose, with the renderer's own
// vertex cache: clear, capture walk, fill.
void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell);

// Render one frame from a vertex cache already looking from the eye (the
// camera's, filled by its collision), without working its vertices out
// again.
void pt_render_view(pt_render *r, pt_vcache *vc, uint8_t angle, uint8_t cell);

#endif
