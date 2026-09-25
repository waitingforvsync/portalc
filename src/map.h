// map.h - the world: convex cells with clockwise vertex rings (viewed from
// above with x up the page, y right, z downwards), twin-linked edges, and the
// per-edge collision data the editor precomputes. All coordinates are
// unsigned: the authored scene is translated into positive space.

#ifndef PT_MAP_H_
#define PT_MAP_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "palette.h"

#define PT_MAX_VERTS 64
#define PT_MAX_CELLS 24
#define PT_MAX_EDGES 96
#define PT_MAX_CELL_EDGES 8

// The translation applied to the authored scene coordinates.
#define PT_WORLD_OX 1536
#define PT_WORLD_OY 1536
#define PT_WORLD_OZ 512

typedef struct pt_vertex {
    uint16_t x;
    uint16_t y;
} pt_vertex;

typedef struct pt_edge {
    uint8_t v0;          // start vertex; the end is the next ring edge's v0
    uint8_t cell;        // owning cell
    uint8_t adj;         // adjacent cell, 0 = solid wall
    uint8_t twin;        // global index of the twin edge (valid when adj != 0)
    uint8_t soft;        // soft edge: exists only to keep cells convex
    pt_pattern wall;     // wall colour (walls, risers and lintels)
    int16_t ex, ey;      // unit direction s0.8
    int16_t nx, ny;      // inward unit normal s0.8
    int32_t d0;          // dot(V0, n) in s16.8
    uint16_t len;        // edge length in world units
} pt_edge;

typedef struct pt_cell {
    uint8_t first_edge;
    uint8_t num_edges;
    uint16_t floor_z;    // z increases downwards: floor_z > ceil_z
    uint16_t ceil_z;
    pt_pattern floor;
    pt_pattern ceil;
} pt_cell;

typedef struct pt_map {
    pt_vertex verts[PT_MAX_VERTS];
    pt_cell cells[PT_MAX_CELLS];    // cell 0 is the "outside" and never used
    pt_edge edges[PT_MAX_EDGES];
    uint8_t num_verts;
    uint8_t num_cells;
    uint8_t num_edges;
} pt_map;

// The ring successor of an edge (wraps within its cell).
static inline uint8_t pt_map_ring_next(const pt_map *m, uint8_t e)
{
    const pt_cell *c = &m->cells[m->edges[e].cell];
    uint8_t k = (uint8_t)(e - c->first_edge + 1);
    return (uint8_t)(c->first_edge + (k == c->num_edges ? 0 : k));
}

// Build the demo scene: a pentagonal room with a depressed square in the
// centre of its floor, and a corridor to a second, square room.
void pt_scene_build(pt_map *m);

// Check every structural invariant (twins, convexity, winding, soft-edge
// attribute equality, collision data); trips an assert on failure in debug,
// returns false in release.
bool pt_map_validate(const pt_map *m);

#endif
