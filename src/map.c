// map.c - map builder (the stand-in for the editor: twin matching, collision
// precompute, validation may use full-width signed arithmetic) and the demo
// scene. Only the stored data obeys the engine's unsigned formats.

#include "map.h"

#include <math.h>

#include "richc/macros.h"

// ---- builder ----

typedef struct builder {
    pt_map *m;
    uint8_t cur_cell;
} builder;

static builder begin_map(pt_map *m)
{
    *m = (pt_map) {0};
    m->num_cells = 1;    // cell 0 is the outside
    return (builder) {.m = m};
}

static uint8_t add_vert(builder *b, uint16_t x, uint16_t y)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_verts < PT_MAX_VERTS);
    m->verts[m->num_verts] = (pt_vertex) {.x = x, .y = y};
    return m->num_verts++;
}

static uint8_t begin_cell(builder *b, uint16_t floor_z, uint16_t ceil_z, pt_pattern floor,
                          pt_pattern ceil)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_cells < PT_MAX_CELLS);
    RC_ASSERT(ceil_z < floor_z);

    b->cur_cell = m->num_cells++;
    m->cells[b->cur_cell] = (pt_cell) {
        .first_edge = m->num_edges,
        .num_edges = 0,
        .floor_z = floor_z,
        .ceil_z = ceil_z,
        .floor = floor,
        .ceil = ceil,
    };
    return b->cur_cell;
}

static void add_edge(builder *b, uint8_t v, pt_pattern wall, bool soft)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_edges < PT_MAX_EDGES);
    RC_ASSERT(b->cur_cell != 0);

    pt_cell *c = &m->cells[b->cur_cell];
    RC_ASSERT(c->num_edges < PT_MAX_CELL_EDGES);

    m->edges[m->num_edges++] = (pt_edge) {
        .v0 = v,
        .cell = b->cur_cell,
        .soft = soft,
        .wall = wall,
    };
    c->num_edges++;
}

// The end vertex of an edge: the next ring edge's start.
static uint8_t edge_v1(const pt_map *m, uint8_t e)
{
    return m->edges[pt_map_ring_next(m, e)].v0;
}

// Twin matching and collision precompute; editor work, so floats are fine.
static void end_map(builder *b)
{
    pt_map *m = b->m;

    // match twins: the edge running v1 -> v0 in another cell
    for (uint8_t i = 0; i < m->num_edges; i++) {
        pt_edge *e = &m->edges[i];
        uint8_t v1 = edge_v1(m, i);
        for (uint8_t j = 0; j < m->num_edges; j++) {
            if (j == i)
                continue;
            if (m->edges[j].v0 == v1 && edge_v1(m, j) == e->v0) {
                RC_ASSERT(e->adj == 0);    // exactly one neighbour per edge
                e->adj = m->edges[j].cell;
                e->twin = j;
            }
        }
        RC_ASSERT(!e->soft || e->adj != 0);
    }

    // collision data: unit direction, inward normal, offset, length
    for (uint8_t i = 0; i < m->num_edges; i++) {
        pt_edge *e = &m->edges[i];
        pt_vertex a = m->verts[e->v0];
        pt_vertex q = m->verts[edge_v1(m, i)];
        double dx = (double)q.x - a.x;
        double dy = (double)q.y - a.y;
        double len = sqrt(dx * dx + dy * dy);
        RC_ASSERT(len >= 1.0 && len < 65535.0);

        e->ex = (int16_t)lround(dx * 256.0 / len);
        e->ey = (int16_t)lround(dy * 256.0 / len);
        e->nx = (int16_t)-e->ey;    // inward normal (-ey, ex): interior right
        e->ny = e->ex;
        e->d0 = (int32_t)a.x * e->nx + (int32_t)a.y * e->ny;    // s16.8 exactly
        e->len = (uint16_t)lround(len);
    }

    RC_PANIC(pt_map_validate(m));
}

// ---- validation ----

