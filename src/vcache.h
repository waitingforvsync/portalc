// vcache.h - the vertex cache: the polar forms (angle and log length) of
// cells' ring vertices as seen from one position, the camera's, made a cell
// at a time the first time a cell is needed, like the walk's visits. So its
// size follows the cells a position needs, not the map's.
//
// Collision fills it from where the camera moves to, and the frame's
// capture looks from there too, so they share it. It stays good while the
// camera stands still: turning on the spot costs no vertex work at all.

#ifndef PT_VCACHE_H_
#define PT_VCACHE_H_

#include <stdint.h>

#include "arith.h"
#include "map.h"

#define PT_VC_MAX 32    // entries: the cells one position needs

typedef struct pt_vcache {
    uint16_t x, y;                              // the position, in whole units
    uint8_t slot[PT_MAX_CELLS];                 // each cell's entry + 1, 0 = none
    uint8_t num;                                // entries made
    uint8_t cell[PT_VC_MAX];                    // each entry's cell
    pt_polar polar[PT_VC_MAX][PT_MAX_RING];     // by ring position
} pt_vcache;

// Empty it, and look from (x, y).
void pt_vcache_reset(pt_vcache *vc, uint16_t x, uint16_t y);

// Look from (x, y): a new position empties it, the same one keeps it.
void pt_vcache_move(pt_vcache *vc, uint16_t x, uint16_t y);

// The polar forms of a cell's ring vertices from the cache's position (the
// middle of its whole-unit square), by ring position (the vertex starting
// each side), made on first need.
const pt_polar *pt_vcache_cell(pt_vcache *vc, const pt_map *m, uint8_t cell);

// The cell holding the cache's position, as the angles tell it: from cell,
// hop through any portal side the position is beyond, and again. A point
// on a side's line goes to the lower-numbered cell.
uint8_t pt_vcache_locate(pt_vcache *vc, const pt_map *m, uint8_t cell);

#endif
