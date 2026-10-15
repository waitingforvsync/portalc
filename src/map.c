// map.c - map builder (the stand-in for the editor: matching sides into
// shared edges, the angle bytes, collision precompute; validation may use
// full-width signed arithmetic and floats) and the demo scene. Only the
// stored data obeys the engine's unsigned formats.

#include "map.h"

#include <math.h>
#include <stddef.h>

#include "richc/macros.h"

// ---- builder ----

// A side as the builder first holds it, in the order the scene names them
// (clockwise, as the rings are stored), until end_map has matched the twins
// and made the edges.
typedef struct half {
    uint8_t v0;          // start vertex; the end is the next half's v0
    uint8_t adj;         // the cell across, 0 = solid wall
    uint8_t twin;        // the half running the same edge the other way, in adj
    uint8_t colour;      // the shade + 1 its cell sees; 0 = soft
} half;

typedef struct builder {
    pt_map *m;
    uint8_t cur_cell;
    half halves[PT_MAX_SIDES];    // each cell's range of sides[], clockwise
} builder;

static builder begin_map(pt_map *m)
{
    *m = (pt_map) {0};
    m->num_cells = 1;      // cell 0 is the outside
    m->num_sprites = 1;    // sprite 0 is "none"
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
        .first = m->num_sides,
        .floor_z = (uint8_t)(floor_z >> 3),
        .ceil_z = (uint8_t)(ceil_z >> 3),
        .floor = floor,
        .ceil = ceil,
    };
    return b->cur_cell;
}

static uint8_t add_art(builder *b, const char *const rows[], const uint8_t pal[7])
{
    pt_map *m = b->m;
    RC_PANIC(m->num_arts < PT_MAX_ARTS);
    pt_sprite_encode(&m->arts[m->num_arts], rows, pal);
    return m->num_arts++;
}

pt_sprite pt_map_sprite(const pt_map *m, uint8_t v, uint16_t size, uint8_t art)
{
    RC_ASSERT(art < m->num_arts && size != 0 && v < m->num_verts);
    double h = pt_sprite_height(&m->arts[art]);
    double ls = 256.0 * log2(64.0 * 256.0 * size / h);
    return (pt_sprite) {.v = v, .size = size, .art = art, .ls = (int16_t)lround(ls)};
}

// Stand a sprite in the current cell, on its floor, its base's centre (x, y)
// a vertex of its own.
static void add_sprite(builder *b, uint16_t x, uint16_t y, uint16_t size, uint8_t art)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_sprites < PT_MAX_SPRITES);
    RC_ASSERT(b->cur_cell != 0 && m->cells[b->cur_cell].sprite == 0);
    m->sprites[m->num_sprites] = pt_map_sprite(m, add_vert(b, x, y), size, art);
    m->cells[b->cur_cell].sprite = m->num_sprites++;
}

// The current cell's next side, clockwise, from vertex v to the next side's
// start, in the wall colour its cell sees. The sides are held as halves
// until end_map makes the edges.
static void add_edge(builder *b, uint8_t v, uint8_t colour)
{
    pt_map *m = b->m;
    RC_PANIC(m->num_sides < PT_MAX_SIDES);
    RC_ASSERT(b->cur_cell != 0);

    const pt_cell *c = &m->cells[b->cur_cell];
    RC_ASSERT(m->num_sides - c->first < PT_MAX_RING);

    b->halves[m->num_sides++] = (half) {
        .v0 = v,
        .colour = colour,
    };
}

// The end vertex of a half of cell c: the next one's start.
static uint8_t half_v1(const builder *b, const pt_cell *c, uint8_t i)
{
    return b->halves[pt_map_ring_next(c, i)].v0;
}

// Does a half show a face to its own cell: a solid wall, or a lintel (the
// cell across has a lower ceiling) or a riser (a higher floor)?
static bool shows_face(const pt_map *m, uint8_t ci, const half *h)
{
    if (h->adj == 0)
        return true;
    const pt_cell *a = &m->cells[ci];
    const pt_cell *b = &m->cells[h->adj];
    return b->ceil_z > a->ceil_z || b->floor_z < a->floor_z;
}

