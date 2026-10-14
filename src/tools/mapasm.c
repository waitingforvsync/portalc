// mapasm.c - the demo scene as Baron source, for beeb/src/mapdata.6502: every
// field a list (map_...), then a table of its own (16-bit fields as low bytes and
// high bytes), each kept within a page. Only what the engine reads goes:
// a sprite's size and an art's palette are the map builder's. Each side's twin
// goes too, which the C searches for, and the shades the colours index.
//
//   portalc_mapasm > mapdata.6502
//   portalc_mapasm --stress > stressdata.6502    (the stress scene, for the walk's check)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "palette.h"

static pt_map m;

static void list(const char *name, const int32_t *v, unsigned n)
{
    printf("map_%s = {", name);
    for (unsigned i = 0; i < n; i++)
        printf(i == 0 ? "%d" : ", %d", (int)v[i]);
    printf("}\n");
}

static void table(const char *label, const char *count, const char *value)
{
    printf("    FITPAGE %s\n.%-15s EQUB %s\n", count, label, value);
}

// An art's encoded length: its last column's runs end on its top row.
static unsigned art_len(const pt_sprite_art *a)
{
    uint8_t w = pt_sprite_width(a);
    uint8_t h = pt_sprite_height(a);
    unsigned n = a->data[3 + w - 1];
    while ((a->data[n++] >> 3) != h - 1u)
        ;
    return n;
}

