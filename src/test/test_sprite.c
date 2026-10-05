// test_sprite.c - sprite art encoding and the demo scene's boulder. (The
// sprite pass and its tests come back with phase 5.)

#include <string.h>

#include "arith.h"
#include "map.h"
#include "richc/test.h"
#include "sprite.h"

// Every pixel decodes back to the code it was encoded from.
static void check_round_trip(const char *const rows[])
{
    static pt_sprite_art a;
    static const uint8_t pal[7] = {1, 2, 3, 4, 5, 6, 7};
    pt_sprite_encode(&a, rows, pal);
    uint8_t h = 0;
    while (h < PT_SPRITE_MAX_ROWS && rows[h] != NULL)
        h++;
    RC_CHECK(pt_sprite_height(&a), ==, h);
    for (uint8_t r = 0; r < h; r++)
        for (uint8_t c = 0; c < pt_sprite_width(&a); c++)
            RC_CHECK(pt_sprite_code(&a, c, r), ==, (uint8_t)(rows[r][c] - '0'));
}

RC_TEST(sprite, round_trip)
{
    // every code, runs of one row, a colour change on every row, and a
    // column that is one run throughout
    static const char *const small[] = {
        "0123",
        "4567",
        "7777",
        "0000",
        "0707",
        NULL,
    };
    check_round_trip(small);

    // the full height: runs starting on every row up to the last
    static const char *tall[PT_SPRITE_MAX_ROWS];
    static char buf[PT_SPRITE_MAX_ROWS][4];
    for (uint8_t r = 0; r < PT_SPRITE_MAX_ROWS; r++) {
        buf[r][0] = (char)('0' + r % 8);
        buf[r][1] = (char)('0' + (r / 4) % 8);
        buf[r][2] = '5';
        buf[r][3] = 0;
        tall[r] = buf[r];
    }
    check_round_trip(tall);
}

RC_TEST(sprite, format)
{
    static const char *const rows[] = {
        "10",
        "12",
        "22",
        NULL,
    };
    static const uint8_t pal[7] = {0};
    static pt_sprite_art a;
    pt_sprite_encode(&a, rows, pal);

    // header, column offsets, then the runs: column 0 is 1 from row 0 and 2
    // from row 2; column 1 is 0 from row 0 and 2 from row 1; a 0 ends it
    static const uint8_t want[] = {2, 3, 4, 6, 0 << 3 | 1, 2 << 3 | 2, 0 << 3 | 0, 1 << 3 | 2, 0};
    for (uint8_t i = 0; i < sizeof want; i++)
        RC_CHECK(a.data[i], ==, want[i]);
}

RC_TEST(sprite, boulder)
{
    static pt_map m;
    pt_scene_init(&m);

    // the pit (cell 1) holds the boulder: 14 x 16, 128 units tall
    const pt_cell *pit = &m.cells[1];
    RC_CHECK(pit->sprite, !=, (uint8_t)0);
    const pt_sprite *sp = &m.sprites[pit->sprite];
    const pt_sprite_art *a = &m.arts[sp->art];
    RC_CHECK(pt_sprite_width(a), ==, (uint8_t)14);
    RC_CHECK(pt_sprite_height(a), ==, (uint8_t)16);
    RC_CHECK(sp->size, ==, (uint16_t)128);
    RC_CHECK_TRUE(pt_map_cell_contains(&m, 1, sp->x, sp->y));

    // no other cell has one
    for (uint8_t c = 2; c < m.num_cells; c++)
        RC_CHECK(m.cells[c].sprite, ==, (uint8_t)0);
}
