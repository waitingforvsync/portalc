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

#ifndef PT_MAX_DEPTH          // (a check lowers it, so the cut-off runs)
#define PT_MAX_DEPTH 16      // portals nested along a line of sight (the walk's
                              // recursion, which keeps only a visit index)
#endif
#define PT_MAX_VISITS 31      // visits one frame may make, not necessarily a cell each
                              // (entry 0 is null, so 32 a table: on the Beeb, 8 tables a page)
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
#define PT_MAX_KEPT 8                  // sides' lines kept in a frame, for later
                                       // visits; entry 0 is null. The other 7
                                       // take every keep in 98.6% of map 0's
                                       // frames (1.4 a frame, 15 the most seen);
                                       // past that a later visit makes them again

// A plotted run, as the unplotter wants it: columns [x0, x1) from the row at
// x0 (u8.8, the rounding half in), stepping by g (plot.h's gradient). Six
// bytes; on the 6502, six arrays, a field each.
typedef struct pt_rec {
    uint8_t x0, x1;
    uint16_t y;
    uint16_t g;
} pt_rec;

// A height as an edge's lines see it, m = |z - eye_z| in the map's 8-unit z
// steps.
typedef struct pt_height {
    uint8_t below;        // z below the eye (z increases downwards)
    int16_t l;            // 256 log2(64 * 8m) + 8 * 256: less a depth's log,
                          // the log of a row's offset from the middle, u8.8.
                          // 0 for m = 0, the eye's own height (otherwise it's
                          // 17 * 256 at least, so its high byte tells)
} pt_height;

// A line kept for a later visit: its columns on the screen, [x0, x1), its
// row at x0 (u8.8, the rounding half in), its gradient (sign bit + u7.8),
// and the row at its end (pt_line's rl), for the fast path. Its side of the
// screen and its colour come from its height and its edge. 7 bytes.
typedef struct pt_kept {
    uint8_t x0, x1;
    uint16_t y;
    uint16_t g;
    uint8_t rl;
} pt_kept;

// The most lines an edge makes: one at each of its two cells' ceilings and
// floors, which are also a kept side's slots
#define PT_KEPT 4

