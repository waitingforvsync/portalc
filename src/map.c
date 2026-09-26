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

static uint8_t begin_cell(builder *b, uint16_t floor_z, uint16_t ceil_z, uint8_t floor,
                          uint8_t ceil)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_cells < PT_MAX_CELLS);
    RC_ASSERT(ceil_z < floor_z);
    // heights come in 8-unit steps and span a byte of them
    RC_ASSERT((floor_z & 7) == 0 && (ceil_z & 7) == 0 && floor_z <= 8 * 255);

    b->cur_cell = m->num_cells++;
    m->cells[b->cur_cell] = (pt_cell) {
        .first_edge = m->num_edges,
        .num_edges = 0,
        .floor_z = (uint8_t)(floor_z >> 3),
        .ceil_z = (uint8_t)(ceil_z >> 3),
        .floor = floor,
        .ceil = ceil,
    };
    return b->cur_cell;
}

static void add_edge(builder *b, uint8_t v, uint8_t colour)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_edges < PT_MAX_EDGES);
    RC_ASSERT(b->cur_cell != 0);

    pt_cell *c = &m->cells[b->cur_cell];
    RC_ASSERT(c->num_edges < PT_MAX_CELL_EDGES);

    m->edges[m->num_edges++] = (pt_edge) {
        .v0 = v,
        .cell = b->cur_cell,
        .colour = colour,
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
            }
        }
        RC_ASSERT(e->colour != PT_EDGE_SOFT || e->adj != 0);
    }

    // collision data: the edge direction as an angle (the camera works in
    // the edge's frame through pt_rot), and the quantised length
    for (uint8_t i = 0; i < m->num_edges; i++) {
        pt_edge *e = &m->edges[i];
        pt_vertex a = m->verts[e->v0];
        pt_vertex q = m->verts[edge_v1(m, i)];
        double dx = (double)q.x - a.x;
        double dy = (double)q.y - a.y;
        double len = sqrt(dx * dx + dy * dy);
        // the upper bound keeps the transition band's cross product in s16
        // with the quantised angle's tilt included
        RC_ASSERT(len >= 1.0 && len < 896.0);

        e->angle = (uint8_t)lround(atan2(dy, dx) * (128.0 / 3.14159265358979323846));
        e->len8 = (uint8_t)((uint32_t)len / 8);
    }

    // a twin's angle is exactly the reverse of its partner's, never its own
    // independent rounding, so both sides of a portal work against one line
    for (uint8_t i = 0; i < m->num_edges; i++) {
        pt_edge *e = &m->edges[i];
        if (e->adj != 0) {
            uint8_t t = pt_map_twin(m, i);
            if (t < i)
                e->angle = (uint8_t)(m->edges[t].angle + 128);
        }
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
                uint8_t ti = pt_map_twin(m, ei);
                const pt_edge *t = &m->edges[ti];
                if (t->cell != e->adj || t->adj != e->cell || pt_map_twin(m, ti) != ei)
                    return false;
                if (t->v0 != edge_v1(m, ei) || edge_v1(m, ti) != e->v0)
                    return false;
                if (e->colour == PT_EDGE_SOFT) {
                    const pt_cell *a = &m->cells[e->cell];
                    const pt_cell *b = &m->cells[e->adj];
                    if (t->colour != PT_EDGE_SOFT || a->floor_z != b->floor_z
                        || a->ceil_z != b->ceil_z || a->floor != b->floor || a->ceil != b->ceil)
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
            if (pt_map_edge_side(m, ei, (uint16_t)cx, (uint16_t)cy) <= 0)
                return false;
        }
    }
    return true;
}

// ---- queries ----

int32_t pt_map_edge_side(const pt_map *m, uint8_t e, uint16_t x, uint16_t y)
{
    // the exact cross product against the line through the edge's vertices:
    // the same geometry the camera's transition band test decides by, so
    // point location and transitions can never disagree
    const pt_edge *ed = &m->edges[e];
    pt_vertex a = m->verts[ed->v0];
    pt_vertex b = m->verts[m->edges[pt_map_ring_next(m, e)].v0];
    return (int32_t)((int16_t)(b.x - a.x)) * (int16_t)(y - a.y)
         - (int32_t)((int16_t)(b.y - a.y)) * (int16_t)(x - a.x);
}