// The in-quadrant byte of a direction: the quadrant's start turned to +x,
// then the angle from there, floored to its 1024th (so an axis is 0).
static uint8_t angle_byte(int32_t dx, int32_t dy)
{
    for (uint8_t q = pt_quadrant((int16_t)dx, (int16_t)dy); q != 0; q--) {
        int32_t t = dx;    // a quarter turn back: (x, y) -> (y, -x)
        dx = dy;
        dy = -t;
    }
    RC_ASSERT(dx > 0 && dy >= 0);
    double a = atan2((double)dy, (double)dx) * (512.0 / 3.14159265358979323846);
    return (uint8_t)floor(a);
}

// Matching sides into edges, the halves' twins, and each edge's angle,
// colour and length; editor work, so floats are fine.
static void end_map(builder *b)
{
    pt_map *m = b->m;

    // the last ring's end, past the cells
    m->cells[m->num_cells].first = m->num_sides;

    // match twins: the half running v1 -> v0 in another cell
    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        for (uint8_t i = c->first; i < pt_map_ring_end(c); i++) {
            half *h = &b->halves[i];
            uint8_t v1 = half_v1(b, c, i);
            for (uint8_t cj = 1; cj < m->num_cells; cj++) {
                if (cj == ci)
                    continue;
                const pt_cell *d = &m->cells[cj];
                for (uint8_t j = d->first; j < pt_map_ring_end(d); j++) {
                    if (b->halves[j].v0 == v1 && half_v1(b, d, j) == h->v0) {
                        RC_ASSERT(h->adj == 0);    // exactly one neighbour per side
                        h->adj = cj;
                        h->twin = j;
                    }
                }
            }
            RC_ASSERT(h->colour != PT_EDGE_SOFT || h->adj != 0);
        }
    }

    // an edge for each wall, and for each portal the first time a cell
    // names it, in the colour of whichever side shows a face (both sides
    // showing must agree); then the sides
    uint8_t edge_of[PT_MAX_SIDES];
    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        for (uint8_t i = c->first; i < pt_map_ring_end(c); i++) {
            const half *h = &b->halves[i];
            if (h->adj != 0 && h->adj < ci) {
                edge_of[i] = edge_of[h->twin];
                continue;
            }
            RC_PANIC(m->num_edges < PT_MAX_EDGES);
            uint8_t e = m->num_edges++;
            pt_vertex a = m->verts[h->v0];
            pt_vertex q = m->verts[half_v1(b, c, i)];
            double len = hypot((double)q.x - a.x, (double)q.y - a.y);
            RC_ASSERT(len >= 1.0 && len < 2048.0);
            uint8_t colour = h->colour;
            if (h->adj != 0) {
                const half *t = &b->halves[h->twin];
                bool here = shows_face(m, ci, h);
                bool there = shows_face(m, h->adj, t);
                RC_PANIC((colour == PT_EDGE_SOFT) == (t->colour == PT_EDGE_SOFT));
                RC_PANIC(!(here && there) || t->colour == colour);
                if (there && !here)
                    colour = t->colour;
            }
            m->edges[e] = (pt_edge) {
                .angle = angle_byte((int32_t)q.x - a.x, (int32_t)q.y - a.y),
                .colour = colour,
                .len8 = (uint8_t)((uint32_t)len / 8),
            };
            edge_of[i] = e;
        }
    }
    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        for (uint8_t i = c->first; i < pt_map_ring_end(c); i++) {
            const half *h = &b->halves[i];
            pt_vertex a = m->verts[h->v0];
            pt_vertex q = m->verts[half_v1(b, c, i)];
            m->sides[i] = (pt_side) {
                .v = h->v0,
                .edge = edge_of[i],
                .adj = h->adj,
                .anghi = pt_quadrant((int16_t)((int32_t)q.x - a.x), (int16_t)((int32_t)q.y - a.y)),
            };
        }
    }

    RC_PANIC(pt_map_validate(m));
}

// ---- validation ----