int main(int argc, char **argv)
{
    static int32_t v[PT_MAX_SIDES];
    char name[32];
    bool stress = argc == 2 && strcmp(argv[1], "--stress") == 0;

    pt_arith_init();
    if (stress) {
        pt_stress_scene_init(&m);
        printf("\\\\ stressdata.6502 - the stress scene (map.c's pt_stress_scene_init), for the "
               "walk's check:\n"
               "\\\\ as mapdata.6502. Made by portalc_mapasm --stress (make map): don't edit "
               "it.\n\n");
    } else {
        pt_scene_init(&m);
        printf("\\\\ mapdata.6502 - the demo scene (map.c's pt_scene_init), a table a field, "
               "each within a page.\n"
               "\\\\ Made by portalc_mapasm (make map): don't edit it. "
               "The map's routines are in map.6502.\n\n");
    }

    printf("\\\\ Where the camera starts (camera.h), and the cell that holds it.\n\n");
    printf("CAM_START_X = %d\nCAM_START_Y = %d\nCAM_START_ANGLE = %d\nCAM_START_CELL = %d\n\n",
           PT_CAM_START_X, PT_CAM_START_Y, PT_CAM_START_ANGLE,
           pt_map_find_cell(&m, PT_CAM_START_X, PT_CAM_START_Y));

    printf("\\\\ The fields, an element an entry. Cell 0 (the outside) and sprite 0 (none) are "
           "there, unused.\n\n");
#define FIELD(nm, n, expr)               \
    do {                                 \
        for (unsigned i = 0; i < (n); i++) \
            v[i] = (int32_t)(expr);      \
        list(nm, v, (n));                \
    } while (0)
    FIELD("vert_x", m.num_verts, m.verts[i].x);
    FIELD("vert_y", m.num_verts, m.verts[i].y);
    FIELD("cell_first", m.num_cells, m.cells[i].first);
    FIELD("cell_floorz", m.num_cells, m.cells[i].floor_z);
    FIELD("cell_ceilz", m.num_cells, m.cells[i].ceil_z);
    FIELD("cell_floor", m.num_cells, m.cells[i].floor);
    FIELD("cell_ceil", m.num_cells, m.cells[i].ceil);
    FIELD("cell_sprite", m.num_cells, m.cells[i].sprite);
    FIELD("edge_angle", m.num_edges, m.edges[i].angle);
    FIELD("edge_colour", m.num_edges, m.edges[i].colour);
    FIELD("edge_len8", m.num_edges, m.edges[i].len8);
    FIELD("side_v", m.num_sides, m.sides[i].v);
    FIELD("side_edge", m.num_sides, m.sides[i].edge);
    FIELD("side_adj", m.num_sides, m.sides[i].adj);
    FIELD("side_anghi", m.num_sides, m.sides[i].anghi);
    FIELD("side_twin", m.num_sides, m.sides[i].adj != 0 ? pt_map_twin(&m, (uint8_t)i) : 0);
    FIELD("sprite_x", m.num_sprites, m.sprites[i].x);
    FIELD("sprite_y", m.num_sprites, m.sprites[i].y);
    FIELD("sprite_art", m.num_sprites, m.sprites[i].art);
    FIELD("sprite_ls", m.num_sprites, m.sprites[i].ls);
    FIELD("art_e", 8u * m.num_arts, m.arts[i / 8].pats[i % 8].e);
    FIELD("art_o", 8u * m.num_arts, m.arts[i / 8].pats[i % 8].o);
    for (uint8_t a = 0; a < m.num_arts; a++) {
        snprintf(name, sizeof name, "art%d", a);
        FIELD(name, art_len(&m.arts[a]), m.arts[a].data[i]);
    }
    FIELD("shade_even", PT_NUM_SHADES, pt_shade((uint8_t)i).e);
    FIELD("shade_odd", PT_NUM_SHADES, pt_shade((uint8_t)i).o);
#undef FIELD

    printf("\nMAP_VERTS = LEN(map_vert_x)\nMAP_CELLS = LEN(map_cell_first)\n"
           "MAP_EDGES = LEN(map_edge_angle)\nMAP_SIDES = LEN(map_side_v)\n"
           "MAP_SPRITES = LEN(map_sprite_x)\nMAP_ARTS = LEN(map_art_e) DIV 8\n"
           "MAP_SHADES = LEN(map_shade_even)\n\n");

    printf("\\\\ The tables. A side's twin is the side across its portal, 0 for a wall. An art's "
           "patterns\n"
           "\\\\ are by art * 8 + colour code, 0 transparent; its data is by art_lo and art_hi. "
           "A shade's\n"
           "\\\\ patterns are palette.c's, the even scanlines' and the odd's.\n\n");
    table("vert_xlo", "MAP_VERTS", "LO(map_vert_x)");
    table("vert_xhi", "MAP_VERTS", "HI(map_vert_x)");
    table("vert_ylo", "MAP_VERTS", "LO(map_vert_y)");
    table("vert_yhi", "MAP_VERTS", "HI(map_vert_y)");
    printf("\n");
    // a ring ends where the next cell's begins: cell_first runs one past the cells, to where the
    // sides end
    table("cell_first", "MAP_CELLS+1", "map_cell_first, MAP_SIDES");
    const char *cell_fields[] = {"floorz", "ceilz", "floor", "ceil", "sprite"};
    for (unsigned i = 0; i < sizeof cell_fields / sizeof *cell_fields; i++) {
        char label[32], value[32];
        snprintf(label, sizeof label, "cell_%s", cell_fields[i]);
        snprintf(value, sizeof value, "map_cell_%s", cell_fields[i]);
        table(label, "MAP_CELLS", value);
    }
    printf("\n");
    table("edge_angle", "MAP_EDGES", "map_edge_angle");
    table("edge_colour", "MAP_EDGES", "map_edge_colour");
    table("edge_len8", "MAP_EDGES", "map_edge_len8");
    printf("\n");
    table("side_v", "MAP_SIDES", "map_side_v");
    table("side_edge", "MAP_SIDES", "map_side_edge");
    table("side_adj", "MAP_SIDES", "map_side_adj");
    table("side_anghi", "MAP_SIDES", "map_side_anghi");
    table("side_twin", "MAP_SIDES", "map_side_twin");
    printf("\n");
    table("sprite_xlo", "MAP_SPRITES", "LO(map_sprite_x)");
    table("sprite_xhi", "MAP_SPRITES", "HI(map_sprite_x)");
    table("sprite_ylo", "MAP_SPRITES", "LO(map_sprite_y)");
    table("sprite_yhi", "MAP_SPRITES", "HI(map_sprite_y)");
    table("sprite_art", "MAP_SPRITES", "map_sprite_art");
    table("sprite_lslo", "MAP_SPRITES", "LO(map_sprite_ls)");
    table("sprite_lshi", "MAP_SPRITES", "HI(map_sprite_ls)");
    printf("\n");
    table("shade_even", "MAP_SHADES", "map_shade_even");
    table("shade_odd", "MAP_SHADES", "map_shade_odd");
    printf("\n");
    table("art_pate", "LEN(map_art_e)", "map_art_e");
    table("art_pato", "LEN(map_art_o)", "map_art_o");
    printf("    FITPAGE MAP_ARTS\n.%-15s EQUB LO({", "art_lo");
    for (uint8_t a = 0; a < m.num_arts; a++)
        printf(a == 0 ? "art%d" : ", art%d", a);
    printf("})\n    FITPAGE MAP_ARTS\n.%-15s EQUB HI({", "art_hi");
    for (uint8_t a = 0; a < m.num_arts; a++)
        printf(a == 0 ? "art%d" : ", art%d", a);
    printf("})\n");
    for (uint8_t a = 0; a < m.num_arts; a++) {
        char label[16], count[32], value[16];
        snprintf(label, sizeof label, "art%d", a);
        snprintf(value, sizeof value, "map_art%d", a);
        snprintf(count, sizeof count, "LEN(%s)", value);
        table(label, count, value);
    }
    return 0;
}
