// vcache.c - the vertex cache (see vcache.h).

#include "vcache.h"

#include <stdbool.h>

#include "richc/macros.h"

void pt_vcache_init(pt_vcache *vc)
{
    // only the null entry made, and no vertex has one
    *vc = (pt_vcache) {.num = 1};
}

void pt_vcache_reset(pt_vcache *vc, uint16_t x, uint16_t y)
{
    // each entry clears its own vertex's slot, as the visits do
    RC_ASSERT(vc->num >= 1);
    for (uint8_t i = 1; i < vc->num; i++)
        vc->slot[vc->vert[i]] = 0;
    vc->num = 1;
    vc->x = x;
    vc->y = y;
}

void pt_vcache_move(pt_vcache *vc, uint16_t x, uint16_t y)
{
    if (x != vc->x || y != vc->y)
        pt_vcache_reset(vc, x, y);
}

const pt_polar *pt_vcache_vert(pt_vcache *vc, const pt_map *m, uint8_t v)
{
    RC_ASSERT(vc->num >= 1);
    uint8_t e = vc->slot[v];
    if (e != 0)
        return &vc->polar[e];

    RC_PANIC(vc->num < PT_VC_MAX);
    e = vc->num++;
    vc->vert[e] = v;
    vc->slot[v] = e;
    // the vertex is seen from the middle of the position's whole-unit
    // square, so its offset is odd in half units and never zero: the camera
    // can stand on a vertex (crossing a fan), and every vertex still has an
    // angle. A half unit is an octave off a whole one's log
    pt_vertex w = m->verts[v];
    int16_t dx = pt_i16(2 * ((int32_t)w.x - vc->x) - 1);
    int16_t dy = pt_i16(2 * ((int32_t)w.y - vc->y) - 1);
    pt_polar p = pt_polar_of(dx, dy);
    p.lr = pt_i16((int32_t)p.lr - 256);
    vc->polar[e] = p;
    return &vc->polar[e];
}

// Has the position crossed side s of cell ci, into adj?
// Seen from it, the side's vertices run clockwise from inside the cell:
// the angle from its start to its end is under a half turn. Exactly a half
// turn is the position on the side: the lower-numbered cell keeps it, so
// both cells always give the same answer. None is the position on the
// side's line but past an end, where it isn't this side that's crossed: if
// it's outside the cell at all, it's outside the side on round from that
// end.
static bool side_crossed(pt_vcache *vc, const pt_map *m, const pt_cell *c, uint8_t s,
                         uint8_t ci, uint8_t adj)
{
    uint16_t a0 = pt_vcache_vert(vc, m, pt_map_side(m, s)->v)->angle;
    uint16_t a1 = pt_vcache_vert(vc, m, pt_map_side_v1(m, c, s))->angle;
    uint16_t d = (uint16_t)((uint16_t)(a1 - a0) & 1023u);
    if (d == 512)
        return adj < ci;
    return d > 512;
}

uint8_t pt_vcache_locate(pt_vcache *vc, const pt_map *m, uint8_t cell)
{
    // a move skimming a fan vertex may cross several cells, and can legally
    // walk the long way round the fan: a full ring of hops at most (which
    // the 6502 doesn't count)
    for (uint8_t hops = 0;; hops++) {
        const pt_cell *c = &m->cells[cell];
        bool crossed = false;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            uint8_t adj = pt_map_side(m, s)->adj;
            if (adj != 0 && side_crossed(vc, m, c, s, cell, adj)) {
                cell = adj;
                crossed = true;
                break;
            }
        }
        if (!crossed)
            return cell;
        RC_ASSERT(hops < 8);
    }
}
