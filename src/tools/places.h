// places.h - the camera's places, for the checks' tools (captureref, walkref): cell by cell,
// the centroid (the mean of its ring's vertices, truncated), then 7/8 of the way from it to each
// vertex in turn, near the corners, where the view's right edge can land on the cell's last
// side. beeb/test/places.6502 makes the same.

#ifndef PT_PLACES_H
#define PT_PLACES_H

#include <stdint.h>

#include "map.h"

#define PT_MAX_PLACES (PT_MAX_CELLS + PT_MAX_SIDES)

typedef struct place {
    uint8_t cell;
    uint16_t x, y;
} place;

static unsigned places(const pt_map *m, place *out)
{
    unsigned n = 0;
    for (uint8_t cell = 1; cell < m->num_cells; cell++) {
        const pt_cell *c = &m->cells[cell];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            sx += m->verts[pt_map_side(m, s)->v].x;
            sy += m->verts[pt_map_side(m, s)->v].y;
        }
        uint32_t k = (uint32_t)(pt_map_ring_end(c) - c->first);
        uint32_t cx = sx / k;
        uint32_t cy = sy / k;
        out[n++] = (place) {cell, (uint16_t)cx, (uint16_t)cy};
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            pt_vertex v = m->verts[pt_map_side(m, s)->v];
            out[n++] = (place) {cell, (uint16_t)((cx + 7u * v.x) / 8),
                                (uint16_t)((cy + 7u * v.y) / 8)};
        }
    }
    for (unsigned i = 0; i < n; i++)
        RC_ASSERT(pt_map_cell_contains(m, out[i].cell, out[i].x, out[i].y));
    return n;
}

#endif
