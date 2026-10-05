// vcache.h - the vertex cache: the polar forms (angle and log length) of
// vertices as seen from one position, the camera's, each made the first
// time it's needed, like the walk's visits. A vertex has its entry's index
// (0 for none), and an entry its vertex, so emptying the cache clears just
// the vertices it holds.
//
// Collision fills it from where the camera moves to, and the frame's
// capture looks from there too, so they share it. It stays good while the
// camera stands still: turning on the spot costs no vertex work at all.

#ifndef PT_VCACHE_H_
#define PT_VCACHE_H_

#include <stdint.h>

#include "arith.h"
#include "map.h"

#define PT_VC_MAX (PT_MAX_VERTS + 1)    // entries, entry 0 the null one

typedef struct pt_vcache {
    uint16_t x, y;                      // the position, in whole units
    uint8_t slot[PT_MAX_VERTS];         // each vertex's entry, 0 = none
    uint8_t num;                        // entries made, the null one included
    uint8_t vert[PT_VC_MAX];            // each entry's vertex
    pt_polar polar[PT_VC_MAX];
} pt_vcache;

// Empty it, and look from (x, y).
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
