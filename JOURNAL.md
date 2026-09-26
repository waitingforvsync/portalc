# Journal

Notes on the engine's evolution, newest entry last.

## 2026-09-26 — Sines never manifested; the 16×16 multiply removed

Two passes over the arithmetic, both in service of the same rule: consider
what translates most efficiently to the 6502, keep every multiply and divide
on unsigned magnitudes of the narrowest width, and hold signs in conditions
rather than in values.

### Multiplication by sin/cos as its own operation

`pt_sin` and `pt_cos` are gone: a signed sine value is never formed anywhere.
The sine table keeps its u0.8 magnitudes over [0, π) (a stored 0 meaning 256,
indices 0 and 128 special cased), and multiplying by sin(θ) became the
primitive instead: the index's low 7 bits pick the magnitude, and the index's
top bit together with the sign of the other operand picks the positive or the
negated product path.

Since the only consumers are the camera direction, the resolution happens
once per frame: `pt_sin_resolve` produces a `pt_sincos` — magnitude in
[0, 256] with the sign kept apart — and the per-vertex `pt_mul_sc` is then
just a 256-test, two quarter-square lookups (rounded to nearest, keeping the
transform unbiased) and the sign fork. No per-call angle masking or zero
tests. `pt_mul_sin`/`pt_mul_cos` remain as one-step wrappers for the camera
tick, which also now computes its facing step once and derives all four move
directions from it (2 table multiplies per tick instead of up to 8).

### The reciprocal rework: no more `mulwide`

The engine had one deliberately wide helper, `mulwide`, a full 16×16=32
product (four quarter-square multiplies) under every reciprocal division —
and `pt_div_q8` chained two of them. Overkill for results that are 7, 8 or
16 bits wide. All divisions now run through one core, `recip_div`, built on
a re-shaped reciprocal table:

- The denominator is normalised into **[128, 255]** — left shifts exact,
  right shifts truncating as before — so its reciprocal is always
  `1 + f/256` at a known power of two. `f` is a single byte
  (`round(0x10000/n) − 256`, a stored 0 meaning 256, the sine table's
  trick). The table shrinks from 512 bytes of u16 to 128 bytes of u8.
- A division is then `num + (num·f >> 8)` and a shift: one add plus at most
  three table lookups (two when the numerator fits u16, one when it fits
  u8). `pt_div_q8`'s eight lookups became at most three plus one compare
  that saturates overflowing quotients up front.
- When the net shift is left (small denominators), the numerator is
  pre-shifted exactly *before* the fractional multiply, so truncation is
  never amplified. Every caller's domain keeps the chain within u24
  (asserted).
- Power-of-two denominators (`f = 0`) collapse to pure shifts; dividing by 1
  is now exact instead of the old `0xFFFF` saturation entries.

Precision: the byte reciprocal's error is relative, ≤ ~0.2% plus a count of
truncation. Denominators up to 255 lose the old table's exactness, but the
entry rounding (round vs floor) centres the error better for large ones.
The only test tolerance that moved is `grad_mag`, from ±2 absolute to
`exact/256 + 2` — the same relative form `gradq8_mag` already used, and over
a line's own column span the row drift stays under a sixth of a row.

### `pt_vv` keeps signs, not sums

The transformed-vertex struct stored the frustum ray sums `a = x + y` and
`b = x − y`, yet almost every consumer only tested their signs. The sums now
exist only inside `transform`, where their sign and zero states become four
flag bits (the negative bits at 7 and 6, where the 6502's `BIT` lands them
in N and V — on the port they are the adds' own N and Z, stored). The two
value consumers, the frustum clips, recompute the magnitudes they need from
`x` and `y` as unsigned adds with the signs known from the flags. `pt_vv`
drops from 10 bytes to 6, which the 24-deep walk stack feels.

### Consolidation

The camera's keep-high u24×u8 chain now calls arith's exported
`pt_mulu24_frac`; `pt_recip16` went static inside arith as `recip16`.
Spec §4's reciprocal bullet updated to match.

### Verification

1,076 frames (every cell centroid × 32 angles, plus a 4,000-tick scripted
walk) rendered before and after: 664 bit-identical; the rest averaged 29 of
8,192 subpixels changed, worst 216 — eyeballed side by side, boundary rows
shifting ±1 along slopes, indistinguishable. All 22 tests pass in debug
(asserts live) and the release build is warning-free.

### Products never negated

A follow-up pass hunted the negate-after-multiply sites. A lone signed
product has to end up two's complement, so its fork-and-negate is
irreducible — but wherever two products meet in a sum, the negations can
go. The three-way split: opposite signs are one subtraction either way
round, whose 16-bit wraparound simply *is* the two's complement of the
answer; same-positive is an add; and only same-negative negates at all, once,
of the finished sum — and on the 6502 even that disappears, either as an SBC
chain from zero (the same cycles as the ADC chain) or by accumulating the
pair from the negated-product table, which is `pt_mulu8_neg`'s reason to
exist. That answers the earlier open question: it stays.

Fused accordingly:

- `transform`'s rotation: `pt_mac_sc` / `pt_msub_sc` (u·a ± v·b for resolved
  sines) replace the two `pt_mul_sc + pt_mul_sc` sums — the hottest negates
  in the engine, per vertex.
