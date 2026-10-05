// render.h - the portal renderer: the capture walk over the cells, which
// finds its way by the angles of their vertices from the eye (the app's
// vertex cache, shared with the camera); its lines, made from those
// vertices' logs, clipped against bounds held a row per screen column and
// toggled straight into the back buffer as they're made (plot.h); and the
// EOR fill.

#ifndef PT_RENDER_H_
#define PT_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "map.h"
#include "mode5.h"
#include "richc/macros.h"
#include "sprite.h"
#include "vcache.h"

#define PT_MAX_DEPTH 16      // portals nested along a line of sight (the walk's
                              // recursion, which keeps only a visit index)
#define PT_MAX_VISITS PT_MAX_CELLS    // distinct cells one frame may visit; entry 0 is null
#define PT_CAM_HEIGHT 10      // eye height above the cell's floor, in 8-unit z steps
#define PT_MAX_RECS 128       // runs plotted in a frame, for the unplotter
#define PT_MAX_INSTS PT_MAX_SPRITES    // sprites seen in a frame; entry 0 is null
#define PT_CLIP_POOL 768               // the sprites' clip rows, two bytes a column:
                                       // three full-width sprites (336 the most
                                       // seen in 60,000 poses); one that won't
                                       // fit isn't drawn
#define PT_SPRITE_NEAR_L (5 * 256)     // sprites nearer than 32 units (main's
                                       // limit: 16x for the boulder and the
                                       // obelisk) aren't drawn; a depth's log

// A plotted run, as the unplotter wants it: columns [x0, x1) from the row at
// x0 (u8.8, the rounding half in), stepping by g (plot.h's gradient). Six
// bytes; on the 6502, six arrays, a field each.
typedef struct pt_rec {
    uint8_t x0, x1;
    uint16_t y;
    uint16_t g;
} pt_rec;

// A height as an edge's lines see it.
typedef struct pt_height {
    uint8_t m;            // |z - eye_z|, in the map's 8-unit z steps
    uint8_t below;        // z below the eye (z increases downwards)
    int16_t l;            // 256 log2(64 * 8m) + 8 * 256: less a depth's log,
                          // the log of a row's offset from the middle, u8.8
                          // (0 for m = 0, the eye's own height)
} pt_height;

// A visited cell, made when an edge into it is first met and shared by
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
    uint8_t tmax, bmin;   // the bounds' extents over the portal: at least the
                          // lowest top row, at most the highest bottom row.
                          // 0 and 64 say the bound is the screen's edge all
                          // along, and its rows aren't kept in top[] or bot[]
    uint16_t lo;          // the cell's window, set when first walked: its
                          // start, a world angle (10 bits). A vertex's place
                          // in it is t = (angle - lo) & 1023
    uint16_t tl;          // the view's left edge's place in the window, 0..768;
                          // its right edge is 256 on
    uint8_t lo8;          // the low byte of the window start's view angle: an
                          // in-view t's view angle is the byte t + lo8
    uint16_t t0, t1;      // the current side's ends' places (t1 a turn on, up
                          // to 1,535, for the eye's own cell's last side)
    uint8_t c0, c1;       // and their columns
    pt_height hc, hf;     // the cell's ceiling and floor, as its lines see them
    uint8_t inst;         // its sprite as seen this frame, 0 for none
} pt_visit;

// A sprite seen this frame: where it goes on the screen, its depth for the
// order, and the rows it shows in each column it covers, gathered from its
// cell's visits into the clip pool (a column none of them covers shows
// none).
typedef struct pt_inst {
    uint8_t sprite;           // the map's
    pt_sprite_place place;
    int16_t ld;               // 256 log2 of its depth
    uint8_t cx0, cx1;         // the screen columns it covers
    uint8_t p0, p1;           // the pairs it covers, clamped to the screen
    uint16_t clip;            // its rows in the pool: each column's bottom
                              // pair (lo) from here, then its top (hi)
    uint8_t cols;             // the columns its cell's visits have shown
    bool whole;               // and each showed all of it there
} pt_inst;

typedef struct pt_render {
    const pt_map *map;
    uint8_t fb[PT_FB_BYTES];    // the frame: the back buffer, every byte of it
                                // written by the fill
    uint8_t tmem[PT_FB_GUARD + PT_FB_BYTES + PT_FB_GUARD];    // the toggle buffer, T,
                                // with a junk character row either side for
                                // the plotters' overruns
    uint8_t *tb;                // T proper, within tmem: all zero between frames
                                // (its junk rows aside: never read)

    // camera for the frame being rendered
    uint8_t eye_z;           // in 8-unit z steps, like the cell heights
    uint16_t cam4;           // facing, in 1024ths of a turn (the camera's 256ths, x4)
    uint8_t cam_cell;
    pt_vcache *vc;           // the app's vertex cache, shared with the camera

    // the bounds, a row per screen column: the top bound's row (the first
    // row inside) and the bottom bound's (the first row below). The walk
    // overwrites a portal's own with a child's over the child's columns,
    // never needing the portal's there again
    uint8_t top[PT_SCREEN_WIDTH];
    uint8_t bot[PT_SCREEN_WIDTH];
    uint8_t open[PT_SCREEN_WIDTH];    // an edge's scratch: the child's open columns,
                                      // kept only once one closes (pt_open)

    pt_visit visits[PT_MAX_VISITS];
    uint8_t num_visits;
    uint8_t visits_high;             // high-water mark across frames, for stats
    uint8_t cell_visit[PT_MAX_CELLS];    // each cell's visit record, 0 = none
    uint8_t depth_high;              // deepest walk nesting across frames, for stats

    // the frame's plotted runs, for the unplotter, and each byte column's
    // last toggled row + 1, for the fill
    pt_rec recs[PT_MAX_RECS];
    uint8_t num_recs;
    uint8_t recs_high;               // high-water mark across frames, for stats
    uint8_t lastp1[PT_SCREEN_WIDTH / 4];

    // the sprites seen, the order they're drawn in, far to near, and their
    // clip rows
    pt_inst insts[PT_MAX_INSTS];
    uint8_t num_insts;
    uint8_t order[PT_MAX_INSTS];
    uint8_t num_order;
    uint8_t clipmem[PT_CLIP_POOL];
    uint16_t clip_used;
    uint16_t clip_high;              // high-water mark across frames, for stats

    bool sprites;                    // draw sprites
} pt_render;

// Bind the renderer to a map and the app's vertex cache.
void pt_render_init(pt_render *r, const pt_map *m, pt_vcache *vc);

// Render one frame into r->fb from the camera: the vertex cache looks from
// its eye, and cell is the one the cache's angles put it in, as the
// camera's tick left them. The capture walk toggles lines into T, the fill
// takes T into the frame, the sprites go over it, and the unplotter clears
// T again.
void pt_render_view(pt_render *r, uint8_t angle, uint8_t cell);

// Render one frame from a bare pose (the tools' and tests'): the vertex
// cache is moved to look from (cam_x, cam_y), and the eye's cell found from
// cell by its angles, then as pt_render_view.
void pt_render_frame(pt_render *r, uint16_t cam_x, uint16_t cam_y, uint8_t angle, uint8_t cell);

#endif
