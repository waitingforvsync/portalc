// map.h - the world: convex cells with clockwise vertex rings (viewed from
// above with x up the page, y right, z downwards), twin-linked edges, and the
// per-edge collision data the editor precomputes. All coordinates are
// unsigned: the scene is authored directly in positive space.

#ifndef PT_MAP_H_
#define PT_MAP_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "palette.h"
#include "sprite.h"

#define PT_MAX_VERTS 64
#define PT_MAX_CELLS 24
#define PT_MAX_EDGES 96
#define PT_MAX_CELL_EDGES 8
#define PT_MAX_SPRITES 8
#define PT_MAX_ARTS 4

typedef struct pt_vertex {
    uint16_t x;
    uint16_t y;
} pt_vertex;

// An edge colour: a catalogue shade plus one, 0 marking a soft edge (one
// that exists only to keep cells convex and never draws).
#define PT_EDGE_SOFT 0
#define PT_EDGE_COLOUR(shade) ((uint8_t)((shade) + 1))

// An edge does not know its owning cell: edges are only ever reached through
// a cell's [first_edge, first_edge + num_edges) ring, so every walker already
// holds the cell and threads it into the ring functions below.
typedef struct pt_edge {
    uint8_t v0;          // start vertex; the end is the next ring edge's v0
    uint8_t adj;         // adjacent cell, 0 = solid wall
    uint8_t twin;        // the adjacent cell's edge running this one the
                         // other way (a portal's twin; unused for walls)
    uint8_t colour;      // wall shade + 1 (walls, risers and lintels); 0 = soft
    uint8_t angle;       // direction V0 -> V1 in 256ths of a turn; a twin is
                         // exactly this + 128, so both sides share one line
    uint8_t len8;        // floor(len / 8): the foot cap in 8-unit steps (the
                         // last few units of a wall belong to its post)
} pt_edge;

typedef struct pt_cell {
    uint8_t first_edge;
    uint8_t num_edges;
    uint8_t floor_z;     // z in 8-unit steps, increasing downwards: floor_z > ceil_z
    uint8_t ceil_z;
    uint8_t floor;       // catalogue shade indices
    uint8_t ceil;
    uint8_t sprite;      // the cell's sprite, 0 = none
} pt_cell;

// A sprite standing on its cell's floor. It lies wholly inside the cell
// (its width as a disc around the base), and below the cell's ceiling.
typedef struct pt_sprite {
    uint16_t x;          // the base's centre, in world units
    uint16_t y;
    uint16_t size;       // height in world units; the width follows the art's aspect
    uint8_t art;         // index into the map's art
} pt_sprite;

typedef struct pt_map {
    pt_vertex verts[PT_MAX_VERTS];
    pt_cell cells[PT_MAX_CELLS];    // cell 0 is the "outside" and never used
    pt_edge edges[PT_MAX_EDGES];
    pt_sprite sprites[PT_MAX_SPRITES];    // sprite 0 is unused ("none")
    pt_sprite_art arts[PT_MAX_ARTS];
    uint8_t num_verts;
    uint8_t num_cells;
    uint8_t num_edges;
    uint8_t num_sprites;
    uint8_t num_arts;
} pt_map;

// The ring successor of an edge of cell c (wraps within the cell).
static inline uint8_t pt_map_ring_next(const pt_cell *c, uint8_t e)
{
    uint8_t k = (uint8_t)(e - c->first_edge + 1);
    return (uint8_t)(c->first_edge + (k == c->num_edges ? 0 : k));
}

// The twin of a portal edge: the adjacent cell's edge running the shared
// wall the other way (it starts at our end vertex), stored by the builder.
static inline uint8_t pt_map_twin(const pt_map *m, uint8_t e)
{
    return m->edges[e].twin;
}

// Build the demo scene: a pentagonal room with a depressed square in the
// centre of its floor holding a boulder, and a corridor to a second, square
// room with a thin pillar in its middle.
void pt_scene_init(pt_map *m);

// Check every structural invariant (twins, convexity, winding, soft-edge
// attribute equality, collision data, sprites within their cells); trips an
// assert on failure in debug, returns false in release.
bool pt_map_validate(const pt_map *m);

// The signed distance of a point from an edge's line, s16.8: positive on the
// cell's interior side.
int32_t pt_map_edge_side(const pt_map *m, const pt_cell *c, uint8_t e, uint16_t x, uint16_t y);

// True when the point lies inside the cell (boundary included).
bool pt_map_cell_contains(const pt_map *m, uint8_t cell, uint16_t x, uint16_t y);

// The cell containing the point, 0 (the outside) when none does; a point on
// a shared edge goes to the lower-numbered cell.
uint8_t pt_map_find_cell(const pt_map *m, uint16_t x, uint16_t y);

#endif