// The cells' rings: each closes up, every portal side's twin (the one side
// of the cell across on the same edge) is the edge the other way round, and
// every edge is named once a side.
static bool rings_valid(const pt_map *m)
{
    uint8_t named[PT_MAX_EDGES] = {0};    // sides naming each edge
    bool wall[PT_MAX_EDGES] = {0};
    if (m->cells[m->num_cells].first != m->num_sides)
        return false;
    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        if (c->first > pt_map_ring_end(c) || pt_map_ring_end(c) > m->num_sides)
            return false;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            const pt_side *sd = pt_map_side(m, s);
            if (sd->edge >= m->num_edges)
                return false;
            named[sd->edge]++;
            wall[sd->edge] = sd->adj == 0;
            if (sd->adj != 0) {
                const pt_cell *d = &m->cells[sd->adj];
                uint8_t twins = 0;
                for (uint8_t t = d->first; t < pt_map_ring_end(d); t++) {
                    const pt_side *td = pt_map_side(m, t);
                    if (td->edge != sd->edge)
                        continue;
                    twins++;
                    if (td->adj != ci || td->v != pt_map_side_v1(m, c, s)
                        || pt_map_side_v1(m, d, t) != sd->v)
                        return false;
                }
                if (twins != 1)
                    return false;
            }
        }
    }
    for (uint8_t e = 0; e < m->num_edges; e++)
        if (named[e] != (wall[e] ? 1 : 2))
            return false;
    return true;
}

// A side's direction against its stored angle: its quadrant is its
// direction's signs', and with its edge's byte it's the true angle's 1024th.
static bool angle_valid(const pt_map *m, const pt_cell *c, uint8_t s)
{
    const pt_side *sd = pt_map_side(m, s);
    pt_vertex a = m->verts[sd->v];
    pt_vertex b = m->verts[pt_map_side_v1(m, c, s)];
    int32_t dx = (int32_t)b.x - a.x;
    int32_t dy = (int32_t)b.y - a.y;
    if (sd->anghi != pt_quadrant((int16_t)dx, (int16_t)dy))
        return false;
    double t = atan2((double)dy, (double)dx) * (512.0 / 3.14159265358979323846);
    if (t < 0)
        t += 1024.0;
    double d = pt_map_side_angle(m, sd) - t;
    if (d > 512.0)
        d -= 1024.0;
    return d >= -1.0 - 1e-9 && d <= 1e-9;
}

bool pt_map_validate(const pt_map *m)
{
    if (!rings_valid(m))
        return false;

    for (uint8_t ci = 1; ci < m->num_cells; ci++) {
        const pt_cell *c = &m->cells[ci];
        if (pt_map_ring_end(c) - c->first < 3)
            return false;

        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            const pt_side *sd = pt_map_side(m, s);
            const pt_edge *e = &m->edges[sd->edge];
            uint8_t sn = pt_map_ring_next(c, s);
            if (!angle_valid(m, c, s))
                return false;
            if (sd->adj == 0 && e->colour == PT_EDGE_SOFT)
                return false;    // a solid wall draws

            // clockwise, convex ring (collinear split points allowed)
            pt_vertex v0 = m->verts[sd->v];
            pt_vertex v1 = m->verts[pt_map_side(m, sn)->v];
            pt_vertex v2 = m->verts[pt_map_side_v1(m, c, sn)];
            int32_t cross = ((int32_t)v1.x - v0.x) * ((int32_t)v2.y - v1.y)
                          - ((int32_t)v1.y - v0.y) * ((int32_t)v2.x - v1.x);
            if (cross < 0)
                return false;

            // a soft edge joins cells alike
            if (e->colour == PT_EDGE_SOFT) {
                const pt_cell *b = &m->cells[sd->adj];
                if (c->floor_z != b->floor_z || c->ceil_z != b->ceil_z || c->floor != b->floor
                    || c->ceil != b->ceil)
                    return false;
            }

            // a sprite stands inside its cell: its base centre at least half
            // its width from every edge line (editor work, so floats)
            if (c->sprite != 0) {
                if (c->sprite >= m->num_sprites)
                    return false;
                const pt_sprite *sp = &m->sprites[c->sprite];
                if (sp->art >= m->num_arts || sp->v >= m->num_verts)
                    return false;
                pt_vertex base = m->verts[sp->v];
                const pt_sprite_art *a = &m->arts[sp->art];
                double half = 0.5 * sp->size * pt_sprite_width(a) / pt_sprite_height(a);
                double len = sqrt(((double)v1.x - v0.x) * ((double)v1.x - v0.x)
                                  + ((double)v1.y - v0.y) * ((double)v1.y - v0.y));
                if (pt_map_edge_side(m, c, s, base.x, base.y) < half * len)
                    return false;
                if (sp->size > 8 * (c->floor_z - c->ceil_z))
                    return false;
                // its log made, and a column under 256 pixels wide at the
                // nearest it's drawn, 32 units: 512 size / h in 8.8
                if (sp->ls != pt_map_sprite(m, sp->v, sp->size, sp->art).ls)
                    return false;
                if (512.0 * sp->size / pt_sprite_height(a) >= 65280.0)
                    return false;
            }

            // the inward normal must point at the cell's centroid
            int64_t cx = 0;
            int64_t cy = 0;
            for (uint8_t j = c->first; j < pt_map_ring_end(c); j++) {
                cx += m->verts[pt_map_side(m, j)->v].x;
                cy += m->verts[pt_map_side(m, j)->v].y;
            }
            cx /= pt_map_ring_end(c) - c->first;
            cy /= pt_map_ring_end(c) - c->first;
            if (pt_map_edge_side(m, c, s, (uint16_t)cx, (uint16_t)cy) <= 0)
                return false;
        }
    }
    return true;
}

