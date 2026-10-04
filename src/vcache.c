// vcache.c - the vertex cache (see vcache.h).

#include "vcache.h"

#include "richc/macros.h"

void pt_vcache_reset(pt_vcache *vc, uint16_t x, uint16_t y)
{
    // each entry clears its own cell's slot, as the visits do
    for (uint8_t i = 0; i < vc->num; i++)
        vc->slot[vc->cell[i]] = 0;
    vc->num = 0;
    vc->x = x;
    vc->y = y;
}

void pt_vcache_move(pt_vcache *vc, uint16_t x, uint16_t y)
{
    if (x != vc->x || y != vc->y)
        pt_vcache_reset(vc, x, y);
}

const pt_polar *pt_vcache_cell(pt_vcache *vc, const pt_map *m, uint8_t cell)
{
    if (vc->slot[cell] != 0)
        return vc->polar[vc->slot[cell] - 1];

    RC_PANIC(vc->num < PT_VC_MAX);
    uint8_t e = vc->num++;
    vc->cell[e] = cell;
    vc->slot[cell] = (uint8_t)(e + 1);
    const pt_cell *c = &m->cells[cell];
    for (uint8_t k = 0; k <= c->num; k++) {
        pt_vertex v = m->verts[pt_map_side(m, c, k)->v];
        int16_t dx = (int16_t)(v.x - vc->x);
        int16_t dy = (int16_t)(v.y - vc->y);
        // standing exactly on a vertex: no angle exists, so any will do; it
        // ties the cells round the vertex, and the tie rules settle it
        vc->polar[e][k] = dx == 0 && dy == 0 ? (pt_polar) {0, 0} : pt_polar_of(dx, dy);
    }
    return vc->polar[e];
}
