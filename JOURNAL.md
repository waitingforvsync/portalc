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

## 2026-09-26 (later still) — The shade catalogue hardcoded

palette.c no longer builds anything: the 35 shades are a hardcoded table of
byte pairs, each tile written out pixel by pixel through PT_PX4 (a
compile-time four-pixels-to-scanline-byte macro in palette.h, kept so every
entry stays inspectable), with the construction rules recorded as the table's
comment. The builders (solid/check/cell/blend and their helpers) are gone;
the demo map picks its colours from the catalogue by mix name (PT_SH_KRRR
and friends), which every one of its solids and quarter blends already was.
The test derives the expected catalogue from the rules — enumerating the
multisets in luminance order — and checks each tile's content, ordering and
the every-scanline-carries-every-hue property. Bit-identical, and exactly
the shape the 6502 build wants: 70 bytes of data, no code.

## 2026-09-26 (evening) — Dither shapes regularised

Rich reshaped shades 2, 9, 12 and 14 by hand; the rest of the catalogue now
follows the same rules. Every 2+2 mix is the single-pixel checkerboard with
the darkest colour top left (21, 26, 31 flipped to match 2, 9, 14). Every
three-colour mix follows shade 12's shape: a checkerboard between the pair
colour on one phase and the two singles on the other, the singles
alternating line by line with the darker on the even line — and where the
pair itself is the darkest hue (5, 11, 15, 20), it takes the top-left phase
so the darkest-top-left convention holds throughout. The old
every-scanline-carries-every-hue rule survives only for the quarter fills
and the full mix; the skipped catalogue test is re-enabled, deriving the
exact expected tile for both checker classes from the rules.

## 2026-09-26 (night) — Sines restricted to rotation

Every sine use turned out to be a component of one 2D rotation: the vertex
transform is the pair x' = x c + y s, y' = y c - x s, and the camera's
facing step is (speed, 0) rotated back into the world (the sine's sign
flipped on the way in). So the whole sine-multiply family (pt_mul_sc,
pt_mac_sc, pt_msub_sc, pt_mul_sin, pt_mul_cos) collapsed into pt_sin_resolve
plus one pt_rot that produces both components: the two magnitude/sign splits
and the four unsigned rounded products are shared, where the paired
operations split each operand twice over. Bit-identical against a
HEAD-baseline sweep; the rot test checks both components against wide
references at every angle across the sign quadrants.

## 2026-09-26 (late) — pt_edge and pt_cell slimmed

pt_edge drops three fields. The twin index is derived on demand by
pt_map_twin — the adjacent cell's one edge starting at our end vertex
(rings are clockwise, so the shared wall runs the other way there), a tiny
ring scan that only runs when a portal is pushed. The wall pattern and the
soft flag fold into one colour byte: a catalogue shade plus one, with 0
marking a soft edge (a soft edge never draws, and black remains expressible
as shade 0 + 1). pt_cell's floor and ceiling patterns become shade indices
too. pt_edge goes 19 bytes to 16, pt_cell 8 to 6; the renderer looks the
patterns up once per edge.

One conversion slip caught by the tooling: the stairs' blue25 serves as both
a cell surface and looked like a wall colour, so the mechanical shade+1
wrapping silently brightened nine cells' surfaces by one catalogue step. The
render tests can't see colour-only mistakes; the byte-for-byte sweep and a
semantic map dump (old fields vs new, decoded to patterns) pinned it in
minutes. Both now agree exactly with the old representation.

## 2026-09-26 (midnight) — The transition test without u24

The exact q8 transition test worked but kept 24-bit intermediates. Two
attempts to remove them, one failed and one sound:

