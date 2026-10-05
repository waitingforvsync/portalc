// map.h - the world: convex cells with clockwise rings of sides (viewed from
// above with x up the page, y right, z downwards), each side running along
// an edge that the cells either side share. All coordinates are unsigned:
// the scene is authored directly in positive space.

#ifndef PT_MAP_H_
#define PT_MAP_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"
#include "palette.h"
#include "sprite.h"

#define PT_MAX_VERTS 64
#define PT_MAX_CELLS 32
#define PT_MAX_EDGES 128
#define PT_MAX_SIDES 255       // every cell's ring, end to end, a byte indexes it
#define PT_MAX_RING 8          // sides a cell
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

// An edge: what the two sides of a wall share. Where it runs, and between
// which cells, belongs to the rings.
typedef struct pt_edge {
    uint8_t angle;       // its direction within the quadrant, in 1024ths of a
                         // turn: either way round, the quadrant comes from the
                         // direction's signs (pt_quadrant), so the byte serves
                         // both sides
    uint8_t colour;      // the shade + 1 of its faces (wall, riser or
                         // lintel, from whichever side shows them); 0 = soft
    uint8_t len8;        // floor(len / 8): collision's foot cap, in 8-unit
                         // steps (a wall's last few units belong to its post)
} pt_edge;

// A cell's side, one place in its ring: from its start vertex to the next
// side's, clockwise, along an edge.
typedef struct pt_side {
    uint8_t v;           // the start vertex
    uint8_t edge;
    uint8_t adj;         // the cell across it, 0 = solid wall
    uint8_t qt;          // quadrant | (twin << 2): the quadrant of the side's
                         // direction (with its edge's byte, its world angle),
                         // and the edge's position in adj's ring, 0..num (the
                         // child's walk starts from there; 0 for a wall)
} pt_side;

// The quadrant of a side's direction, and its twin's ring position.
static inline uint8_t pt_side_quadrant(const pt_side *sd)
{
    return (uint8_t)(sd->qt & 3u);
}

static inline uint8_t pt_side_twin(const pt_side *sd)
{
    return (uint8_t)(sd->qt >> 2);
}

// The old renderer's view of a side, kept until phase 3 rewrites it. A
// cell's halves fill the same range as its sides, clockwise where its
// sides are counter-clockwise. Derived by the builder.
typedef struct pt_half {
    uint8_t v0;          // start vertex; the end is the next ring half's v0
    uint8_t adj;         // adjacent cell, 0 = solid wall
    uint8_t twin;        // the adjacent cell's half running this one the
                         // other way (a portal's twin; unused for walls)
    uint8_t colour;      // wall shade + 1 (walls, risers and lintels); 0 = soft
} pt_half;

typedef struct pt_cell {
    uint8_t first;       // its ring's base: sides[first + k] is position k
    uint8_t num;         // the ring's count LESS ONE: its last position. The
                         // ring is stored counter-clockwise, so the 6502
                         // walks it clockwise from a base pointer at sides +
                         // first, counting positions down from num and
                         // reloading it below 0
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
    pt_side sides[PT_MAX_SIDES];    // the cells' rings
    pt_half halves[PT_MAX_SIDES];   // the old view: each cell's range of
                                    // sides[], but clockwise
    pt_sprite sprites[PT_MAX_SPRITES];    // sprite 0 is unused ("none")
    pt_sprite_art arts[PT_MAX_ARTS];
    uint8_t num_verts;
    uint8_t num_cells;
    uint8_t num_edges;
    uint8_t num_sides;
    uint8_t num_sprites;
    uint8_t num_arts;
} pt_map;

// A side's direction, a world angle: its quadrant over its edge's byte.
static inline uint16_t pt_map_side_angle(const pt_map *m, const pt_side *sd)
{
    return (uint16_t)((uint16_t)((uint16_t)pt_side_quadrant(sd) << 8) | m->edges[sd->edge].angle);
}

// The side at position k (0..num) of cell c's ring.
static inline const pt_side *pt_map_side(const pt_map *m, const pt_cell *c, uint8_t k)
{
    return &m->sides[c->first + k];
}

// The next position clockwise from k: one down, wrapping round to num.
static inline uint8_t pt_map_ring_next(const pt_cell *c, uint8_t k)
{
    return k == 0 ? c->num : (uint8_t)(k - 1);
}

// Position k's end vertex: the next side's start.
static inline uint8_t pt_map_side_v1(const pt_map *m, const pt_cell *c, uint8_t k)
{
    return pt_map_side(m, c, pt_map_ring_next(c, k))->v;
}

// The old view's next half clockwise from half i (an index into halves[]):
// the one after it.
static inline uint8_t pt_map_half_next(const pt_cell *c, uint8_t i)
{
    uint8_t k = (uint8_t)(i - c->first + 1);
    return (uint8_t)(c->first + (k > c->num ? 0 : k));
}

// The twin of a portal half: the adjacent cell's half running the shared
// wall the other way (the old view's).
static inline uint8_t pt_map_twin(const pt_map *m, uint8_t i)
{
    return m->halves[i].twin;
}

// Build the demo scene: a pentagonal room with a depressed square in the
// centre of its floor holding a boulder, a corridor to a second, square
// room with a thin pillar in its middle, and stairs up to an end room with a
// stepped pit.
void pt_scene_init(pt_map *m);

// Build the stress map: a squat corridor into a tall, deep room whose far
// side opens on three little cells, for the paths the demo scene never
// takes (the Beeb port's map 9).
void pt_stress_scene_init(pt_map *m);

// Check every structural invariant (twins, convexity, winding, soft-edge
// attribute equality, collision data, sprites within their cells); trips an
// assert on failure in debug, returns false in release.
bool pt_map_validate(const pt_map *m);

// The signed distance of a point from the line of the side at position k
// of cell c, s16.8: positive on the cell's interior side.
int32_t pt_map_edge_side(const pt_map *m, const pt_cell *c, uint8_t k, uint16_t x, uint16_t y);

// True when the point lies inside the cell (boundary included).
bool pt_map_cell_contains(const pt_map *m, uint8_t cell, uint16_t x, uint16_t y);

// The cell containing the point, 0 (the outside) when none does; a point on
// a shared edge goes to the lower-numbered cell.
uint8_t pt_map_find_cell(const pt_map *m, uint16_t x, uint16_t y);

#endif
