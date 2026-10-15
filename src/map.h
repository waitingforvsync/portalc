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
// side's, clockwise, along an edge. Sides are known by their index in the
// map's sides[], which a byte holds.
typedef struct pt_side {
    uint8_t v;           // the start vertex
    uint8_t edge;
    uint8_t adj;         // the cell across it, 0 = solid wall
    uint8_t anghi;       // the quadrant of the side's direction: over its
                         // edge's byte, its world angle
} pt_side;

typedef struct pt_cell {
    uint8_t first;       // its ring's first side, clockwise. The rings run on
                         // end to end, so a ring ends where the next cell's
                         // begins: the 6502 steps a side on with INY, and
                         // back to first at the next cell's
    uint8_t floor_z;     // z in 8-unit steps, increasing downwards: floor_z > ceil_z
    uint8_t ceil_z;
    uint8_t floor;       // catalogue shade indices
    uint8_t ceil;
    uint8_t sprite;      // the cell's sprite, 0 = none
} pt_cell;

// A sprite standing on its cell's floor. It lies wholly inside the cell
// (its width as a disc around the base), and below the cell's ceiling. Its
// base's centre is a vertex of its own, so the vertex cache sees it as it
// sees a cell's corners.
typedef struct pt_sprite {
    uint8_t v;           // the vertex at its base's centre
    uint16_t size;       // height in world units; the width follows the art's aspect
    uint8_t art;         // index into the map's art
    int16_t ls;          // 256 log2(64 * 256 size / h): its scale's log, 8.8,
                         // before its depth's comes off; made with the map
} pt_sprite;

typedef struct pt_map {
    pt_vertex verts[PT_MAX_VERTS];
    pt_cell cells[PT_MAX_CELLS + 1];    // cell 0 is the "outside" and never used;
                                        // past the last, one with only its first,
                                        // where the sides end
    pt_edge edges[PT_MAX_EDGES];
    pt_side sides[PT_MAX_SIDES];    // the cells' rings
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
    return (uint16_t)((uint16_t)((uint16_t)sd->anghi << 8) | m->edges[sd->edge].angle);
}

// Side s.
static inline const pt_side *pt_map_side(const pt_map *m, uint8_t s)
{
    return &m->sides[s];
}

// One past the last side of cell c's ring: the next cell's first.
static inline uint8_t pt_map_ring_end(const pt_cell *c)
{
    return c[1].first;
}

// The next side clockwise from s in cell c's ring: the one after it,
// wrapping round from the last to the first.
static inline uint8_t pt_map_ring_next(const pt_cell *c, uint8_t s)
{
    s++;
    return s == pt_map_ring_end(c) ? c->first : s;
}

// Side s's end vertex: the next side's start.
static inline uint8_t pt_map_side_v1(const pt_map *m, const pt_cell *c, uint8_t s)
{
    return pt_map_side(m, pt_map_ring_next(c, s))->v;
}

// Portal side s's twin: the side of the cell across that runs along the same
// edge, found by searching that cell's ring.
uint8_t pt_map_twin(const pt_map *m, uint8_t s);

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
// attribute equality, collision data, sprites within their cells and
// their scales in range); trips an assert on failure in debug, returns
// false in release.
bool pt_map_validate(const pt_map *m);

// A sprite of art art in m, size units tall, its base's centre at vertex v,
// with its scale's log made (by the host, which builds maps).
pt_sprite pt_map_sprite(const pt_map *m, uint8_t v, uint16_t size, uint8_t art);

// The signed distance of a point from the line of cell c's side s, s16.8:
// positive on the cell's interior side.
int32_t pt_map_edge_side(const pt_map *m, const pt_cell *c, uint8_t s, uint16_t x, uint16_t y);

// True when the point lies inside the cell (boundary included).
bool pt_map_cell_contains(const pt_map *m, uint8_t cell, uint16_t x, uint16_t y);

// The cell containing the point, 0 (the outside) when none does; a point on
// a shared edge goes to the lower-numbered cell.
uint8_t pt_map_find_cell(const pt_map *m, uint16_t x, uint16_t y);

#endif
