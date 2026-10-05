// sprite.h - scaled sprite art: columns of colour change points, top-down,
// over a palette of up to seven catalogue shades plus transparent. The
// runtime format is the Baron sprite demo's, read top-down:
//
//   byte 0         width in columns (1..PT_SPRITE_MAX_COLS)
//   byte 1         height in rows (1..PT_SPRITE_MAX_ROWS)
//   bytes 2..w+1   each column's offset from the art's start
//   then per column, one byte per run, top to bottom: row * 8 + code, the
//   row where the run starts (0 for the first) and its colour code (0
//   transparent, 1..7 the palette). A run ends where the next starts, the
//   column's last at the sprite's height; a 0 byte follows the last column,
//   so every column's runs end at a byte whose row is 0.

#ifndef PT_SPRITE_H_
#define PT_SPRITE_H_

#include <stdint.h>

#define PT_SPRITE_MAX_COLS 32
#define PT_SPRITE_MAX_ROWS 32      // five bits of row in a change byte
#define PT_SPRITE_MAX_BYTES 256    // column offsets are a byte

typedef struct pt_sprite_art {
    uint8_t pal[7];    // catalogue shades of colour codes 1..7
    uint8_t data[PT_SPRITE_MAX_BYTES];
} pt_sprite_art;

// Encode art from rows of colour codes, top row first and NULL-terminated,
// one character per column ('0' transparent, '1'..'7' the palette).
void pt_sprite_encode(pt_sprite_art *a, const char *const rows[], const uint8_t pal[7]);

static inline uint8_t pt_sprite_width(const pt_sprite_art *a)
{
    return a->data[0];
}

static inline uint8_t pt_sprite_height(const pt_sprite_art *a)
{
    return a->data[1];
}

// The colour code at (column, row), by walking the column's runs; for tests
// and reference rendering, not the sprite pass.
uint8_t pt_sprite_code(const pt_sprite_art *a, uint8_t col, uint8_t row);

#endif