uint8_t pt_map_twin(const pt_map *m, uint8_t e)
{
    RC_ASSERT(m->edges[e].adj != 0);

    // the twin starts at our end vertex: rings are clockwise, so the shared
    // wall runs the other way round in the neighbour
    uint8_t v1 = m->edges[pt_map_ring_next(m, e)].v0;
    const pt_cell *c = &m->cells[m->edges[e].adj];
    for (uint8_t k = 0; k < c->num_edges; k++) {
        uint8_t t = (uint8_t)(c->first_edge + k);
        if (m->edges[t].v0 == v1)
            return t;
    }
    RC_PANIC(0);
    return 0;
}

bool pt_map_cell_contains(const pt_map *m, uint8_t cell, uint16_t x, uint16_t y)
{
    const pt_cell *c = &m->cells[cell];
    for (uint8_t k = 0; k < c->num_edges; k++)
        if (pt_map_edge_side(m, (uint8_t)(c->first_edge + k), x, y) < 0)
            return false;
    return true;
}

uint8_t pt_map_find_cell(const pt_map *m, uint16_t x, uint16_t y)
{
    for (uint8_t c = 1; c < m->num_cells; c++)
        if (pt_map_cell_contains(m, c, x, y))
            return c;
    return 0;
}

// ---- the demo scene ----