The failed one is worth recording. Evaluating each portal line once,
canonically (through the lower-numbered cell's twin), makes the whole-unit
test pairwise consistent -- but pairwise is not enough. Near a fan vertex
the truncation-shifted lines no longer concur at a point, and slivers appear
that no cell claims: cell X hops to the pit, the pit hops to Y, Y hops back
to X, and the pass cap parks the walk wherever it stops (the probe caught a
16-unit mis-assignment).

The sound one is the band trick. Away from the line, the whole-unit
distance is decisive (margin 8 covers its truncation plus the stored
normal's divergence from the vertex line). Within the band, the true side
value is provably small -- perpendicular distance times edge length, under
s16 for any edge shorter than 2048 units (asserted in the builder) -- so
the exact cross product against the line through the edge's two integer
vertices is computed entirely mod 2^16, where wraparound is exact: three
lookups per product, high partials never formed. That line passes exactly
through both endpoints, the twin's value is its exact negation, and fan
lines concur exactly at their shared vertex, so the hop walk can neither
cycle nor stick. pt_map_edge_side now shares the same vertex-line geometry,
so point location, validation and transitions can never disagree.

Probes: zero penetration over all fan crossings; the only residual
mismatches against find_cell are integer positions exactly on a rim line,
where both cells contain the point and the walk keeps the cell it arrived
from -- hysteresis that avoids eye-height popping while skimming a rim, and
arguably more sensible than find_cell's scan order.

## 2026-09-27 — Edges as angles

The angle representation prototyped, and it is a different algorithm, the
one the discussion hoped for: pt_edge's camera data is now one direction
byte (256ths of a turn, V0 to V1) and one quantised length byte, and every
wall interaction happens in the edge's own frame. edge_dist and edge_t
merged into edge_frame -- one pt_rot of Q - V0 by the edge angle, the
renderer's own primitive, giving the foot position as x and the
interior-positive distance as y. The resolve push is pt_dir8 along
angle + 64 (the inward normal is the direction a quarter turn on), and the
slide is a rotation into the wall frame, keeping x, and a rotation back by
the negated angle byte. dot24_s8, dot16_s8, muls24_s8, mul16u_c8, edge_dist,
edge_t and step all fell away; pt_edge is 6 bytes (from 19 at yesterday's
start), 576 bytes of map edges instead of 1824.

Consistency, per the discussion's warning, rests on two supports. The
builder writes each twin's angle as exactly its partner's + 128 (never an
independent rounding), so both sides of a portal work against one line. And
the transition keeps the angle-free vertex-line band test: the decisive
margin now grows along the edge (2 + t/64) to cover the quantised angle's
tilt about V0, which also bounds the band cross product within s16 for
edges under 896 units (asserted; the demo's longest is 580).

Numbers: the fan probe is identical to the vector build (zero penetration,
only exact on-line ties); the 40k-tick closest approach moved 29.55 to
29.17 units against radius 40 -- the tilt's whole cost; rendering untouched
and bit-identical. The map test now checks the representation directly:
rotating each edge vector by its own angle lands on +x within quantisation.

### Open questions

- `resolve_block`/`emit` recompute `line_y`/`piece_y` at shared columns — a
  port-time zero-page caching matter, not worth contorting the C for.
  (An earlier note about `edge_dist`'s u24 positions is resolved: the edge
  frame works in 16-bit vertex-relative coordinates.)

## 2026-09-27 — Rounding bias folded into the plot DDA

Rich's observation: `plot` was calling `row_of_round` per column — an add
of 128 with an overflow saturate — when the bias could simply be folded
into the starting `y` once and the row read off by truncation. Only +128 is
ever needed: round-to-nearest is `(y + 128) >> 8` whichever way the line
runs, because the gradient's sign decides whether `gm` is added or
subtracted, never how the value rounds. The DDA holds exactly
`y0*256 +/- c*gm`, so one bias up front equals a bias at every column, and
the sweep is bit-identical.

The saturate goes too: the ascending loop breaks once the row reaches 64,
so the biased accumulator sits below 0x4000 at the top of the loop and
adding `gm <= 0x7FFF` cannot wrap; the descending loop already clamps at
zero. On the 6502 the row is now literally the high byte of the
accumulator after the 16-bit add — no per-pixel adjustment at all.

### The bias made the anchor convention

The same fold then went renderer-wide: `piece_y`/`line_y` (and the
near-clip's `base`) seed their low byte with 128 — a free constant load —
so every reconstructed u8.8 row carries the rounding bias, and all four
re-anchor sites (`pool_split`, `emit`, the near-clip, `clamp_line`) read
the nearest row as a plain high byte; `row_of_round` is gone. The bias
cancels in every line-vs-bound difference feeding `crossing`, so the
resolve logic is untouched. The one debt: the conservative extents must
peel the bias off again — floor became `row_of_floor` (`(y - 128) >> 8`,
two sites) and ceil's constant moved 255 to 127 (free). Net: four
add-and-saturates traded for two subtracts, and one convention shared
with the plot DDA instead of a bias sprinkled per call.

Rail semantics note: `row_step`'s clamps now sit at *biased* 0 and
0xFFFF, half a row different from before in unbiased terms. Re-anchored
rows are provably identical at both rails; only a `crossing` against a
rail-pinned value could shift by a column. The sweep says none does: all
1,076 frames bit-identical.

## 2026-09-27 — Edges no longer know their cell

Rich's question — why does `pt_edge` need its owning cell? — had a short
answer: it didn't. The only runtime consumer was `pt_map_ring_next`'s
wrap, and every walker already arrives at an edge through some cell's
`[first_edge, first_edge + num_edges)` range: the render walk's frames
carry the cell, the camera loops its own cell (and named neighbours), and
the builder and validator loop all cells. So the member is gone —
`pt_edge` is 5 bytes (480 bytes of edge data for the demo's 96 edges) —
and the ring functions take the cell instead: `pt_map_ring_next(c, e)`,
`pt_map_twin(m, c, e)`, `pt_map_edge_side(m, c, e, x, y)`. On the 6502
that is also the cheaper shape: the walker holds {first, num} in zero
page once per ring rather than each edge re-fetching its cell record.

The one caller that didn't know its cells was the camera's post-collection
loop, which ran over the candidate array after the fact; posts are now
collected at candidate creation, where the ring is in hand (same
vertices, same order, same dedup — the array is identical). In the
builder, twin matching and the angle/length pass became per-cell walks,
and the twin-angle override folded into the same pass: `e->adj < ci`
replaces `t < i` (equivalent, since rings are laid out in cell order and
twins never share a cell), and an earlier cell's angles are final by the
time a later cell reads them. Validation drops the `t->cell != e->adj`
check — `pt_map_twin` only searches `cells[e->adj]`, so it is true by
construction. Tests 22/22, sweep bit-identical.

### The verification kit joins the repo

The scratchpad harnesses that every refactor has leaned on are now real
parts of the tree. The two with ground truth became tests in
`src/test/probes/`, running with the suite: `probe.fan_crossings` drives
the camera over every portal-only fan vertex from all 256 directions and
asserts a cell mismatch against exact point location only ever happens
dead on a boundary line (penetration exactly zero), and
`probe.collision_distance` walks 40,000 pseudo-random ticks asserting the
exact closest approach to a solid wall stays above 28 units (29.17
observed, radius 40). The rest became tools beside the frame dumper:
`portalc_sweep` (the byte-exact before/after framebuffer dump - 576
centroid frames plus a 500-frame walkthrough; two builds' outputs compare
with cmp), `portalc_fb2ppm` (extract one sweep frame as a PPM) and
`portalc_mapdump` (the semantic map dump for diffing map data). The
sweep's dumps stay outside the repo - the tool regenerates them; only
the generators are versioned. Earlier probe iterations (three older fan
probes, the palette generators, a stale field-lister) stay retired in
the scratchpad.

## 2026-09-27 — Boundary poses: robustness contained

Rich asked whether the transition tie rules could leave the camera
rendering from outside, or exactly on, its claimed cell's boundary - and
they can: the tie rule parks the centre exactly on portal lines (integer
positions, axis moves), and the old prefilter margin only grew along
positive t while the quantised angle's tilt grows with |t|, so poses on a
line's backward extension could hop falsely or not at all (a probe
measured 65 false and 129 missed hops out to |t| = 432). A directed
render probe then faulted 152 of 181 legitimate on-boundary poses, three
mechanisms deep: rounded ray sums flipping near-zero signs (push_root's
winding search exhausts), an eye-coincident vertex transforming to view
(0, 0) with no direction (the ring's stop test never fires and the walk
wraps into a cut regression), and a latent seam bug - a behind vertex
exactly on a frustum ray's backward extension read as "on the forward
ray". In exact arithmetic none of this is structural: the walk already
handles an on-line eye perfectly (the root search picks the through-the-
eye portal, clip_left maps the eye-crossing to column 0, and the capture
pulls the neighbour in over exactly its half of the view).

After a costlier intermediate design (camera nudged off lines; exact
classification of every vertex), the fix contracted to Rich's framing:
only the camera's cell needs care, children may misproject as long as the
walk survives it. The whole thing is now five small pieces:

- camera.c: the prefilter margin is symmetric in |t| (one abs). The
  transition loop is otherwise untouched; ties on the line remain, and
  both their claiming cells render the same correct view.
- transform: rounded flags as ever, plus the seam rule - a vertex on a
  backward-ray seam counts as behind (two branches hidden behind the
  zero tests, which the 6502 gets from the adds' own Z).
- process_edge: spans clamp to the cursors' frontier - ownership only
  moves right, so any wrap or misordered projection becomes empty spans,
  never a cut regression. One compare per edge.
- push_root: the rounded winding search, when it exhausts (at most once
  per frame, only at boundary poses), retries with exact ray sums -
  pt_ray_basis gives k1 = C - S, k2 = C + S, and a * 256 = x k1 + y k2
  is linear in the world offset, so convexity guarantees a transition
  for any eye in its closed cell; the sole exception (eye ON a ring
  vertex, cell wholly to its right) roots at that zero vertex, whose
  ring then ends by its steps instead of a stop column - accepted.
- The walk's steps-out ending is an outcome, not an assert.

Verification: the boundary probe passes 181/181 poses at all 256 facings
(renders may carry a one-column sliver at exact ties - the accepted price
of rounded spans); the fan drive's 49,152 reachable-state renders are
clean; the extension probe reads 0/0; collision and fan ground truth are
unchanged (29.17 units closest, 101 exact-tie mismatches, zero
penetration); and the full sweep is BYTE-IDENTICAL to the pre-work
baseline - the robustness layer is invisible until a degenerate pose
needs it. probe_walk joined the suite (25 tests). Map data stays
unconstrained; a 45-degree-grid map would let the margin collapse to a
constant and the length bound relax, noted for the future.

## 2026-09-27 — Division without u24, and the duplicate work out of the walk

Rich asked whether recip_div's 24-bit multiply was necessary. It was not:
a quotient num * 2^k / den is num * (256 + f) >> (16 + s - k), and for a
byte numerator that product is one lookup with num added into the high
byte - exact in 17 bits - so the left pre-shift that widened the chain is
never needed. The core split in two: mul_recip8 (one lookup, exact) for
byte numerators, and mul_recip16 (num + frac(num * f), the carry as a
17th bit) for u16 ones, each caller shifting right from there. Only
pt_gradq8_mag had really used the u24 chain (its pre-shift reached 22
bits); it now shifts the 17-bit product either way, saturating before a
left shift. 445M comparisons against the old routines: no mismatch.
pt_mulu24_frac is gone, and pt_div_q8 takes a u16 numerator (den >= 128).

The camera's corner deflection was the other u24 user. The move is now
int16 (under 12 units in any direction); M . v drops to 1/64-unit steps so
each product fits u16, and the correction is q14. The collision probe's
closest approach moved 29.1728 -> 29.1405 (tripwire 28); the sweep's
camera walk is unchanged. The slide loop now computes each candidate's
current distance once, not once per iteration.

Four exact savings in the renderer: push_child and push_root hand the end
vertex they found to the frame instead of the walk transforming it again;
a vertex's depth reciprocal is normalised once in transform and reused by
every row projection there (pt_project_col/pt_project_rowd take it);
row_step skips the multiply at dx = 0 or on a flat line (43% of calls
were at dx = 0); resolve_block reuses the line rows it already has.
Lookups per frame 494 -> 240 on the centroid sweep, transforms 20 -> 15;
sweep byte-identical.

## 2026-09-27 — Visit records, resume, line extension, and a pillar

Nothing kept per-cell state across visits: every visit searched for its
first edge from the twin and allocated fresh pieces, and §7.10's line
extension was never built. Rich's design: a small cache of visit records,
created on a cell's first visit of the frame (a one-byte index per cell,
0 = none), each holding a back-reference so the frame's end can clear the
indices by walking the cache - cost proportional to the cells actually
seen. The record carries the walk cursor as a ring index 0..num_edges-1
with the current edge's two vertices, the last finished visit's right
column, a live flag, and the active piece of each of the edge's four line
slots. Visits of one cell never nest, so the cursor lives there and the
frame shrinks to span, chains, and a pending start. The per-cell index
sits in pt_render (cell_visit[]) rather than pt_cell, so the map stays
read-only to the renderer; the 6502 code is the same byte table either way.

Resume: a later visit's first edge can be no earlier than the edge the
last visit stopped on (every edge before it ends left of that visit's xr,
which is at or left of the new xl), so the search continues from there,
usually stopping at once. Frames are now pushed pending and search when
they start: an edge split by closed columns pushes several children of
one cell together, and starting late lets each resume from the cursor
its left sibling left. The live flag skips a nested visit should boundary
rounding ever produce one. The map now stores each edge's twin (the spec
always said so; pt_map_twin had been searching the neighbour's ring).

Extension: a visible range starting exactly where the same slot's active
piece ends, with the same gradient and colour, only moves x1 - across
clip blocks within a visit and across the seam between visits. The
checks catch splits and merges the child walks make to bound pieces in
between; a piece carried over from a previous visit can only open the
chain being built, never follow its tail.

The demo gained a thin (24-unit) pillar in the middle of room 2, the room
split into four convex quarters by soft diagonals from its corners to the
pillar's so both doorways stay whole (cells 8 and 19..21; 21 cells now).

Verification: with extension disabled the sweep is byte-identical to the
pillar-map baseline, and across 66,157 resumes in the sweep and tests the
resumed edge always matched the search from the portal. With extension,
273 of 1172 sweep frames change by one-row shifts, and the worst are
fixes: a shallow wall edge split at a clip block used to drop its
fraction at the re-anchor and meet the next wall a row off; extended, it
steps at the right column and joins exactly. On this map extension is a
modest saving (pieces 25.9 -> 24.6 per frame, plotted 18.1 -> 16.9, pool
high-water 123 -> 103): most lines take the fast path whole, and round
the pillar the visits leave gaps. Up to 17 cells are visited in a frame
here, so PT_MAX_VISITS is the full cell count for now; the 6502 cap wants
sizing from real maps. The spec's §7.1, 7.2, 7.4, 7.5 and 7.10 describe
the new walk.

## 2026-09-27 — Visit state slimmed to what is implied

The first cut of the visit cache carried started/done/live flags, a
last_xr, the active slots' edge index, and a stored left column per
frame. Rich pointed out the state was all implied, and it went:

- A frame's left column is its chains' frontier, max(tcur.x0, bcur.x0),
  once the push splits the chain heads at it (the child's first cut made
  that split anyway, so no piece is added). Without the split, a piece
  extended from the cell's previous visit could head the chain left of
  the span: 1927 of 262066 test starts. process_edge's span clamp is now
  just max(e0.sx, frontier).
- A frame has started when its cell's record holds its xr - two visits
  of a cell never share a right column. No record means a first visit
  (search from P); the record's xr equal to the new left column means a
  contiguous revisit, whose active pieces carry over, and any other
  value a gap, which clears them. The monotonic order (record xr <= new
  left column) is a debug assert; it never failed.
- A frame pops as its last edge begins, so that edge's children take its
  slot: no done flag, and a shallower stack.
- The live guard never fired (0 of 262066 starts). Should rounding ever
  nest a cell in itself, the outer frame now sees the foreign xr when it
  is back on top and just restarts from its own frontier.
- Forgetting the cursor when a ring runs out of steps was moot: all 25
  such frames in the tests were the camera's cell, never revisited.
- The active slots are cleared whenever the cursor advances (and on a
  gapped revisit), which replaced tracking the edge they belong to.

The frame is now cell, xr, the two cursors, the extents, steps, and the
pending start (P and its edge); the record is cell, k, xr, the edge's two
vertices and the four active pieces. Byte-identical on the sweep and the
pillar poses; 25/25 tests.

Later the same day, Rich asked why frames were pushed pending at all.
Only siblings needed it - several frames of one neighbour from one edge,
when closed columns split the portal's span - and an edge pushing more
than one frame occurred 0 times in 262066 test pushes and 4934 sweep
pushes (it needs a V-shaped bound). Frames are now pushed ready: the
push sets up the record's cursor (a first visit searching from P, a
revisit resuming), so the frame loses P and its edge index (9 bytes) and
the walk loses its per-step started test. Siblings fall out of a
property the walk already had: a cursor that starts early is harmless,
because every edge before a frame's true first edge ends at or left of
its left column and clamps to an empty span. So when the left sibling is
pushed and finds the right sibling's xr (right of its own xl) in the
record, it just searches from P again; the right sibling later walks on
from the cursor the left one leaves, and restores the record's xr as it
pops. To exercise a path the map never takes, a scratch build split
every open range of 4+ columns in two (150283 forced siblings in the
tests, 4059 in the sweep): 25/25 tests with asserts, and renders equal
to the unsplit ones but for one-row re-anchoring shifts at the forced
seams. The committed pending version crashed on the same forced split:
contiguous siblings shared a chain piece across their seam, and the right
one's cursor followed it into the left one's relinked chains. Real
siblings are separated by a closed range whose emits split the pieces at
its ends, so that was latent; splitting the heads at push removes it.
Sweep and pillar poses byte-identical.

Then the premise went too. Rich could not picture how one edge would push
two frames, and rightly: the spec's "closure in the middle needs a
V-shaped bound" was wrong, because such a bound cannot arise. Every
portal opening is convex on screen - the root is the screen, and a
child's opening is the parent's intersected with the region between the
edge's two straight bound-forming lines, the chains being exactly
max(C, T) and min(F, B) - so its open columns are one interval, and
closed columns only trim a span's ends. Only rounding on an opening
under about two rows tall can pinch it in the middle. So an edge now
pushes at most one child: the widest open range, with any other open
columns closed after the fact by the same CB ^ FB fix-up EORed into the
bottom chain over exactly those columns (a sliver of flat colour at
worst). The open-range arrays, the insertion sort, the sibling search
restart and the xr restore at pop are gone; step 5 is a left-to-right
walk over the open ranges (next_open) plus a push. The pinch has never
occurred; a scratch build closing the right half of every portal
exercised the fix-up (flat child-ceiling colour in the closed halves,
everything below them intact). Sweep and pillar poses byte-identical;
25/25 tests.