// ---- queries ----

int32_t pt_map_edge_side(const pt_map *m, const pt_cell *c, uint8_t s, uint16_t x, uint16_t y)
{
    // the exact cross product against the line through the side's vertices
    // (the camera decides its cell by angles instead, which can differ from
    // this within a unit or so of a portal's line)
    pt_vertex a = m->verts[pt_map_side(m, s)->v];
    pt_vertex b = m->verts[pt_map_side_v1(m, c, s)];
    return (int32_t)((int16_t)(b.x - a.x)) * (int16_t)(y - a.y)
         - (int32_t)((int16_t)(b.y - a.y)) * (int16_t)(x - a.x);
}

bool pt_map_cell_contains(const pt_map *m, uint8_t cell, uint16_t x, uint16_t y)
{
    const pt_cell *c = &m->cells[cell];
    for (uint8_t s = c->first; s < pt_map_ring_end(c); s++)
        if (pt_map_edge_side(m, c, s, x, y) < 0)
            return false;
    return true;
}

uint8_t pt_map_twin(const pt_map *m, uint8_t s)
{
    const pt_side *sd = pt_map_side(m, s);
    RC_ASSERT(sd->adj != 0);
    const pt_cell *d = &m->cells[sd->adj];
    uint8_t t = d->first;
    while (pt_map_side(m, t)->edge != sd->edge) {
        t++;
        RC_ASSERT(t < pt_map_ring_end(d));
    }
    return t;
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

    // A thin square pillar in the middle of the second room, in line with
    // both the corridor and the stair doorway.
    uint8_t pnw = add_vert(&b, 724, 1524);
    uint8_t pne = add_vert(&b, 724, 1548);
    uint8_t pse = add_vert(&b, 700, 1548);
    uint8_t psw = add_vert(&b, 700, 1524);

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

    // The end room's pit, a quad sunk in three steps, and where its spokes
    // meet the walls. Seen from the doorway its near edge is split at q
    // (into the mid and shallow steps), and the deep step behind is cut off
    // from the mid one by l-p; p and q sit exactly halfway along their
    // edges, so the ring cells see one straight side each.
    uint8_t ql = add_vert(&b, 1107, 602);
    uint8_t qt = add_vert(&b, 1251, 623);
    uint8_t qr = add_vert(&b, 1273, 787);
    uint8_t qs = add_vert(&b, 1111, 776);
    uint8_t qp = add_vert(&b, 1262, 705);    // halfway from qt to qr
    uint8_t qq = add_vert(&b, 1109, 689);    // halfway from ql to qs
    uint8_t ka = add_vert(&b, 964, 967);     // on the door wall
    uint8_t kb = add_vert(&b, 1320, 875);    // on the north-east wall
    uint8_t kc = add_vert(&b, 1388, 539);    // on the far wall
    uint8_t kd = add_vert(&b, 1022, 451);    // on the south-west wall

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
    uint8_t room1_ceil = PT_SH_KKKB;
    uint8_t room2_ceil = PT_SH_KKKB;
    uint8_t pit_floor = PT_SH_GGGG;    // the one pure green
    uint8_t corr_ceil = PT_SH_KKRR;
    uint8_t end_ceil = PT_SH_KKKR;
    uint8_t room2_floor = PT_SH_KKGG;
    uint8_t pit1_floor = PT_SH_KKRR;    // the end room's pit, step by step down
    uint8_t pit2_floor = PT_SH_KKKR;
    uint8_t pit3_floor = PT_SH_KKKB;

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

    // The boulder, from the Baron sprite demo: 14 x 16, in black, black-red,
    // red and red-green.
    static const char *const boulder_rows[] = {
        "00003333400000",
        "00333344440000",
        "03333344444300",
        "03323334444300",
        "33333333434430",
        "33322333344330",
        "33233323334433",
        "23333223333333",
        "22333333233433",
        "22332333223330",
        "22233323333330",
        "02223333233300",
        "02222333333000",
        "00222223331100",
        "01111111111110",
        "00011111111000",
        NULL,
    };
    static const uint8_t boulder_pal[7] = {PT_SH_KKKK, PT_SH_KKRR, PT_SH_RRRR, PT_SH_RRGG,
                                           PT_SH_KKKK, PT_SH_KKKK, PT_SH_KKKK};
    uint8_t boulder = add_art(&b, boulder_rows, boulder_pal);

    // Cell 1: the depressed central square (floor 24 units below the ring's);
    // its edge colours are the riser faces seen from inside the pit, lit like
    // the walls: the south riser faces the light, the north riser is dark.
    // The boulder stands in its middle, sized to a little over half the
    // screen's height from the start position (220 units away).
    begin_cell(&b, 536, 352, pit_floor, room1_ceil);
    add_sprite(&b, 1536, 1536, 128, boulder);
    add_edge(&b, s0, pit_dark);   // north riser, faces south, in shadow
    add_edge(&b, s1, lit_w);      // east riser, faces west
    add_edge(&b, s2, lit_n);      // south riser, faces north
    add_edge(&b, s3, lit_e);      // west riser, faces east

    // Cells 2..6: the pentagon ring around the square, split convex by soft
    // spokes; all five share identical attributes. The five wall orientations
    // give five distinct brightnesses. The colours of the ring's pit edges
    // are never drawn (the drop face points away).
    begin_cell(&b, 512, 352, main_floor, room1_ceil);    // A
    add_edge(&b, p0, lit_shadow);    // faces south-west, fully away
    add_edge(&b, p1, PT_EDGE_SOFT);
    add_edge(&b, s1, none);
    add_edge(&b, s0, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, room1_ceil);    // B
    add_edge(&b, p1, lit_wnw);    // faces west-north-west
    add_edge(&b, p2, PT_EDGE_SOFT);
    add_edge(&b, s2, none);
    add_edge(&b, s1, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, room1_ceil);    // C, holds the doorway
    add_edge(&b, p2, lit_n);      // faces north
    add_edge(&b, d0, lit_n);      // the doorway portal (lintel face, stepped)
    add_edge(&b, d1, lit_n);
    add_edge(&b, p3, PT_EDGE_SOFT);
    add_edge(&b, s3, none);
    add_edge(&b, s2, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, room1_ceil);    // D
    add_edge(&b, p3, lit_ene);    // faces east-north-east
    add_edge(&b, p4, PT_EDGE_SOFT);
    add_edge(&b, s0, none);
    add_edge(&b, s3, PT_EDGE_SOFT);

    begin_cell(&b, 512, 352, main_floor, room1_ceil);    // E
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

    // Cell 8: the second room's north quarter, floor a step down from the
    // corridor; its portal edge colours both the lintel over the way in and
    // the riser of the step back up. The north wall faces away from the
    // light, so the room darkens toward the way in. The room is split round
    // its pillar into four convex quarters by soft diagonals from the room's
    // corners to the pillar's (cells 8 and 19..21), leaving both doorways
    // whole; each pillar face is lit by its facing like the room's walls.
    begin_cell(&b, 544, 352, room2_floor, room2_ceil);
    add_edge(&b, r0, lit_shadow);    // north wall, faces south
    add_edge(&b, w1, lintel2);       // portal back to the corridor (lintel)
    add_edge(&b, w0, lit_shadow);
    add_edge(&b, r3, PT_EDGE_SOFT);
    add_edge(&b, pne, lit_n);        // pillar north face
    add_edge(&b, pnw, PT_EDGE_SOFT);

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

    // Cell 18: the end room's near quarter, back in the warm shades: level
    // floor, loftier ceiling. The room is a ring of four cells (18 and
    // 22..24) round a pit of three (25..27). The whole door wall is one red
    // face: the pieces either side of the doorway and the lintel above it
    // share the same solid red. The pit's edges draw nothing from up here
    // (the drop faces away); its risers are lit like the walls, seen from
    // below.
    begin_cell(&b, 448, 288, main_floor, end_ceil);
    add_edge(&b, rb, lit_n);      // south-west wall, fully lit
    add_edge(&b, kd, PT_EDGE_SOFT);
    add_edge(&b, ql, none);       // into the mid step
    add_edge(&b, qq, none);       // into the shallow step
    add_edge(&b, qs, PT_EDGE_SOFT);
    add_edge(&b, ka, lit_ene);    // door wall, lit
    add_edge(&b, en, lit_ene);    // the doorway (lintel face)
    add_edge(&b, es, lit_ene);    // door wall, lit

    // Cells 19..21: the rest of the second room round the pillar (east,
    // south and west quarters).
    begin_cell(&b, 544, 352, room2_floor, room2_ceil);
    add_edge(&b, r3, lit_wnw);       // east wall, faces west
    add_edge(&b, r4, PT_EDGE_SOFT);
    add_edge(&b, pse, lit_e);        // pillar east face
    add_edge(&b, pne, PT_EDGE_SOFT);

    begin_cell(&b, 544, 352, room2_floor, room2_ceil);
    add_edge(&b, r4, room2_far);     // far wall (black-blue-red mix)
    add_edge(&b, r5, PT_EDGE_SOFT);
    add_edge(&b, psw, lit_shadow);   // pillar south face
    add_edge(&b, pse, PT_EDGE_SOFT);

    begin_cell(&b, 544, 352, room2_floor, room2_ceil);
    add_edge(&b, r5, lit_e2);        // west wall up to the stair doorway
    add_edge(&b, sv[0], blue75);     // stair doorway (first riser face)
    add_edge(&b, nv[0], lit_e2);     // west wall beyond the doorway
    add_edge(&b, r0, PT_EDGE_SOFT);
    add_edge(&b, pnw, lit_w);        // pillar west face
    add_edge(&b, psw, PT_EDGE_SOFT);

    // Cells 22..24: the rest of the end room's ring (east, far and west).
    begin_cell(&b, 448, 288, main_floor, end_ceil);
    add_edge(&b, qs, none);       // into the shallow step
    add_edge(&b, qr, PT_EDGE_SOFT);
    add_edge(&b, kb, lintel2);    // north-east wall, faces away
    add_edge(&b, ra, lit_ene);    // door wall, lit
    add_edge(&b, ka, PT_EDGE_SOFT);

    begin_cell(&b, 448, 288, main_floor, end_ceil);
    add_edge(&b, qt, PT_EDGE_SOFT);
    add_edge(&b, kc, PT_EDGE_COLOUR(room2_floor));    // far wall, dark green check
    add_edge(&b, rd, lintel2);
    add_edge(&b, kb, PT_EDGE_SOFT);
    add_edge(&b, qr, none);       // into the shallow step
    add_edge(&b, qp, none);       // into the deep step

    begin_cell(&b, 448, 288, main_floor, end_ceil);
    add_edge(&b, kd, lit_n);
    add_edge(&b, rc, PT_EDGE_COLOUR(room2_floor));
    add_edge(&b, kc, PT_EDGE_SOFT);
    add_edge(&b, qt, none);       // into the deep step
    add_edge(&b, ql, PT_EDGE_SOFT);

    // Cells 25..27: the pit, deep, mid and shallow steps, darker the deeper.
    begin_cell(&b, 528, 288, pit3_floor, end_ceil);
    add_edge(&b, ql, lit_se);     // up to the west quarter
    add_edge(&b, qt, lit_shadow); // up to the far quarter, faces away
    add_edge(&b, qp, lit_wnw);    // up to the mid step

    begin_cell(&b, 480, 288, pit2_floor, end_ceil);
    add_edge(&b, ql, none);       // down into the deep step
    add_edge(&b, qp, lit_w);      // up to the shallow step
    add_edge(&b, qq, lit_n);      // up to the near quarter, fully lit

    begin_cell(&b, 464, 288, pit1_floor, end_ceil);
    add_edge(&b, qq, none);       // down into the mid step
    add_edge(&b, qp, lit_shadow); // up to the far quarter, faces away
    add_edge(&b, qr, lit_w);      // up to the east quarter
    add_edge(&b, qs, lit_n);      // up to the near quarter, fully lit

    end_map(&b);
}

