// plot.c - the line plotters (see plot.h).

#include "plot.h"

#include <stdbool.h>

// The pixels from p to the byte's right end, and from its left end to p.
static const uint8_t mask_from_[4] = {0xFF, 0x77, 0x33, 0x11};
static const uint8_t mask_to_[4] = {0x88, 0xCC, 0xEE, 0xFF};

// One column's step of a falling line's row fraction: the carry, 1 when the
// next column is a row down.
static uint8_t carry_down(uint8_t *f, uint8_t gm)
{
    uint16_t s = (uint16_t)(*f + gm);
    *f = (uint8_t)s;
    return (uint8_t)(s >> 8);
}

// And a rising line's: the borrow, 1 when the next column is a row up.
static uint8_t borrow_up(uint8_t *f, uint8_t gm)
{
    uint8_t b = *f < gm;
    *f = (uint8_t)(*f - gm);
    return b;
}

// Plot a row of a byte under mask, then move the cursor a row down (up).
static void row_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t mask, pt_pattern pat)
{
    pt_mode5_eor(fb, *cur, mask, pat);
    pt_mode5_down(cur);
}

static void row_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t mask, pt_pattern pat)
{
    pt_mode5_eor(fb, *cur, mask, pat);
    pt_mode5_up(cur);
}

// One whole byte of a falling shallow line, as the 6502 does it: the three
// steps inside the byte decide, carry by carry, which of eight fixed routines
// plots it, each with its row masks as constants (the comments give the
// carries, the step after the leftmost pixel first). The fourth step then
// says whether the next byte starts a row down. (The 6502 takes that step at
// the routine's start and carries it through the plotting in C.)
static void byte_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, pt_pattern pat)
{
    if (!carry_down(f, gm)) {
        if (!carry_down(f, gm)) {
            if (!carry_down(f, gm)) {
                pt_mode5_eor(fb, *cur, 0xFF, pat);    // 000: all four on one row
            } else {
                row_down(fb, cur, 0xEE, pat);         // 001
                pt_mode5_eor(fb, *cur, 0x11, pat);
            }
        } else if (!carry_down(f, gm)) {
            row_down(fb, cur, 0xCC, pat);             // 010
            pt_mode5_eor(fb, *cur, 0x33, pat);
        } else {
            row_down(fb, cur, 0xCC, pat);             // 011
            row_down(fb, cur, 0x22, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!carry_down(f, gm)) {
        if (!carry_down(f, gm)) {
            row_down(fb, cur, 0x88, pat);             // 100
            pt_mode5_eor(fb, *cur, 0x77, pat);
        } else {
            row_down(fb, cur, 0x88, pat);             // 101
            row_down(fb, cur, 0x66, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!carry_down(f, gm)) {
        row_down(fb, cur, 0x88, pat);                 // 110
        row_down(fb, cur, 0x44, pat);
        pt_mode5_eor(fb, *cur, 0x33, pat);
    } else {
        row_down(fb, cur, 0x88, pat);                 // 111: a row per pixel
        row_down(fb, cur, 0x44, pat);
        row_down(fb, cur, 0x22, pat);
        pt_mode5_eor(fb, *cur, 0x11, pat);
    }
    if (carry_down(f, gm))
        pt_mode5_down(cur);
    pt_mode5_next(cur);
}

// And of a rising one: the same tree on the borrows, moving up.
static void byte_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, pt_pattern pat)
{
    if (!borrow_up(f, gm)) {
        if (!borrow_up(f, gm)) {
            if (!borrow_up(f, gm)) {
                pt_mode5_eor(fb, *cur, 0xFF, pat);    // 000
            } else {
                row_up(fb, cur, 0xEE, pat);           // 001
                pt_mode5_eor(fb, *cur, 0x11, pat);
            }
        } else if (!borrow_up(f, gm)) {
            row_up(fb, cur, 0xCC, pat);               // 010
            pt_mode5_eor(fb, *cur, 0x33, pat);
        } else {
            row_up(fb, cur, 0xCC, pat);               // 011
            row_up(fb, cur, 0x22, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!borrow_up(f, gm)) {
        if (!borrow_up(f, gm)) {
            row_up(fb, cur, 0x88, pat);               // 100
            pt_mode5_eor(fb, *cur, 0x77, pat);
        } else {
            row_up(fb, cur, 0x88, pat);               // 101
            row_up(fb, cur, 0x66, pat);
            pt_mode5_eor(fb, *cur, 0x11, pat);
        }
    } else if (!borrow_up(f, gm)) {
        row_up(fb, cur, 0x88, pat);                   // 110
        row_up(fb, cur, 0x44, pat);
        pt_mode5_eor(fb, *cur, 0x33, pat);
    } else {
        row_up(fb, cur, 0x88, pat);                   // 111
        row_up(fb, cur, 0x44, pat);
        row_up(fb, cur, 0x22, pat);
        pt_mode5_eor(fb, *cur, 0x11, pat);
    }
    if (borrow_up(f, gm))
        pt_mode5_up(cur);
    pt_mode5_next(cur);
}

// A shallow line: its row moves by at most one per column, so it is kept as
// a row byte and a fraction, and each step's carry out of the fraction says
// whether the next column is a row on. Whole bytes go four pixels at a time
// (byte_down, byte_up); the partial bytes at either end go a pixel at a
// time. A flat line has no steps: whole bytes on one row, the end bytes
// masked, with no arithmetic at all (through the tree each byte would still
// take its four steps).
void pt_plot_shallow(uint8_t *fb, const pt_run *p, pt_pattern pat)
{
    uint8_t gm = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as line_y
    uint8_t c = p->x0;

    if (gm == 0) {
        uint8_t n = (uint8_t)(((uint8_t)(p->x1 - 1) >> 2) - (p->x0 >> 2));    // bytes after the first
        uint8_t m0 = mask_from_[p->x0 & 3];
        uint8_t m1 = mask_to_[(p->x1 - 1) & 3];
        if (n == 0) {
            pt_mode5_eor(fb, cur, m0 & m1, pat);
            return;
        }
        pt_mode5_eor(fb, cur, m0, pat);
        while (--n) {
            pt_mode5_next(&cur);
            pt_mode5_eor(fb, cur, 0xFF, pat);
        }
        pt_mode5_next(&cur);
        pt_mode5_eor(fb, cur, m1, pat);
    } else if (!pt_g_isneg(p->g)) {
        for (; (c & 3) != 0 && c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (carry_down(&f, gm))
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
            byte_down(fb, &cur, &f, gm, pat);
        for (; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (carry_down(&f, gm))
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
    } else {
        for (; (c & 3) != 0 && c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (borrow_up(&f, gm))
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
        for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
            byte_up(fb, &cur, &f, gm, pat);
        for (; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            if (borrow_up(&f, gm))
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
    }
}

// A steep line (|g| >= 1): a pixel pair per column, the row moving on by
// the gradient's whole rows plus the fraction's carry, a row at a time.
void pt_plot_steep(uint8_t *fb, const pt_run *p, pt_pattern pat)
{
    uint8_t gi = (uint8_t)(pt_g_mag(p->g) >> 8);
    uint8_t gf = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t mask = pt_mode5_pixmask(p->x0);
    uint8_t f = p->yf;    // the row fraction, rounding bias and all, as line_y

    if (!pt_g_isneg(p->g)) {
        for (uint8_t c = p->x0; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            for (uint8_t n = (uint8_t)(gi + carry_down(&f, gf)); n != 0; n--)
                pt_mode5_down(&cur);
            pt_mode5_right(&cur, &mask);
        }
    } else {
        for (uint8_t c = p->x0; c < p->x1; c++) {
            pt_mode5_eor(fb, cur, mask, pat);
            for (uint8_t n = (uint8_t)(gi + borrow_up(&f, gf)); n != 0; n--)
                pt_mode5_up(&cur);
            pt_mode5_right(&cur, &mask);
        }
    }
}

void pt_plot_flat(uint8_t *fb, uint8_t x0, uint8_t x1, uint8_t row, pt_pattern pat)
{
    pt_run p = {.x0 = x0, .x1 = x1, .y0 = row, .yf = 128, .g = 0};
    pt_plot_shallow(fb, &p, pat);
}

// The unplotters zero whole bytes, both scanlines, so no masks: all a byte
// needs is which rows it has.
//
// A whole byte of a falling shallow run, from the cursor at its first
// column: its rows are that column's and one more for each carry over the
// three steps to its last column, so f + 3g's high byte counts them, k, and
// the byte is k + 1 rows. One step more, g added to that sum's low byte, is
// the next byte's first column, its carry a row down. On the 6502, with f
// in A and the carry clear, it's
//   ADC #g3 : TAX : LDA #0 : BCS more   ; C: a row more than 3g's high byte
//   STA (c),Y : INY : STA (c),Y         ; each row, INY : CPY #8 between
//   DEY : LDA c : ADC #8 : STA c        ; on a byte
//   TXA : ADC #g : BCC on : DOWN        ; C: the next byte a row down
// about 48 cycles a byte and 20 a row more, in one of three loops picked
// by 3g's high byte (0..2).
static void unbyte_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint16_t g3, uint8_t gm)
{
    uint16_t s = (uint16_t)(*f + g3);    // to the byte's last column: the carries
    pt_mode5_zero(fb, *cur);
    for (uint8_t k = (uint8_t)(s >> 8); k != 0; k--) {
        pt_mode5_down(cur);
        pt_mode5_zero(fb, *cur);
    }
    s = (uint16_t)((uint8_t)s + gm);    // and a step on
    *f = (uint8_t)s;
    if (s >> 8)
        pt_mode5_down(cur);
    pt_mode5_next(cur);
}

// And of a rising one: f - 3g, its high byte minus the borrows (0 - k,
// wrapped), then a step on, a borrow wrapping the high byte, a row up.
static void unbyte_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint16_t g3, uint8_t gm)
{
    uint16_t s = (uint16_t)(*f - g3);
    pt_mode5_zero(fb, *cur);
    for (uint8_t k = (uint8_t)(0u - (s >> 8)); k != 0; k--) {
        pt_mode5_up(cur);
        pt_mode5_zero(fb, *cur);
    }
    s = (uint16_t)((uint8_t)s - gm);
    *f = (uint8_t)s;
    if (s >> 8)
        pt_mode5_up(cur);
    pt_mode5_next(cur);
}

// A partial byte at a run's end, n columns of a falling run, a column at a
// time: zero the row, then add g for each step, a carry a row down and
// zeroed, and a step on into the next byte. On the 6502, with f in A,
//   ADC #g : BCS row      ; 4 cycles a column on the same row
// and DOWN : CPY #8 leaves the carry clear again.
static void unpart_down(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, uint8_t n)
{
    pt_mode5_zero(fb, *cur);
    for (; n > 1; n--) {
        if (carry_down(f, gm)) {
            pt_mode5_down(cur);
            pt_mode5_zero(fb, *cur);
        }
    }
    if (carry_down(f, gm))
        pt_mode5_down(cur);
    pt_mode5_next(cur);
}

// And of a rising one, on the borrows.
static void unpart_up(uint8_t *fb, pt_mode5_cur *cur, uint8_t *f, uint8_t gm, uint8_t n)
{
    pt_mode5_zero(fb, *cur);
    for (; n > 1; n--) {
        if (borrow_up(f, gm)) {
            pt_mode5_up(cur);
            pt_mode5_zero(fb, *cur);
        }
    }
    if (borrow_up(f, gm))
        pt_mode5_up(cur);
    pt_mode5_next(cur);
}

// A shallow run, split as the plotter splits it: the partial bytes at its
// ends a column at a time (unpart_down, unpart_up), and the whole bytes
// with 3g (unbyte_down, unbyte_up), made, and its loop picked, only for a
// run that has one. A flat run is its bytes along one row, no adds at all.
void pt_unplot_shallow(uint8_t *fb, const pt_run *p)
{
    uint8_t gm = (uint8_t)pt_g_mag(p->g);
    RC_ASSERT(pt_g_mag(p->g) < 0x100);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t f = p->yf;
    uint8_t c = p->x0;
    uint8_t n = (uint8_t)(4u - (c & 3u));    // the first byte's columns
    if (n > (uint8_t)(p->x1 - c))
        n = (uint8_t)(p->x1 - c);

    if (gm == 0) {
        uint8_t nb = (uint8_t)(((uint8_t)(p->x1 - 1) >> 2) - (p->x0 >> 2));    // bytes after the first
        pt_mode5_zero(fb, cur);
        for (; nb != 0; nb--) {
            pt_mode5_next(&cur);
            pt_mode5_zero(fb, cur);
        }
    } else if (!pt_g_isneg(p->g)) {
        if (n != 4) {
            unpart_down(fb, &cur, &f, gm, n);
            c = (uint8_t)(c + n);
        }
        if ((uint8_t)(p->x1 - c) >= 4) {
            uint16_t g3 = (uint16_t)((gm << 1) + gm);
            for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
                unbyte_down(fb, &cur, &f, g3, gm);
        }
        if (c != p->x1)
            unpart_down(fb, &cur, &f, gm, (uint8_t)(p->x1 - c));
    } else {
        if (n != 4) {
            unpart_up(fb, &cur, &f, gm, n);
            c = (uint8_t)(c + n);
        }
        if ((uint8_t)(p->x1 - c) >= 4) {
            uint16_t g3 = (uint16_t)((gm << 1) + gm);
            for (; (uint8_t)(p->x1 - c) >= 4; c = (uint8_t)(c + 4))
                unbyte_up(fb, &cur, &f, g3, gm);
        }
        if (c != p->x1)
            unpart_up(fb, &cur, &f, gm, (uint8_t)(p->x1 - c));
    }
}

// A steep run: a row a column, the cursor stepped on gi rows and the
// fraction's carry, as pt_plot_steep's, but with no mask: every fourth
// column the cursor just moves on a byte. On the 6502 a column is
//   LDA #0 : STA (c),Y : INY : STA (c),Y
//   LDA f : CLC : ADC gf : STA f, then DOWN gi times, or gi + 1 on a carry
// about 30 cycles and 8 a row moved, unrolled four to a byte and entered
// part way for the run's first. Rising borrows and goes up.
void pt_unplot_steep(uint8_t *fb, const pt_run *p)
{
    uint8_t gi = (uint8_t)(pt_g_mag(p->g) >> 8);
    uint8_t gf = (uint8_t)pt_g_mag(p->g);
    pt_mode5_cur cur = pt_mode5_at(p->x0 >> 2, p->y0);
    uint8_t f = p->yf;

    if (!pt_g_isneg(p->g)) {
        for (uint8_t c = p->x0;;) {
            pt_mode5_zero(fb, cur);
            if (++c == p->x1)
                break;
            for (uint8_t n = (uint8_t)(gi + carry_down(&f, gf)); n != 0; n--)
                pt_mode5_down(&cur);
            if ((c & 3u) == 0)
                pt_mode5_next(&cur);
        }
    } else {
        for (uint8_t c = p->x0;;) {
            pt_mode5_zero(fb, cur);
            if (++c == p->x1)
                break;
            for (uint8_t n = (uint8_t)(gi + borrow_up(&f, gf)); n != 0; n--)
                pt_mode5_up(&cur);
            if ((c & 3u) == 0)
                pt_mode5_next(&cur);
        }
    }
}
