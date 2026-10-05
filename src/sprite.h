// sprite.h - scaled sprites: Baron's examples/spritescale format and its two
// plotters, drawn over the finished picture, and clipped to rows given per
// screen column.
//
// The format is spritescale's: run-length columns, bottom up.
//
//   byte 0           width in columns (1..PT_SPRITE_MAX_COLS)
//   byte 1           height in rows (1..PT_SPRITE_MAX_ROWS)
//   byte 2           the crossover scale: below it (in 256ths) the per-pair
//                    rebuild beats the stretch engine
//   bytes 3..w+2     each column's offset from the art's start
//   then per column, one byte per run, bottom to top: its last row * 8 +
//   its colour code (0 transparent, 1..7 the palette). A run starts where
//   the one below ended, and a column's last ends at the top row.

#ifndef PT_SPRITE_H_
#define PT_SPRITE_H_

#include <stdbool.h>
#include <stdint.h>

#include "mode5.h"
#include "palette.h"

#define PT_SPRITE_MAX_COLS 32
#define PT_SPRITE_MAX_ROWS 32      // five bits of row in a run byte
#define PT_SPRITE_MAX_BYTES 256    // column offsets are a byte

typedef struct pt_sprite_art {
    uint8_t pal[7];           // catalogue shades of colour codes 1..7
    pt_pattern pats[8];       // their patterns, 0 transparent: made with the
                              // art, as the 6502's tables are
    uint8_t data[PT_SPRITE_MAX_BYTES];
} pt_sprite_art;

// Encode art from rows of colour codes, top row first and NULL-terminated,
// one character per column ('0' transparent, '1'..'7' the palette), with
// the crossover worked out as Baron's sprite macro does, and the palette's
// patterns.
void pt_sprite_encode(pt_sprite_art *a, const char *const rows[], const uint8_t pal[7]);

static inline uint8_t pt_sprite_width(const pt_sprite_art *a)
{
    return a->data[0];
}

static inline uint8_t pt_sprite_height(const pt_sprite_art *a)
{
    return a->data[1];
}

static inline uint8_t pt_sprite_crossover(const pt_sprite_art *a)
{
    return a->data[2];
}

// The colour code at (column, row), the row counted up from the bottom, by
// walking the column's runs; for tests and reference rendering, not the
// plotters.
uint8_t pt_sprite_code(const pt_sprite_art *a, uint8_t col, uint8_t row);

// Where a sprite goes, in spritescale's terms. A pair is a canvas row, its
// two scanlines, counted up from the screen's bottom: pair p is row 63 - p.
typedef struct pt_sprite_place {
    uint16_t scale;    // 8.8: screen pixels per source column, and pairs per
                       // source row
    int16_t x;         // the left pixel column: off the left when negative
    int16_t y;         // the bottom pair: off the bottom when negative
} pt_sprite_place;

// The pairs each screen column the sprite covers shows, [lo, hi) counted up
// from the screen's bottom, held for columns [x0, x1) (column x at x - x0);
// hi <= lo shows none. The plotters read only the columns they plot, which
// lie in there. whole says every one of them shows all of the sprite, so no
// strip needs classing.
typedef struct pt_sprite_clip {
    const uint8_t *lo;
    const uint8_t *hi;
    uint8_t x0, x1;
    bool whole;
} pt_sprite_clip;

// Plot a sprite into a finished frame, its pixels where it's opaque and the
// clip shows them, by the plotter its scale and crossover pick: the per-pair
// rebuild below the crossover (under 1x), else the stretch engine.
void pt_sprite_plot(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p, pt_sprite_clip clip);

// The two plotters, by name; the same bytes. The rebuild only under 1x.
void pt_sprite_plot_engine(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p,
                           pt_sprite_clip clip);
void pt_sprite_plot_rebuild(uint8_t *fb, const pt_sprite_art *a, pt_sprite_place p,
                            pt_sprite_clip clip);

#endif