// ---- the stress map ----
//
// For the paths the demo scene never takes (beeb/PLAN.md 3.4, 5.5). A squat
// corridor of 17 soft-joined cells runs up +x into a tall, deep room Q,
// whose far side is three portals to little cells R1..R3 (x up the page,
// y right, as the demo scene):
//
//   +            +------------+           +
//   | R1  \      |    R3      |     / R2  |     R1 behind a-b, R3 behind
//   |            b------------c           |     b-c, R2 behind c-d
//   |         /                 \         |
//   |     /           Q             \     |
//   |  /                               \  |
//   a                                     d
//   +-------------------------------------+
//   |              corridor               |     cells 16..0, 0 the wedge
//
// - From corridor cell 0 a line of sight crosses more portals than
//   PT_MAX_DEPTH: the depth cap closes the rest.
// - R1's floor, 48 above the corridor's eye, hides behind the corridor's
//   lintel (32 above) where a-b is close enough behind it: closed columns
//   from the floor line, at a-b's near end, the left.
// - R2's ceiling, 160 below the eye, hides under the corridor's floor edge
//   (80 below) the same way: closed columns from the ceiling line, at c-d's
//   near end, the right.
// - R3 is a slot 8 units tall: far off, a sliver for rounding to pinch.
// - The corridor's near end is a wedge (65 degrees at the tip), where the
//   camera's slide wedges and the move is blocked.
// - An obelisk in Q, taller than the corridor's lintel lets through, and a
//   one-unit one down the corridor, too small to scale from far off.
//
// Built with the same builder as the demo scene, so it passes
// pt_map_validate like it.

