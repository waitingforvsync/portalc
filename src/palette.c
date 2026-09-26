// palette.c - the hardcoded 35-shade catalogue, one 4x2 dither tile per mix
// of four pixels from the four hardware colours, written out pixel by pixel
// (PT_PX4 builds the scanline bytes at compile time). The tiles follow these
// rules:
//
// - one or two colours: quarter-density fills. 50% is the single-pixel
//   checkerboard, darkest colour top left (already chunky on screen, since
//   MODE 5 pixels are double wide); the quarter and three-quarter levels put
//   one dot (or hole) in each 2x2 half of the tile, the second half offset
//   onto the other scanline so nothing stacks into stripes.
// - three colours (2+1+1): a checkerboard between the pair colour on one
//   phase and the two singles on the other, the singles alternating line by
//   line with the darker one on the even line; the phases are assigned so
//   the darkest colour sits top left.
// - 1+1+1+1: every scanline carries every colour, the pairs swapped.

#include "palette.h"

#include "richc/macros.h"

enum { K = PT_BLACK, B = PT_BLUE, R = PT_RED, G = PT_GREEN };

// Indexed by mix, darkest first; the comments give the summed Rec. 601
// luminance (K 0, B 114, R 299, G 587 per thousand).
static const pt_pattern shades_[PT_NUM_SHADES] = {
    {PT_PX4(K, K, K, K), PT_PX4(K, K, K, K)},    //  0 KKKK    0
    {PT_PX4(B, K, K, K), PT_PX4(K, K, B, K)},    //  1 KKKB  114
    {PT_PX4(K, B, K, B), PT_PX4(B, K, B, K)},    //  2 KKBB  228
    {PT_PX4(R, K, K, K), PT_PX4(K, K, R, K)},    //  3 KKKR  299
    {PT_PX4(B, K, B, B), PT_PX4(B, B, B, K)},    //  4 KBBB  342
    {PT_PX4(K, B, K, B), PT_PX4(R, K, R, K)},    //  5 KKBR  413
    {PT_PX4(B, B, B, B), PT_PX4(B, B, B, B)},    //  6 BBBB  456
    {PT_PX4(K, B, K, B), PT_PX4(B, R, B, R)},    //  7 KBBR  527
    {PT_PX4(G, K, K, K), PT_PX4(K, K, G, K)},    //  8 KKKG  587
    {PT_PX4(K, R, K, R), PT_PX4(R, K, R, K)},    //  9 KKRR  598
    {PT_PX4(R, B, B, B), PT_PX4(B, B, R, B)},    // 10 BBBR  641
    {PT_PX4(K, B, K, B), PT_PX4(G, K, G, K)},    // 11 KKBG  701
    {PT_PX4(K, R, K, R), PT_PX4(R, B, R, B)},    // 12 KBRR  712
    {PT_PX4(K, B, K, B), PT_PX4(B, G, B, G)},    // 13 KBBG  815
    {PT_PX4(B, R, B, R), PT_PX4(R, B, R, B)},    // 14 BBRR  826
    {PT_PX4(K, R, K, R), PT_PX4(G, K, G, K)},    // 15 KKRG  886
    {PT_PX4(R, K, R, R), PT_PX4(R, R, R, K)},    // 16 KRRR  897
    {PT_PX4(G, B, B, B), PT_PX4(B, B, G, B)},    // 17 BBBG  929
    {PT_PX4(K, B, R, G), PT_PX4(R, G, K, B)},    // 18 KBRG 1000
    {PT_PX4(R, B, R, R), PT_PX4(R, R, R, B)},    // 19 BRRR 1011
    {PT_PX4(B, R, B, R), PT_PX4(G, B, G, B)},    // 20 BBRG 1114
    {PT_PX4(K, G, K, G), PT_PX4(G, K, G, K)},    // 21 KKGG 1174
    {PT_PX4(K, R, K, R), PT_PX4(R, G, R, G)},    // 22 KRRG 1185
    {PT_PX4(R, R, R, R), PT_PX4(R, R, R, R)},    // 23 RRRR 1196
    {PT_PX4(K, G, K, G), PT_PX4(G, B, G, B)},    // 24 KBGG 1288
    {PT_PX4(B, R, B, R), PT_PX4(R, G, R, G)},    // 25 BRRG 1299
    {PT_PX4(B, G, B, G), PT_PX4(G, B, G, B)},    // 26 BBGG 1402
    {PT_PX4(K, G, K, G), PT_PX4(G, R, G, R)},    // 27 KRGG 1473
    {PT_PX4(G, R, R, R), PT_PX4(R, R, G, R)},    // 28 RRRG 1484
    {PT_PX4(B, G, B, G), PT_PX4(G, R, G, R)},    // 29 BRGG 1587
    {PT_PX4(G, K, G, G), PT_PX4(G, G, G, K)},    // 30 KGGG 1761
    {PT_PX4(R, G, R, G), PT_PX4(G, R, G, R)},    // 31 RRGG 1772
    {PT_PX4(G, B, G, G), PT_PX4(G, G, G, B)},    // 32 BGGG 1875
    {PT_PX4(G, R, G, G), PT_PX4(G, G, G, R)},    // 33 RGGG 2060
    {PT_PX4(G, G, G, G), PT_PX4(G, G, G, G)},    // 34 GGGG 2348
};

pt_pattern pt_shade(uint8_t i)
{
    RC_ASSERT(i < PT_NUM_SHADES);
    return shades_[i];
}

uint32_t pt_palette_rgba(uint8_t c)
{
    RC_ASSERT(c < 4);

    static const uint32_t rgba[4] = {
        0xFF000000u,    // black
        0xFFFF0000u,    // blue
        0xFF0000FFu,    // red
        0xFF00FF00u,    // green
    };
    return rgba[c];
}
