// vcache.h - the vertex cache: the polar forms (angle and log length) of
// vertices as seen from one position, the camera's, each made the first
// time it's needed, like the walk's visits. There's one, the app's, and the
// camera and the renderer share it: the camera looks for its cell with it
// after each move, and the frame's capture then looks from the same place,
// with the same angles. A vertex has its entry's index (0 for none), and an
// entry its vertex, so emptying the cache clears just the vertices it
// holds. It's emptied when the position changes, so turning on the spot
// costs no vertex work at all.

#ifndef PT_VCACHE_H_
#define PT_VCACHE_H_

#include <stdint.h>

#include "arith.h"
#include "map.h"

// The most entries the cache holds. No position sees every vertex, and maps
// are designed so that none needs more than this: the panic in
// pt_vcache_vert is where a map that does is caught.
#define PT_MAX_VCACHE 63    // with the null entry, 64: four tables a page on the Beeb
#define PT_VC_MAX (PT_MAX_VCACHE + 1)    // entries, entry 0 the null one

typedef struct pt_vcache {
    uint16_t x, y;                      // the position, in whole units
    uint8_t slot[PT_MAX_VERTS];         // each vertex's entry, 0 = none
    uint8_t num;                        // entries made, the null one included
    uint8_t vert[PT_VC_MAX];            // each entry's vertex
    pt_polar polar[PT_VC_MAX];
} pt_vcache;

// Make it, empty, looking from (0, 0).
void pt_vcache_init(pt_vcache *vc);

// Empty it, and look from (x, y), in whole units.
void pt_vcache_reset(pt_vcache *vc, uint16_t x, uint16_t y);

// Look from (x, y): a new position empties it, the same one keeps it.
void pt_vcache_move(pt_vcache *vc, uint16_t x, uint16_t y);

// The polar form of vertex v from the cache's position (the middle of its
// whole-unit square), made on first need.
const pt_polar *pt_vcache_vert(pt_vcache *vc, const pt_map *m, uint8_t v);

// The cell holding the cache's position, as the angles tell it: from cell,
// hop through any portal side the position is beyond, and again. A point
// on a side goes to the lower-numbered cell.
uint8_t pt_vcache_locate(pt_vcache *vc, const pt_map *m, uint8_t cell);

#endif