- camera: `dot24_s8` fuses `edge_t` and the slide's `along`; `edge_dist`
  accumulates the two normal products directly (the positions are unsigned,
  so only the normal forks); `corner_deflect`'s approach dot is now built as
  sign-and-magnitude outright, since its sign only gates and the division
  wanted the magnitude anyway — so `-dot` went too, along with `mul_su` and
  `mulq_s8`.
- the resolve push and the corner correction step positions by unsigned
  magnitudes through an add/sub fork instead of negate-then-add.
- `clip_right`'s `-(x - y)` became the reversed subtraction `y - x` (free);
  `clip_left`'s `-(x + y)` a subtract chain from zero.

All bit-identical: the 1,076-frame sweep matches byte for byte, and a new
`mac_sc` test pins the accumulate to the summed products across every sign
quadrant.

## 2026-09-26 (later) — Byte heights; the camera drops to whole units

### Heights quantised to 8-unit steps

Map heights are now constrained to multiples of 8 (the builder asserts it;
the demo scene and PT_CAM_HEIGHT already complied), so a relative height
|z − eye| / 8 fits a byte with room to spare (up to 2040 units). `pt_relh`
carries that byte plus a below-the-eye flag — computed by an unsigned
compare-and-subtract, so the height is never a signed value — and
`make_line` forks on the side exactly where the sign used to fork: the
screen-edge row for the near side and the centre ± offset of the projection.
`pt_project_row` became `pt_project_rowd`, returning just the offset
magnitude 64·8·h8/x (the ÷8 compensated by a wider shift in the reciprocal
core, at no cost). The demo sweep is bit-identical.

The cell heights themselves now live in those steps: `pt_cell.floor_z` and
`ceil_z` are single bytes of z/8 (the builder takes world units and asserts
the contract: multiples of 8, under 2040), `eye_z` and PT_CAM_HEIGHT follow,
and `rel_h` collapses to a byte compare-and-subtract with no scaling at all.
The cell struct sheds two bytes; rendering stays bit-identical.

### Whole-unit wall interactions

The camera's u16.8 fraction exists so the move vector can accumulate
diagonal steps; against a 40-unit radius it is noise in every wall test. So
`edge_dist`, `edge_t`, `foot_in_range`, the resolve push and the portal
transition all now run in whole world units and 16-bit chains (positions
enter as their high bytes — free on the 6502), with `pt_edge.d0` stored
floored to whole units (s16 instead of s16.8 in the map). The multiplies
drop from three-partial u24 chains to two-partial u16 ones, and the resolve
push is a genuine 8-bit quantity (under the radius). Wraparound mod 2^16
keeps the accumulations honest wherever the true value fits s15, and
`pt_map_edge_side` computes exactly from the edge vertex instead of the
floored d0, so the builder's convexity validation keeps its precision.

Whole-unit truncation puts each distance within (−2, +2) of exact, so
HIT_EPS became 2 whole units. What still runs at s16.8: the move vector and
its slide projection (the accumulator itself), and `corner_deflect`'s dot —
its post gate is already whole-unit and 8-bit, but the deflection must see
sub-unit moves or slow diagonals could creep into a jamb.

Verified: over a 40,000-tick random walk, the exact closest approach to any
solid wall is 29.06 units against the old code's 29.03 — the truncation
did not move the sliding invariant (both figures are corner-post overlap,
which is by design). All 23 tests pass; render output is untouched by the
camera change and bit-identical under the height change.

### Open questions

- `resolve_block`/`emit` recompute `line_y`/`piece_y` at shared columns — a
  port-time zero-page caching matter, not worth contorting the C for.
- `edge_dist` in the camera multiplies full u24 positions; cell-relative
  coordinates would narrow it but cost map data.