#define STRESS_X0 256
#define STRESS_Y0 1024
#define STRESS_LEN 48
#define STRESS_WIDTH 192
#define STRESS_CELLS 17

void pt_stress_scene_init(pt_map *m)
{
    builder b = begin_map(m);
    uint8_t lv[STRESS_CELLS + 1];
    uint8_t rv[STRESS_CELLS + 1];
    for (uint32_t i = 0; i <= STRESS_CELLS; i++) {
        uint16_t x = (uint16_t)(STRESS_X0 + i * STRESS_LEN);
        lv[i] = add_vert(&b, x, STRESS_Y0);
        rv[i] = add_vert(&b, x, STRESS_Y0 + STRESS_WIDTH);
    }
    uint16_t xq = STRESS_X0 + STRESS_CELLS * STRESS_LEN;    // the room's near wall
    uint8_t qa = add_vert(&b, xq + 24, STRESS_Y0);
    uint8_t qb = add_vert(&b, xq + 120, STRESS_Y0 + 64);
    uint8_t qc = add_vert(&b, xq + 120, STRESS_Y0 + 128);
    uint8_t qd = add_vert(&b, xq + 24, STRESS_Y0 + STRESS_WIDTH);
    uint8_t r1 = add_vert(&b, xq + 168, STRESS_Y0);
    uint8_t r3l = add_vert(&b, xq + 168, STRESS_Y0 + 64);
    uint8_t r3r = add_vert(&b, xq + 168, STRESS_Y0 + 128);
    uint8_t r2 = add_vert(&b, xq + 168, STRESS_Y0 + STRESS_WIDTH);
    uint8_t tip = add_vert(&b, STRESS_X0 - 150, STRESS_Y0 + STRESS_WIDTH / 2);

    static const char *const obelisk_rows[] = {
        "0110", "0110", "1221", "1221", "1221", "1221", "1221", "1221",
        "1221", "1221", "1221", "1221", "1221", "1221", "1221", "1221",
        "1331", "1331", "1331", "1331", "1331", "1331", "1331", "1331",
        "1331", "1331", "1331", "1331", "1331", "1331", "3333", "3333",
        NULL,
    };
    static const uint8_t obelisk_pal[7] = {PT_SH_KKKK, PT_SH_BBRR, PT_SH_RRGG, PT_SH_KKKK,
                                           PT_SH_KKKK, PT_SH_KKKK, PT_SH_KKKK};
    uint8_t obelisk = add_art(&b, obelisk_rows, obelisk_pal);

    uint8_t wall_l = PT_EDGE_COLOUR(PT_SH_BBRR);
    uint8_t wall_r = PT_EDGE_COLOUR(PT_SH_KBBB);
    uint8_t lit = PT_EDGE_COLOUR(PT_SH_RRGG);
    uint8_t dim = PT_EDGE_COLOUR(PT_SH_KKBB);

    // the corridor: floor 480, eye 400, ceiling 368; clockwise from above:
    // far left, far right, near right, near left
    for (uint32_t i = 0; i < STRESS_CELLS; i++) {
        begin_cell(&b, 480, 368, PT_SH_KRRR, PT_SH_KKKB);
        if (i == 14)
            add_sprite(&b, (uint16_t)(STRESS_X0 + 14 * STRESS_LEN + STRESS_LEN / 2),
                       STRESS_Y0 + STRESS_WIDTH / 2, 1, obelisk);
        add_edge(&b, lv[i + 1], i + 1 < STRESS_CELLS ? PT_EDGE_SOFT : lit);
        add_edge(&b, rv[i + 1], wall_r);
        if (i == 0) {
            add_edge(&b, rv[0], wall_r);    // the wedge
            add_edge(&b, tip, wall_l);
        } else {
            add_edge(&b, rv[i], PT_EDGE_SOFT);
        }
        add_edge(&b, lv[i], wall_l);
    }

    // Q: a lofty ceiling, a deep floor
    begin_cell(&b, 600, 336, PT_SH_KKGG, PT_SH_KKKB);
    add_sprite(&b, xq + 60, STRESS_Y0 + STRESS_WIDTH / 2, 256, obelisk);
    add_edge(&b, lv[STRESS_CELLS], wall_l);
    add_edge(&b, qa, lit);    // to R1
    add_edge(&b, qb, dim);    // to R3
    add_edge(&b, qc, lit);    // to R2
    add_edge(&b, qd, wall_r);
    add_edge(&b, rv[STRESS_CELLS], dim);    // back to the corridor

    // R1: a high shelf, 16 tall, floor 48 above the corridor's eye
    begin_cell(&b, 352, 336, PT_SH_RRRR, PT_SH_KKKB);
    add_edge(&b, qa, wall_l);
    add_edge(&b, r1, wall_r);
    add_edge(&b, qb, dim);

    // R3: a slot 8 tall, just above the corridor's eye
    begin_cell(&b, 392, 384, PT_SH_BBBB, PT_SH_KKKB);
    add_edge(&b, qb, wall_l);
    add_edge(&b, r3l, lit);
    add_edge(&b, r3r, wall_r);
    add_edge(&b, qc, dim);

    // R2: a crawlspace in the pit, its ceiling 160 below the corridor's eye
    begin_cell(&b, 600, 560, PT_SH_KKGG, PT_SH_RRRR);
    add_edge(&b, qc, wall_l);
    add_edge(&b, r2, wall_r);
    add_edge(&b, qd, dim);

    end_map(&b);
}