bool pt_map_validate(const pt_map *m)
{
    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        if (c->num_edges < 3)
            return false;

        for (uint8_t k = 0; k < c->num_edges; k++) {
            uint8_t ei = (uint8_t)(c->first_edge + k);
            const pt_edge *e = &m->edges[ei];
            uint8_t en = pt_map_ring_next(m, ei);

            // clockwise, convex ring (collinear split points allowed)
            pt_vertex v0 = m->verts[e->v0];
            pt_vertex v1 = m->verts[m->edges[en].v0];
            pt_vertex v2 = m->verts[edge_v1(m, en)];
            int32_t cross = ((int32_t)v1.x - v0.x) * ((int32_t)v2.y - v1.y)
                          - ((int32_t)v1.y - v0.y) * ((int32_t)v2.x - v1.x);
            if (cross < 0)
                return false;

            // twin symmetry and soft-edge attribute equality
            if (e->adj != 0) {
                const pt_edge *t = &m->edges[e->twin];
                if (t->cell != e->adj || t->adj != e->cell || t->twin != ei)
                    return false;
                if (m->edges[e->twin].v0 != edge_v1(m, ei) || edge_v1(m, e->twin) != e->v0)
                    return false;
                if (e->soft) {
                    const pt_cell *a = &m->cells[e->cell];
                    const pt_cell *b = &m->cells[e->adj];
                    if (!t->soft || a->floor_z != b->floor_z || a->ceil_z != b->ceil_z
                        || a->floor.e != b->floor.e || a->floor.o != b->floor.o
                        || a->ceil.e != b->ceil.e || a->ceil.o != b->ceil.o)
                        return false;
                }
            }

            // the inward normal must point at the cell's centroid
            int64_t cx = 0;
            int64_t cy = 0;
            for (uint8_t j = 0; j < c->num_edges; j++) {
                cx += m->verts[m->edges[c->first_edge + j].v0].x;
                cy += m->verts[m->edges[c->first_edge + j].v0].y;
            }
            cx /= c->num_edges;
            cy /= c->num_edges;
            if (cx * e->nx + cy * e->ny - e->d0 <= 0)
                return false;
        }
    }
    return true;
}

// ---- the demo scene ----

// Authored coordinates translated into the unsigned world.
static uint16_t wx(int32_t x)
{
    return (uint16_t)(x + PT_WORLD_OX);
}

static uint16_t wy(int32_t y)
{
    return (uint16_t)(y + PT_WORLD_OY);
}

static uint16_t wz(int32_t z)
{
    return (uint16_t)(z + PT_WORLD_OZ);
}

