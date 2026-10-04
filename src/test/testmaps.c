// testmaps.c - map variants shared by the tests and the Beeb port's host tool.

#include "testmaps.h"

bool pt_testmaps_add_sprites(pt_map *m)
{
    static const uint8_t cells[] = {2, 3, 4, 5, 6, 8};
    for (uint32_t i = 0; i < sizeof cells; i++) {
        const pt_cell *c = &m->cells[cells[i]];
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (uint8_t k = 0; k < (c->num + 1); k++) {
            sx += m->verts[m->halves[c->first + k].v0].x;
            sy += m->verts[m->halves[c->first + k].v0].y;
        }
        m->sprites[m->num_sprites] = (pt_sprite) {
            .x = (uint16_t)(sx / (c->num + 1)),
            .y = (uint16_t)(sy / (c->num + 1)),
            .size = 64,
            .art = 0,
        };
        m->cells[cells[i]].sprite = m->num_sprites++;
    }
    return pt_map_validate(m);
}
