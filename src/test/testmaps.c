// testmaps.c - map variants shared by the tests and the Beeb port's host tool.

#include "testmaps.h"

bool pt_testmaps_add_sprites(pt_map *m)
{
    static const uint8_t cells[] = {2, 3, 4, 5, 6, 8};
    for (uint32_t i = 0; i < sizeof cells; i++) {
        const pt_cell *c = &m->cells[cells[i]];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t s = c->first; s < pt_map_ring_end(c); s++) {
            sx += m->verts[pt_map_side(m, s)->v].x;
            sy += m->verts[pt_map_side(m, s)->v].y;
        }
        if (m->num_verts >= PT_MAX_VERTS)
            return false;
        m->verts[m->num_verts] = (pt_vertex) {.x = (uint16_t)(sx / (pt_map_ring_end(c) - c->first)),
                                              .y = (uint16_t)(sy / (pt_map_ring_end(c) - c->first))};
        m->sprites[m->num_sprites] = pt_map_sprite(m, m->num_verts++, 64, 0);
        m->cells[cells[i]].sprite = m->num_sprites++;
    }
    return pt_map_validate(m);
}