void pt_scene_build(pt_map *m)
{
    pt_arith_init();

    builder b = begin_map(m);

    // Pentagon (clockwise: x up the page, y right), central square, corridor
    // doorway split points, corridor far end, and the second room's corners.
    uint8_t p0 = add_vert(&b, wx(400), wy(0));
    uint8_t p1 = add_vert(&b, wx(124), wy(380));
    uint8_t p2 = add_vert(&b, wx(-324), wy(235));
    uint8_t p3 = add_vert(&b, wx(-324), wy(-235));
    uint8_t p4 = add_vert(&b, wx(124), wy(-380));
    uint8_t s0 = add_vert(&b, wx(100), wy(-100));
    uint8_t s1 = add_vert(&b, wx(100), wy(100));
    uint8_t s2 = add_vert(&b, wx(-100), wy(100));
    uint8_t s3 = add_vert(&b, wx(-100), wy(-100));
    uint8_t d0 = add_vert(&b, wx(-324), wy(80));
    uint8_t d1 = add_vert(&b, wx(-324), wy(-80));
    uint8_t w0 = add_vert(&b, wx(-624), wy(80));
    uint8_t w1 = add_vert(&b, wx(-624), wy(-80));
    uint8_t r0 = add_vert(&b, wx(-624), wy(-200));
    uint8_t r3 = add_vert(&b, wx(-624), wy(200));
    uint8_t r4 = add_vert(&b, wx(-1024), wy(200));
    uint8_t r5 = add_vert(&b, wx(-1024), wy(-200));

    // Lighting: one fixed light shining from the north-north-east (x up the
    // page, y right; the toward-light direction is roughly (0.94, 0.34)).
    // Each visible wall face takes a shade whose brightness follows the dot
    // of its inward normal with that direction, so faces turned toward the
    // light read brighter and walls meeting at a corner always differ.
    // Every shade is a two-colour blend of luminance neighbours (black-blue,
    // blue-red, red-green, black-red for shadowed reds): mixes of more
    // colours put a hue per scanline and read as stripes on the doubled
    // rows. Solid black is kept for one small face (the pit's shadow-side
    // riser), never a full wall. Floors and ceilings come from the dark end
    // of the range, and the pit floor stays the only pure saturated green.

    // horizontal surfaces
    pt_pattern main_floor = pt_pattern_blend(PT_BLACK, PT_RED, 6);
    pt_pattern main_ceil = pt_pattern_blend(PT_BLACK, PT_BLUE, 2);
    pt_pattern pit_floor = pt_pattern_solid(PT_GREEN);    // the one pure green
    pt_pattern corr_ceil = pt_pattern_blend(PT_BLACK, PT_RED, 2);
    pt_pattern room2_floor = pt_pattern_blend(PT_BLACK, PT_GREEN, 4);

    // walls by facing, darkest (away from the light) to brightest (toward)
    pt_pattern pit_dark = pt_pattern_solid(PT_BLACK);              // small face only
    pt_pattern lit_shadow = pt_pattern_blend(PT_BLACK, PT_BLUE, 4);
    pt_pattern lit_se = pt_pattern_solid(PT_BLUE);
    pt_pattern lit_w = pt_pattern_blend(PT_BLACK, PT_BLUE, 6);
    pt_pattern lit_wnw = pt_pattern_blend(PT_BLUE, PT_RED, 2);
    pt_pattern lit_e = pt_pattern_blend(PT_BLUE, PT_RED, 4);
    pt_pattern lit_e2 = pt_pattern_blend(PT_BLUE, PT_RED, 6);
    pt_pattern lit_ene = pt_pattern_solid(PT_RED);
    pt_pattern lit_n = pt_pattern_blend(PT_RED, PT_GREEN, 4);      // fully lit
    pt_pattern lintel2 = pt_pattern_blend(PT_BLACK, PT_RED, 4);
    pt_pattern none = {0};

    // Cell 1: the depressed central square (floor 24 units below the ring's);
    // its edge colours are the riser faces seen from inside the pit, lit like
    // the walls: the south riser faces the light, the north riser is dark.
    begin_cell(&b, wz(24), wz(-160), pit_floor, main_ceil);
    add_edge(&b, s0, pit_dark, false);   // north riser, faces south, in shadow
    add_edge(&b, s1, lit_w, false);      // east riser, faces west
    add_edge(&b, s2, lit_n, false);      // south riser, faces north
    add_edge(&b, s3, lit_e, false);      // west riser, faces east

    // Cells 2..6: the pentagon ring around the square, split convex by soft
    // spokes; all five share identical attributes. The five wall orientations
    // give five distinct brightnesses. The colours of the ring's pit edges
    // are never drawn (the drop face points away).
    begin_cell(&b, wz(0), wz(-160), main_floor, main_ceil);    // A
    add_edge(&b, p0, lit_shadow, false);    // faces south-west, fully away
    add_edge(&b, p1, none, true);
    add_edge(&b, s1, none, false);
    add_edge(&b, s0, none, true);

    begin_cell(&b, wz(0), wz(-160), main_floor, main_ceil);    // B
    add_edge(&b, p1, lit_wnw, false);    // faces west-north-west
    add_edge(&b, p2, none, true);
    add_edge(&b, s2, none, false);
    add_edge(&b, s1, none, true);

    begin_cell(&b, wz(0), wz(-160), main_floor, main_ceil);    // C, holds the doorway
    add_edge(&b, p2, lit_n, false);      // faces north
    add_edge(&b, d0, lit_ene, false);    // the doorway portal (lintel face, stepped)
    add_edge(&b, d1, lit_n, false);
    add_edge(&b, p3, none, true);
    add_edge(&b, s3, none, false);
    add_edge(&b, s2, none, true);

    begin_cell(&b, wz(0), wz(-160), main_floor, main_ceil);    // D
    add_edge(&b, p3, lit_ene, false);    // faces east-north-east
    add_edge(&b, p4, none, true);
    add_edge(&b, s0, none, false);
    add_edge(&b, s3, none, true);

    begin_cell(&b, wz(0), wz(-160), main_floor, main_ceil);    // E
    add_edge(&b, p4, lit_se, false);     // faces south-east
    add_edge(&b, p0, none, true);
    add_edge(&b, s0, none, true);

    // Cell 7: the corridor, with a lower ceiling than the rooms it joins; the
    // room 2 portal's colour is the step riser seen from the corridor, fully
    // lit down the corridor's axis. The pentagon portal's colour is never
    // drawn (the opening is full height from this side). The corridor's
    // floor runs on from the pentagon's, so the doorway shows no seam.
    begin_cell(&b, wz(0), wz(-128), main_floor, corr_ceil);
    add_edge(&b, d1, none, false);
    add_edge(&b, d0, lit_w, false);      // east wall, faces west
    add_edge(&b, w0, lit_n, false);      // portal into room 2 (riser face)
    add_edge(&b, w1, lit_e, false);      // west wall, faces east

    // Cell 8: the second room, floor a step up from the corridor; its portal
    // colour is the lintel face over the way back down. The north wall faces
    // away from the light, so the room darkens toward the way in.
    begin_cell(&b, wz(-32), wz(-160), room2_floor, main_ceil);
    add_edge(&b, r0, lit_shadow, false);    // north wall, faces south
    add_edge(&b, w1, lintel2, false);       // portal back to the corridor (lintel)
    add_edge(&b, w0, lit_shadow, false);
    add_edge(&b, r3, lit_wnw, false);       // east wall, faces west
    add_edge(&b, r4, lit_n, false);         // far wall, faces north
    add_edge(&b, r5, lit_e2, false);        // west wall, faces east

    end_map(&b);
}