// A visited cell, made when an edge into it is first met and shared by
// every later visit that frame. Visits of one cell run in increasing x and
// never nest (a ray crosses a convex cell once): a later visit resumes on the
// side the last one stopped on, and that side's lines are the same as last
// time. So a walk that stops on a side running on past its portal keeps the
// side's lines in the pool for the next, and the record points at them.
// That's all it keeps besides the cell's heights and its window, made at
// the first visit: everything else a walk needs it makes again on each
// visit (render.c's pt_walk). The portal's bounds are the renderer's
// per-column rows, over its columns. The walk recurses on the index alone.
typedef struct pt_visit {
    uint8_t cell;         // the cell visited: it clears the cell's index when
                          // the capture's done
    uint8_t side;         // the current side, in the cell's ring: where the
                          // last walk stopped
    uint8_t xr;           // the right column of the portal last walked through;
                          // 0 until first walked
    uint8_t inst;         // its sprite as seen this frame, 0 for none
    uint8_t kept;         // side k's lines in the pool, 0 for none
    pt_height hc, hf;     // the cell's ceiling and floor, as its lines see them
    uint16_t split_angle; // its window (pt_walk's), made at the first visit:
                          // any portal's gives its vertices the same screen
                          // angles
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
    uint16_t vl;             // the view's left edge, cam4 - 128 (10 bits): the
                             // walk measures vertices' angles from it
    uint8_t cam_cell;
    pt_vcache *vc;           // the app's vertex cache, shared with the camera

    // the bounds, a row per screen column: the top bound's row (the first
    // row inside) and the bottom bound's (the first row below). The walk
    // overwrites a portal's own with a child's over the child's columns,
    // never needing the portal's there again
    uint8_t top[PT_SCREEN_WIDTH];
    uint8_t bot[PT_SCREEN_WIDTH];

    pt_visit visits[PT_MAX_VISITS + 1];
    uint8_t num_visits;
    uint8_t visits_high;             // high-water mark across frames, for stats
    uint8_t cell_visit[PT_MAX_CELLS];    // each cell's visit record, 0 = none: all
                                         // 0 between captures
    uint8_t depth_high;              // deepest walk nesting across frames, for stats

    // the sides' lines kept for later visits, by slot, as the visits point
    pt_kept kept[PT_MAX_KEPT][PT_KEPT];
    uint8_t num_kept;

    // the frame's plotted runs, for the unplotter, and each byte column's
    // last toggled row + 1, for the fill
    pt_rec recs[PT_MAX_RECS];
    uint8_t num_recs;
    uint8_t toggle_end[PT_SCREEN_WIDTH / 4];

    // the sprites seen, the order they're drawn in, far to near, and their
    // clip rows
    pt_inst insts[PT_MAX_INSTS];
    uint8_t num_insts;
    uint8_t order[PT_MAX_INSTS];
    uint8_t num_order;
    uint8_t clipmem[PT_CLIP_POOL];
    uint16_t clip_used;

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

// A visit's walk: all it needs while it's in progress, and nothing a later
// visit does. It's made each time a walk enters the cell, and the walk's
// recursion keeps it while a child's walk runs (on the 6502, only vi, tmax
// and bmin, on the stack: the window comes back from the visit, and the
// side's end from its side). The visit record keeps what's left for a later
// visit.
typedef struct pt_walk {
    uint8_t vi;           // the visit
    uint8_t x;            // where the next edge's span starts: the end of the
                          // last one (the portal's left column to begin with)
    uint8_t tmax, bmin;   // the bounds' extents over the portal: at least the
                          // lowest top row, at most the highest bottom row.
                          // 0 and 64 say the bound is the screen's edge all
                          // along, and its rows aren't kept in top[] or bot[]
    uint16_t split_angle; // the cell's window: screen angles at or past this,
                          // 256..1024, are a turn less
    int16_t u0, u1;       // the current side's ends' screen angles (u1 a
                          // turn on, up to 1,535, for the eye's own cell's
                          // last side)
    uint8_t c0, c1;       // and their columns
} pt_walk;

// Where the camera cell's walk begins: the side that straddles the view's
// left edge, and its window's split (pt_walk's split_angle).
typedef struct pt_root_side {
    uint8_t side;
    uint16_t split_angle;
} pt_root_side;

// The root walk's side, for the eye in cell with the view's left edge at vl
// (10 bits), from the vertex cache's angles.
pt_root_side pt_render_root_side(pt_vcache *vc, const pt_map *m, uint8_t cell, uint16_t vl);

// A frame's setup and its root walk, all of a frame up to the walk proper: the
// root visit made and the walk begun on it, for the tools' checks. The caller
// empties the visit cache after, as a frame does.
pt_walk pt_render_root(pt_render *r, uint8_t angle, uint8_t cell);

// A frame's capture: its setup, the walk into T and the records, and the
// visit cache emptied, for the tools' checks; the fill, sprites and unplot
// are left.
void pt_render_capture(pt_render *r, uint8_t angle, uint8_t cell);

// A cell's sprite as it's seen (inst_new): its instance, or 0; and a visit's
// rows in its columns over [xl, xr) (inst_clip), or none there (inst_hide),
// for the tools' checks.
uint8_t pt_render_inst_new(pt_render *r, uint8_t cell, pt_height hf);
void pt_render_inst_clip(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr, uint8_t tmax,
                         uint8_t bmin);
void pt_render_inst_hide(pt_render *r, uint8_t ii, uint8_t xl, uint8_t xr);

// The last frame's runs cleared out of T from the records, then row 0
// (unplot), for the tools' checks: T is all 0 after.
void pt_render_unplot(pt_render *r);

// A height z as an edge's lines see it from an eye at eye_z (height_of).
pt_height pt_render_height(uint8_t z, uint8_t eye_z);

// An edge's ends as its lines see them: the logs of their depths (256 log2,
// in whole units), and of the columns between them (the walk's c1 - c0).
typedef struct pt_ends {
    int16_t logdepth0, logdepth1;
    int16_t logcolumns;
} pt_ends;

// The walk on to its next side (side_next), and its side's ends
// (edge_ends), for the tools' checks. The side must have columns.
void pt_render_side_next(pt_render *r, pt_walk *w);
pt_ends pt_render_ends(const pt_render *r, const pt_walk *w);

// A candidate line from make_line, before clipping against the bounds: on
// the screen over [x0, x1), and beyond its edge on the side below says
// either side of that (the screen pre-clip cut it off there).
typedef struct pt_line {
    uint8_t x0;
    uint8_t x1;
    uint16_t y;       // the row at x0, u8.8 with the rounding bias in
    uint16_t g;       // sign bit + u7.8 magnitude
    uint8_t rl;       // the row at its end: with the row at x0, its rows'
                      // extremes, near enough (make_line, fast_inside)
    uint8_t below;    // the side it is cut off on: below the screen, or above
    uint8_t ce;
    uint8_t co;
} pt_line;

// The walk's side's line at height h, from its ends (make_line), with no
// colour, for the tools' checks.
pt_line pt_render_line(const pt_walk *w, const pt_ends *e, pt_height h);

// Columns [x0, x1) of line L plotted into T from its row y at x0 (plot_run):
// recorded, and toggle_end brought down, for the tools' checks.
void pt_render_plot_run(pt_render *r, const pt_line *L, uint8_t x0, uint8_t x1, uint16_t y);

// The clips over [xa, xb) against the bounds in r->top and r->bot, their
// extents tmax and bmin (clip_plain, clip_floor, clip_ceil), and the columns
// closed (close_columns), for the tools' checks.
void pt_render_clip_plain(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                          uint8_t bmin);
uint8_t pt_render_clip_floor(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb,
                             uint8_t tmax, uint8_t bmin, bool *room);
uint8_t pt_render_clip_ceil(pt_render *r, const pt_line *L, uint8_t xa, uint8_t xb, uint8_t tmax,
                            uint8_t cbmin, bool *room);
void pt_render_close_columns(pt_render *r, uint8_t l, uint8_t rr, pt_pattern cbfb, bool bz);

#endif
