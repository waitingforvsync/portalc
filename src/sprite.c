// sprite.c - encoding sprite art into the runtime format (on the 6502 the
// assembler's job, as in the Baron demo's sprite macro).

#include "sprite.h"

#include <string.h>

#include "richc/macros.h"

void pt_sprite_encode(pt_sprite_art *a, const char *const rows[], const uint8_t pal[7])
{
    uint8_t w = (uint8_t)strlen(rows[0]);
    uint8_t h = 0;
    while (h < PT_SPRITE_MAX_ROWS && rows[h] != NULL)
        h++;
    RC_ASSERT(w >= 1 && w <= PT_SPRITE_MAX_COLS && h >= 1);
    RC_ASSERT(h == PT_SPRITE_MAX_ROWS || rows[h] == NULL);

    memcpy(a->pal, pal, sizeof a->pal);
    a->data[0] = w;
    a->data[1] = h;
    uint16_t n = (uint16_t)(2 + w);
    for (uint8_t c = 0; c < w; c++) {
        RC_PANIC(n < PT_SPRITE_MAX_BYTES);
        a->data[2 + c] = (uint8_t)n;
        for (uint8_t r = 0; r < h; r++) {
            RC_ASSERT(strlen(rows[r]) == w);
            RC_ASSERT(rows[r][c] >= '0' && rows[r][c] <= '7');
            if (r == 0 || rows[r][c] != rows[r - 1][c]) {
                RC_PANIC(n < PT_SPRITE_MAX_BYTES);
                a->data[n++] = (uint8_t)((r << 3) | (rows[r][c] - '0'));
            }
        }
    }
    RC_PANIC(n < PT_SPRITE_MAX_BYTES);
    a->data[n] = 0;
}

uint8_t pt_sprite_code(const pt_sprite_art *a, uint8_t col, uint8_t row)
{
    RC_ASSERT(col < pt_sprite_width(a) && row < pt_sprite_height(a));

    // the last run starting at or above the row; the column's runs end at
    // a byte whose row is 0
    const uint8_t *p = &a->data[a->data[2 + col]];
    uint8_t code = *p & 7;
    for (p++; (*p >> 3) != 0 && (*p >> 3) <= row; p++)
        code = *p & 7;
    return code;
}