void pt_scene_init(pt_map *m)
{
    builder b = begin_map(m);

    // Pentagon (clockwise: x up the page, y right), central square, corridor
    // doorway split points, corridor far end, and the second room's corners.
    uint8_t p0 = add_vert(&b, 1936, 1536);
    uint8_t p1 = add_vert(&b, 1660, 1916);
    uint8_t p2 = add_vert(&b, 1212, 1771);
    uint8_t p3 = add_vert(&b, 1212, 1301);
    uint8_t p4 = add_vert(&b, 1660, 1156);
    uint8_t s0 = add_vert(&b, 1636, 1436);
    uint8_t s1 = add_vert(&b, 1636, 1636);
    uint8_t s2 = add_vert(&b, 1436, 1636);
    uint8_t s3 = add_vert(&b, 1436, 1436);
    uint8_t d0 = add_vert(&b, 1212, 1616);
    uint8_t d1 = add_vert(&b, 1212, 1456);
    uint8_t w0 = add_vert(&b, 912, 1616);
    uint8_t w1 = add_vert(&b, 912, 1456);
    uint8_t r0 = add_vert(&b, 912, 1336);
    uint8_t r3 = add_vert(&b, 912, 1736);
    uint8_t r4 = add_vert(&b, 512, 1736);
    uint8_t r5 = add_vert(&b, 512, 1336);

    // The staircase shaft out of the second room's west wall: seven cell
    // boundaries at 40-unit intervals, on the shaft's north (x 772) and
    // south (x 652) sides; nv[0]/sv[0] are also the doorway split points.
    uint8_t nv[7];
    uint8_t sv[7];
    for (int32_t i = 0; i < 7; i++) {
        nv[i] = add_vert(&b, 772, (uint16_t)(1336 - 40 * i));
        sv[i] = add_vert(&b, 652, (uint16_t)(1336 - 40 * i));
    }

    // Past the stairs the corridor runs on a little, then bends twice by
    // roughly thirty degrees, ending in a doorway into the end room; each
    // joint edge is the mitre of its two side walls, and the split door
    // wall's points are collinear so it reads as one straight wall.
    uint8_t m1n = add_vert(&b, 772, 1012);
    uint8_t m1s = add_vert(&b, 652, 980);
    uint8_t m2n = add_vert(&b, 816, 935);
    uint8_t m2s = add_vert(&b, 728, 847);
    uint8_t en = add_vert(&b, 912, 876);
    uint8_t es = add_vert(&b, 856, 778);
    uint8_t ra = add_vert(&b, 1012, 1051);
    uint8_t rb = add_vert(&b, 756, 603);
    uint8_t rc = add_vert(&b, 1260, 315);
    uint8_t rd = add_vert(&b, 1516, 763);

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

    // horizontal surfaces: catalogue shade indices
    uint8_t main_floor = PT_SH_KRRR;
    uint8_t main_ceil = PT_SH_KKKB;
    uint8_t pit_floor = PT_SH_GGGG;    // the one pure green
    uint8_t corr_ceil = PT_SH_KKRR;
    uint8_t end_ceil = PT_SH_KKKR;
    uint8_t room2_floor = PT_SH_KKGG;

    // walls by facing, darkest (away from the light) to brightest (toward):
    // edge colours, a shade + 1
    uint8_t pit_dark = PT_EDGE_COLOUR(PT_SH_KKKK);           // small face only
    uint8_t lit_shadow = PT_EDGE_COLOUR(PT_SH_KKBB);
    uint8_t lit_se = PT_EDGE_COLOUR(PT_SH_BBBB);
    uint8_t lit_w = PT_EDGE_COLOUR(PT_SH_KBBB);
    uint8_t lit_wnw = PT_EDGE_COLOUR(PT_SH_BBBR);
    uint8_t lit_e = PT_EDGE_COLOUR(PT_SH_BBRR);
    uint8_t lit_e2 = PT_EDGE_COLOUR(PT_SH_BRRR);
    uint8_t room2_far = PT_EDGE_COLOUR(PT_SH_KBRR);          // room 2's far wall only
    uint8_t lit_ene = PT_EDGE_COLOUR(PT_SH_RRRR);
    uint8_t lit_n = PT_EDGE_COLOUR(PT_SH_RRGG);              // fully lit
    uint8_t lintel2 = PT_EDGE_COLOUR(PT_SH_KKRR);
    uint8_t none = PT_EDGE_COLOUR(PT_SH_KKKK);    // featureless black

    // The staircase, elbow and end room use shades of blue only: the four
    // quarter-density rungs, darkest for the treads and ceilings, brightest
    // for the south walls facing the light.
    uint8_t blue25 = PT_SH_KKKB;    // treads and ceilings (a cell surface)
    uint8_t blue50 = PT_EDGE_COLOUR(PT_SH_KKBB);
    uint8_t blue75 = PT_EDGE_COLOUR(PT_SH_KBBB);
    uint8_t blue100 = PT_EDGE_COLOUR(PT_SH_BBBB);

    // Cell 1: the depressed central square (floor 24 units below the ring's);
    // its edge colours are the riser faces seen from inside the pit, lit like
    // the walls: the south riser faces the light, the north riser is dark.
    begin_cell(&b, 536, 352, pit_floor, main_ceil);
    add_edge(&b, s0, pit_dark);   // north riser, faces south, in shadow
    add_edge(&b, s1, lit_w);      // east riser, faces west
    add_edge(&b, s2, lit_n);      // south riser, faces north
    add_edge(&b, s3, lit_e);      // west riser, faces east

    // Cells 2..6: the pentagon ring around the square, split convex by soft
    // spokes; all five share identical attributes. The five wall orientations
    // give five distinct brightnesses. The colours of the ring's pit edges
    // are never drawn (the drop face points away).
    begin_cell(&b, 512, 352, main_floor, main_ceil);    // A
    add_edge(&b, p0, lit_shadow);    // faces south-west, fully away
    add_edge(&b, p1, PT_EDGE_SOFT);
    add_edge(&b, s1, none);
    add_edge(&b, s0, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, main_ceil);    // B
    add_edge(&b, p1, lit_wnw);    // faces west-north-west
    add_edge(&b, p2, PT_EDGE_SOFT);
    add_edge(&b, s2, none);
    add_edge(&b, s1, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, main_ceil);    // C, holds the doorway
    add_edge(&b, p2, lit_n);      // faces north
    add_edge(&b, d0, lit_n);      // the doorway portal (lintel face, stepped)
    add_edge(&b, d1, lit_n);
    add_edge(&b, p3, PT_EDGE_SOFT);
    add_edge(&b, s3, none);
    add_edge(&b, s2, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, main_ceil);    // D
    add_edge(&b, p3, lit_ene);    // faces east-north-east
    add_edge(&b, p4, PT_EDGE_SOFT);
    add_edge(&b, s0, none);
    add_edge(&b, s3, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, main_ceil);    // E
    add_edge(&b, p4, lit_se);     // faces south-east
    add_edge(&b, p0, PT_EDGE_SOFT);
    add_edge(&b, s0, PT_EDGE_SOFT);

    // Cell 7: the corridor, with a lower ceiling than the rooms it joins; the
    // room 2 portal's colour is the step riser seen from the corridor, fully
    // lit down the corridor's axis. The pentagon portal's colour is never
    // drawn (the opening is full height from this side). The corridor's
    // floor runs on from the pentagon's, so the doorway shows no seam.
    begin_cell(&b, 512, 384, main_floor, corr_ceil);
    add_edge(&b, d1, none);
    add_edge(&b, d0, lit_w);      // east wall, faces west
    add_edge(&b, w0, none);       // portal into room 2 (never drawn)
    add_edge(&b, w1, lit_e);      // west wall, faces east

    // Cell 8: the second room, floor a step down from the corridor; its
    // portal edge colours both the lintel over the way in and the riser of
    // the step back up. The north wall faces away from the light, so the
    // room darkens toward the way in.
    begin_cell(&b, 544, 352, room2_floor, main_ceil);
    add_edge(&b, r0, lit_shadow);    // north wall, faces south
    add_edge(&b, w1, lintel2);       // portal back to the corridor (lintel)
    add_edge(&b, w0, lit_shadow);
    add_edge(&b, r3, lit_wnw);       // east wall, faces west
    add_edge(&b, r4, room2_far);     // far wall (black-blue-red mix)
    add_edge(&b, r5, lit_e2);        // west wall up to the stair doorway
    add_edge(&b, sv[0], blue75);     // stair doorway (first riser face)
    add_edge(&b, nv[0], lit_e2);     // west wall beyond the doorway

    // Cells 9..14: the staircase, six cells climbing 16 units each toward
    // the west under one flat ceiling at the upper corridor's height; the
    // first cell's east edge carries the lintel over the doorway seen from
    // inside the stairwell, and west edges the risers seen climbing.
    for (int32_t i = 1; i <= 6; i++) {
        begin_cell(&b, (uint16_t)(544 - 16 * i), 336, blue25, blue25);
        add_edge(&b, nv[i - 1], i == 1 ? blue75 : none);
        add_edge(&b, sv[i - 1], blue100);    // south wall, faces the light
        add_edge(&b, sv[i], i == 6 ? none : blue75);
        add_edge(&b, nv[i], blue50);         // north wall, in shadow
    }

    // Cells 15..17: the upper corridor, three cells joined at mitred bends;
    // the lit south side and shadowed north side each keep one shade, so
    // the bends read as a soft curve. The door jambs at the end frame the
    // opening a rung brighter than the north wall they join.
    begin_cell(&b, 448, 336, blue25, blue25);    // straight run
    add_edge(&b, nv[6], none);    // portal from the top stair
    add_edge(&b, sv[6], blue100); // south wall, faces the light
    add_edge(&b, m1s, none);      // first bend
    add_edge(&b, m1n, blue50);    // north wall, in shadow

    begin_cell(&b, 448, 336, blue25, blue25);    // between bends
    add_edge(&b, m1n, none);
    add_edge(&b, m1s, blue100);
    add_edge(&b, m2s, none);      // second bend
    add_edge(&b, m2n, blue50);

    begin_cell(&b, 448, 336, blue25, blue25);    // to the doorway
    add_edge(&b, m2n, none);
    add_edge(&b, m2s, blue100);
    add_edge(&b, es, none);       // the doorway, the corridor's full width
    add_edge(&b, en, blue50);     // north wall, in shadow

    // Cell 18: the end room, back in the warm shades: level floor, loftier
    // ceiling. The whole door wall is one red face: the pieces either side
    // of the doorway and the lintel above it share the same solid red.
    begin_cell(&b, 448, 288, main_floor, end_ceil);
    add_edge(&b, rd, lintel2);    // north-east wall, faces away
    add_edge(&b, ra, lit_ene);    // door wall, lit
    add_edge(&b, en, lit_ene);    // the doorway (lintel face)
    add_edge(&b, es, lit_ene);    // door wall, lit
    add_edge(&b, rb, lit_n);      // south-west wall, fully lit
    add_edge(&b, rc, PT_EDGE_COLOUR(room2_floor));    // far wall, dark green check

    end_map(&b);
}
