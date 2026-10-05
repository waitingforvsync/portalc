# Notes: the polar branch

The design notebook for `polar`, the rebuild of capture and rendering for
frame rate. `main` keeps the old engine, and I switch branches in place to
compare. JOURNAL.md stays the history; this is where the thinking lives,
tried and kept or tried and dropped.

## Where it stands

- **Phase 0, done.** The branch, and the ray-cast comparer in `src/tools`
  (below).
- **Phase 1, done.** The tables, built the way the 6502 will build them,
  and the polar front end's primitives in `arith.c`.
- **Phase 2a, done.** The map in the new format: shared edges, and rings
  of sides stored backwards.
- **Phase 2b, done.** Collision on the new map: the cell tracked by
  angles, through the vertex cache.
- **Phase 3a, done (awaiting commit).** The walk on the vertex cache's
  angles and the per-column clip.
- **Phase 3b, done (awaiting commit).** The lines from the logs: an end in
  view by `lsec`, one out of it by Doom's clipper, the near clip in screen
  space. View space is gone.
- **The review, done (awaiting commit).** Work done twice, done once; the
  vertex cache by vertex; the side's quadrant stored; every type explicit.
  Pixels unchanged.
- **Next, phase 4.** The toggle buffer, and clearing it from the clips'
  records.

The phases are at the end.

## Why change anything

The micro-optimisation farm got the demo from 17.78 to 17.98 frames a
second. Not worth it. I wanted an algorithmic win, so first, where a frame
goes. Two frames from map 0's demo pose (1756, 1400, cell 6), measured on
the emulator with `frame.py`:

| cycles | typical (angle 56) | heaviest (angle 112) |
|---|---|---|
| clear | 21,777 | 21,777 |
| fill | 36,914 | 36,914 |
| plot | 12,259 | 23,977 |
| sprite pass | 11 | 73,522 |
| capture | 47,594 | 132,933 |
| ... making lines (side clips, projections, gradients) | 13,485 | 30,032 |
| ... vertex transforms | 6,925 | 25,203 |
| ... clipping and row evaluations | 18,960 | 48,738 |
| ... the walk | 6,799 | 26,356 |
| **total** | **118,555 (3.0 vsyncs)** | **289,123 (7.2 vsyncs)** |

Three things stand out:

- **The front end is over 40% of capture.** The heavy frame does 28
  transforms for 21 distinct vertices, at about 900 cycles each with the
  projection.
- **The clip is the other 40%.** Chains of pieces mean crossings (a
  division each), splits to recolour, twins and `yf`.
- **Clear and fill are fixed.** 58.7k every frame, half the typical one, at
  their per-byte floor (9 and 5.3 cycles a byte). Even with a free capture,
  fill, clear and plot top out around 28 frames a second.

## The shape of it

Kept from `main`: convex cells, portals, the front-to-back walk with its
visit cache, and the EOR fill. A raycaster would be simpler with a flat
cost per column, but it gives up heights (the pit, the stairs), and once
you add them back it turns into a per-column portal renderer anyway.

Changed, roughly Doom's architecture with the EOR fill as the rasteriser:

- **The front end goes polar.** Each vertex's angle from the eye, and its
  column by table lookup. Depths, rows and gradients in the log domain. No
  rotation, no multiplies, no divisions.
- **The bounds become per-column arrays.** Lines are clamped and toggled as
  they're made, straight into a separate toggle buffer. No pool, chains,
  twins, `yf` or line extension.
- **Sprites go after the fill,** drawn over the finished picture by Baron's
  spritescale plotter.

The target: a Master, code in a 16K sideways RAM bank, main RAM for the
generated tables, two 4K screens, the toggle buffer and runtime data.
NMOS opcodes only, for older machines. Shadow RAM can be written while it
isn't displayed (ACCCON, &FE34), but I'm not using it. The vertical dither
stays (it's how a 4-colour mode gets more colours), and the screen's as
small as I dare already.

## Angles

### 1024ths of a turn

A world angle runs from +x towards +y in 1024ths of a turn, 10 bits. The
top two bits are the quadrant, the low byte the angle within it. The
quadrant comes free from the signs of dx and dy, so anything that stores
an angle (an edge, say) stores only the byte. An edge's other side is half
a turn round: the same byte, the quadrant from the signs the other way. So
one edge record can serve both its cells, and edges become an array of
their own.

The camera keeps 256 steps a turn, four of these each.

1024 is the coarsest that never skips a column. A column is 0.9 degrees
wide in the middle of the screen and only 0.45 at the edges, and a step is
0.35 degrees: 2.5 steps a column in the middle, 1.3 at the edges.

### The quadrant in seven instructions

From my old beebdoom prototype:

```
    LDA dy+1 : ASL A        ; C = dy's sign
    EOR dx+1 : ROL A        ; bit 0 = dx's sign EOR dy's
    PHP                     ; keep it: it decides the EOR below
    ROL A : AND #3          ; bit 1 = dy's sign: the quadrant
```

Quadrant 0 is +x +y, 1 is -x +y, 2 is -x -y, 3 is +x -y.

**On an axis.** For one byte to serve both sides of an edge, a direction
and its reverse must always be two quadrants apart. Signs alone break that
on the axes: +y has a zero dx, which counts as positive, so it lands in
quadrant 0 at the very end, while -y lands at the start of quadrant 3. So
`pt_quadrant` has a direction along an axis start its quadrant (+x 0, +y 1,
-x 2, -y 3): a zero dx counts as negative when dy is positive, a zero dy
when dx is negative. Two zero tests on top of the seven instructions.
Axis-aligned walls are common, so this matters.

### Steps, and their middles

Within a quadrant, the angle is atan(minor / major), from whichever axis is
the major one. The table floors, and the other octant takes `EOR #255`:
255 - phi rather than 256 - phi. With a floored table that gives the step
the true angle lies in, in both octants. One conditional EOR covers the
octant and the odd quadrants: it happens when steep and odd differ.

So a computed angle is a step, and the step's middle is angle + 0.5. The
tables that take an angle are built at the middles. Two indexing tricks
fall out:

- A view angle (world less camera) in view is in [-128, 128): an `int8_t`.
  Its table index for the column is `v EOR #$80`.
- A negative view angle's middle is |v| - 0.5 from straight ahead, so the
  depth table takes `v EOR #$FF` (`~v`).

### The ratio byte

atan and the length factor are both indexed by one byte, 256 * minor /
major. I tried three ways to get it, over a grid of offsets out to 2,000
units:

| ratio byte from | worst angle error (steps) | mean | over 0.75 steps | cost |
|---|---|---|---|---|
| logs: exp(log minor - log major) | 1.32 | 0.38 | 10.8% | ~100 cycles |
| reciprocal table and a multiply | 1.23 | 0.30 | 2.7% | ~120-150 |
| exact shift-and-subtract division | 0.80 | 0.27 | 0.2% | ~250-300 |

Logs win. The major component's log is needed for the length anyway, so
the ratio costs one more log, a subtract and an exp. The reciprocal route
would keep a reciprocal table, the normalising tables and a quarter-square
multiply, for no speed. The exact division's 0.8 is the ratio byte's own
resolution, and not worth 200 cycles a vertex.

### Logs

Logs are 256 * log2, in 16 bits. `pt_log2` takes the leading one's place as
the whole part, and the 8 bits under it, rounded, index a fraction table.
`pt_exp2` is the reverse: a 9-bit mantissa from a table, shifted by the
whole part (the last shift's carry rounds).

`pt_log2` finds the place and the bits the 6502's way. `k8` gives the high
byte's bit length, `nrm` lines its bits up under its leading one, and the
low byte's go under them, shifted down k - 1 places, the last bit out
rounding:

```
    LDX hi : BEQ small
    LDA nrm,X : ASL A : STA t        ; hi's bits under its leading one
    LDY k8,X : LDA lo : CLC
    DEY : BEQ join                    ; k = 1: lo is the rest, exactly
.sh LSR A : DEY : BNE sh            ; C is the last bit out
.join ORA t : ADC #0                ; carry out: the next octave's 0
```

Under 256 it's exact: `nrm`'s byte with its leading one shifted out. The
offsets in half units are under 4,096, so a vertex's logs shift at most
three times. Same answers as the shift loop, all 65,535 of them.

`pt_exp2` under 8 * 256 gives a byte, and its 9-bit mantissa never needs
a second one: the 1 comes in from the carry on the first shift.

```
    LDA expf,X : SEC : ROR A     ; 1.f >> 1, the 1 from the carry
    LSR A                        ; 7 - n times
    ADC #0                       ; the last bit out rounds
```

A carry out of that is 256. The ratio byte in `pt_polar_of` is always one
(the minor never exceeds the major, and the carry, or a difference of 0,
is 45 degrees), and so is a shallow line's gradient.

| | worst error |
|---|---|
| `pt_log2`, all 16-bit values | 1.15 units (0.31%) |
| `pt_exp2`, results of 512 or more | 0.25% |
| `pt_polar_of`: the angle | 1.32 steps (mean 0.38) |
| `pt_polar_of`: the length's log | 2.4 units (0.6%, mean 0.58) |

A vertex's polar form is its angle and the log of its length:

```
    log r      = log major + lhyp[ratio]       ; r = major / cos(phi)
    log depth  = log r - lsec[v]               ; depth = r cos(v)
    row offset = exp(log h + log K - log depth)
    gradient   = exp(log |drow| - log dcol)
```

### What a vertex costs

My estimate for the 6502, before writing any of it:

| step | cycles |
|---|---|
| offsets, signs and quadrant | ~50 |
| the two logs | ~100 |
| the exp for the ratio byte | ~40 |
| atan and lhyp, the angle and length | ~35 |
| view angle, in-view test, column, log depth | ~40 |
| **total** | **~280** |

That's against about 900 now. With offsets under 4,096 half units, a
component's high byte is 0 to 15, so normalising for the log is a lookup
and at most three shifts.

**Turning is free.** A vertex's world angle and log length depend only on
where the camera stands, not which way it faces. Keep them while the
camera stays put, and a frame that only turns does no polar work at all:
the cached angle less the camera's, and a column lookup, about 25 cycles a
vertex.

The cache goes by vertex, the way the visit cache goes by cell: a vertex
has its entry's index, 0 for none, and an entry its vertex, so emptying
the cache clears just the ones it holds. An entry is made the first time
its vertex is needed, nothing eagerly. It went cell by cell at first, an
entry a cell's ring, and on map 0 a frame worked out 21.6 polar forms for
12.7 distinct vertices: every vertex a cell shares, again. By vertex it's
11.8 (only the ones used), at 4 bytes an entry, 260 bytes for 64
vertices. Collision fills it first: the move is tested from the new
position, which is where the frame's capture then looks from, so the two
share the angles.

### No column-to-angle

beebdoom had a column-to-angle table, and I took it out there too.
Off-portal rows are interpolated (below), and Doom's clipper only clips at
the view's edges, which sit at exactly -128 and +128. The walk compares
columns. Nothing needs it.

## Tables built in code

### A bit a delta

The 6502 builds each table of a smooth function from its first entry and
one bit per delta after it, plus a base for each byte of bits. The bits
come out by ROR through the carry. The first byte holds fewer than 8, with
a set bit above them. That bit, carried round into each next byte, ends up
above its 8, so a byte has run out when ROR leaves it zero (the beebdoom
trick):

```
    LDA #first : LDY #0 : LDX #nbytes - 1 : CLC : BCC start
.loop
    ADC #0                      ; the delta's bit
    CPX #run : ADC #0           ; the byte's base, where runs need it
.start
    STA table,Y : INY
.next
    ROR bits,X : BNE loop
    DEX : BPL next              ; the set bit, in the carry, goes on
```

The last add of an entry must leave the carry clear, because ROR shifts it
in. The bases run in monotonic runs where they can, so a CPX chain makes
them.

`arith.c` builds the tables exactly like this: it packs the bits from the
ideal values, as Baron's FOR loops would at assembly time, and unpacks
them with the loop above. The quarter squares come from the recurrence
f(x + 1) = f(x) + floor((x + 1) / 2), with the second table the first
mirrored.

### The tables

All bytes:

| table | entries | what it gives | bases by byte |
|---|---|---|---|
| `sinq` | 256 | quarter sine | 1, then 0 |
| `atan` | 256 | the in-octant angle of a ratio byte | 0 |
| `lhyp` | 256 | log2 of length over major, same byte | 0 |
| `logf` | 256 | log2's fraction for a mantissa | 1, then 0 |
| `expf` | 256 | 2^x's mantissa for a fraction | 0, then 1 |
| `lsec` | 128 | -log2 cos across the view | 0, 1, 2 |
| `lsx` | 128 | -log2(sin x / x), small angles | 0 |
| `a2c` | 256 | view angle to column | 0 |

Past 45 degrees `lsec` climbs too fast for a bit a delta: to 2,137 at the
last step. Its second half is 16 bits, made from logs. The sine of a small
angle is the angle, short by a factor, so for step j's middle, x = (2j +
1) pi / 1024:

```
    -log2 sin x = -log2(pi / 1024) - log2(2j + 1) + lsx[j]
```

The constant is 2,137 in 256ths. That's 128 calls of `pt_log2` at start
up, and the whole table is within 1.14 units (0.3%). Read backwards it's
log sine, which the view-edge clip wants (Off the view, below).

`nrm` and `k8` go an octave at a time. The bytes of bit length k
normalise to 128, 128 + 2^(8 - k), and so on, the step halving each
octave:

```
    LDX #1 : LDY #1 : LDA #128 : STA step
.octave
    LDA #128
.entry
    STA nrm,X : PHA : TYA : STA k8,X : PLA
    INX : CLC : ADC step : BCC entry
    INY : LSR step : CPX #0 : BNE octave
```

### The sine's 1.0

From index 246 (`PT_SIN_ONE`) on, the rounded sine is 256. A multiply by
sine compares the index first, and skips both the table and the multiply.
Below that, the table's only 0 is at index 0, so the zero case needs no
compare of its own:

```
    CPX #PT_SIN_ONE : BCS one       ; the multiplicand, as it is
    LDA sinq,X : BEQ zero
    ...                             ; multiply by A
```

A cosine is sin(256 - a), so it's 1 for a <= 10, and never 0 otherwise
(its zero is the next quadrant's a = 0). 256 - a is (a EOR $FF) + 1, so
read one entry on:

```
    CPX #256 - PT_SIN_ONE + 1 : BCC one
    TXA : EOR #$FF : TAY
    LDA sinq + 1,Y
```

Axis-aligned walls make both shortcuts common.

### What goes when

`main`'s 256-step sine is now derived from `sinq`, so `main`'s renderer and
collision are untouched: every record the comparer writes is identical.
The old helpers go as their users do:

- **Phase 2, done:** `pt_dir8`, and the builder's and collision's
  `pt_rot`. `pt_div_q8` stays for now: the posts' deflection divides by
  it.
- **Phase 3, done:** `sin_`, `pt_rot`, `pt_ray_basis`, the negated
  products, the lerp, and the division, gradient and projection helpers.

`nrm` and `k8` stay, for normalising logs, and `pt_mulg_u16`: a line
restarting partway along wants an exact gradient times columns.
`pt_div_q8` went too, with `recipf`, `sbyte` and the normaliser: the posts
divide by a byte table now (Collision).

## Types

Every value is held in the type the 6502 will hold it in, and the C says
so. `pt_u8`, `pt_u16` and `pt_i16` narrow with an assert that the value
fits, so the tests, the sweep and the comparer check every narrowing at
the width the port will have. A plain cast wraps on purpose: an angle's
`& 1023`, a low byte. The library builds with `-Wconversion`, so no
narrowing goes unwritten.

The camera's position is u16.8, 24 bits, in a `uint32_t` in the C.

## The map

### Edges and sides

An edge holds only what its two sides share. Where it runs, and between
which cells, belongs to the rings:

```
edge   angle    the direction's byte within its quadrant
       colour   the shade + 1 of its faces, 0 for soft
side   v        the start vertex; the end is the next side's
       edge
       adj      the cell across, 0 for a solid wall
       qt       quadrant | (twin << 2)
```

The angle byte needs no direction: either way round, it's the same byte,
and only the quadrant differs. So the side keeps its own quadrant, in the
low two bits of `qt`, and with the edge's byte that's its world angle,
with no vertices fetched and no signs tested. The rest of `qt` is `twin`,
the edge's position in adj's ring, which saves searching the child's ring
for the way back in every time the walk enters a cell; it's under 8 here,
and could be up to 63. An edge's length comes back if collision wants it.

### Rings stored backwards

A cell keeps `first`, its ring's base in the sides, and `num`, which holds
its count LESS ONE. The ring is stored counter-clockwise, and still walked
clockwise: a base pointer at `sides + first`, and positions counting down
from `num`, which is the cheap way round on the 6502:

```
    DEY : BPL ok
    LDY num                 ; below 0: round to the last position
.ok
```

Everything that names a side by ring, like a side's `twin`, uses these
positions, 0 to `num`, not indices into the sides.

### One colour an edge

A riser shows only to the cell below it, and a lintel only to the cell
under it, so nearly every edge has one visible face. In the demo scene,
one portal edge shows faces both ways, and its two colours agree. The
sides that show nothing still carry colours in the scene source (19 in
the demo scene, 3 in the stress map), like `none` looking down into the
pit. So the builder keeps the authoring call (a colour for each side, as
its cell sees it) and takes the colour of the side that shows a face. It
stops if two showing sides ever disagree, or if one side is soft and the
other isn't.

### The old view, gone

The old renderer read per-side records, `pt_half`: start vertex, cell
across, twin and colour, clockwise. Through phase 2 the builder made them
alongside the sides, so `main`'s pictures stayed byte-identical until
phase 3a replaced the renderer. Now the map doesn't hold them at all. The
builder keeps its own clockwise halves as scratch, as the scene names
them, to match twins and make the edges, and the tests and tools read the
sides.

The Beeb port's host tools follow `main`'s map, so on `polar` they're off
unless `PORTALC_BEEB_HOST` asks for them.

## Collision

### How beebdoom did it

Which cell: find the edge the move heads for (the one whose vertices'
angles, from the old position, bracket the move's angle), then look at its
two vertices from the new position. If their order has swapped, the point
is across: into the next cell, and again. Walls: for each edge the move
faces, the distance from the new point to its line by a dot product with
its normal (two sine-table multiplies); inside the radius, a portal adds
the cell beyond to the edges checked, and a wall, unless the point is past
either end, pushes the point back out along its normal, leaving the slide.

### What I did

`camera.c` keeps its old shape (candidate walls and posts, resolve, three
slide iterations, the hop walk) with new pieces:

- **The cell, by angles from the new position only.** A point is inside a
  cell when every side's vertices run clockwise as seen from it: the
  angle from a side's start to its end is under a half turn. A portal side
  that fails is crossed, so hop and look again. No old position's angles
  needed, and they're exactly the angles the frame's capture will look
  with, so the camera's cell and the capture's view can never disagree.
  On `main` that took an exact cross product near each line, and
  `root_exact`'s retry.
- **Ties to the lower cell.** Exactly a half turn apart (the point on the
  side), the lower-numbered cell keeps the point. Both cells give the same
  answer, so the hop walk can't ping-pong. Level (the point on the side's
  line, past an end) isn't a tie: that side isn't the one being crossed,
  and if the point is outside, the side round from that end says so. I had
  level tying as well at first, and an eye on the line of a portal's
  neighbour hopped into a cell it was outside of.
- **The vertex cache,** by vertex: an entry the first time a vertex is
  needed, its polar form from where the camera stands. The camera owns
  it, and fills it after each move; the capture reads it. A move that
  doesn't change the whole-unit position keeps it, so turning on the spot
  costs nothing.
- **Dot products for walls.** A side's frame is two dot products against
  its direction (the side's stored quadrant, over its edge's byte):
  across it first, along it only when close. Two sine-table
  multiplies each, against four through `pt_rot` before. If collision ever
  needs to be cheaper: the cache already holds each vertex's angle and log
  length, so the distance across a side is r sin(angle off the side), one
  log sine lookup and an exp, no multiplies at all. The catch is that
  log sine near zero is steep, exactly where collision wants it accurate.
- **The edge's length** joins the edge, for the foot cap.
- **The posts stay.** They're cheap, and they keep the radius off wall
  ends, which keeps the eye off vertices.
- **The posts' division is a byte table.** A post pushes the move out by
  its radial part, 1.25 (M . v) v / |v|^2, overcorrected by a quarter so a
  grazing arc drifts out rather than grinding in. So 1/|v|^2 needn't be
  good. `rpost` holds 1.25 * 2^15 / d2 for each 8 of d2 over the circle's
  [256, 1600), 168 bytes indexed by d2 >> 3, rounded up from each bucket's
  start so the factor stays in 1.25 to 1.30. The division and the quarter
  are then one 16 x 8 fractional multiply: about 90 cycles, against 160
  for the reciprocal division and its quarter, or 170 by log and exp. And
  the reciprocal table, `sbyte` and the normaliser go. It's built by
  restoring division at start up, 8 steps an entry.

The facing step comes from the quarter sine table, four 1024ths a camera
step: the same values as before.

### How it comes out

Over the 40,000-tick walk the collision probe takes, the closest the camera
came to a wall was 29.04 units (radius 40; it was 29.17). That wanders
with any change to the walk: with the cell-tracking fix of phase 3a it
was 29.11, and with the posts' byte table 28.47. Over ten seeds it runs
28.1 to 29.2 with the table and 28.0 to 29.6 with an exact division, so
the table costs nothing there. The probe trips at 28, which an exact
division nearly hit on one seed. The cell it
tracks differed from exact point location in 79 ticks, by at most 0.28 of a
unit. The fan probe now allows exactly that: a point d from a side's line
sees its vertices d (1/r0 + 1/r1) radians off a half turn apart, and each
angle is within 1.82 steps, so the point may lie up to 3.64 steps' worth
beyond.

## The walk

Phase 3a: the walk finds its way by the vertex cache's angles, and the
clip is the per-column bounds. The lines themselves are still `main`'s,
made in view space from rotated vertices, then clipped and plotted the new
way. Phase 3b makes the on-screen ones from the logs.

### Columns from angles

An edge's columns are its vertices' view angles (the world angle less the
camera's) through `pt_view_col`. Left of the view is 0, right of it 128.

The catch is what's left and right once a vertex is behind the eye. My
first go unwrapped each edge on its own, its end a clockwise turn on from
its start. That breaks when a cell comes round the back of the eye. Stand
close to a portal and the cell beyond can start behind you on the right
and come round into the view on the left:

```
side   view angles    each edge on its own   in the window
k=1    495 .. 511     128 .. 128             -529 .. -513   0 .. 0
k=0    511 .. 661       0 .. 0               -513 .. -363   0 .. 0
k=3   -363 .. -25       0 .. 54              -363 .. -25    0 .. 54
```

The walk went in at k=1, took it for the last edge (it ends at 128), and
drew nothing.

So each visit has a window, a whole turn, `[lo, lo + 1024)`, that its
cell's angles are read in. A cell seen through a portal spans under a half
turn, the portal's middle among it, so a window centred there holds the
whole cell in one piece, and clockwise along the sides facing the eye the
angles only go up. The eye's own cell spans the whole turn: its window
starts where the walk does, at the side across the view's left edge.

The visit keeps its window as `lo`, its start, a world angle, so a
vertex's place in it is one 10-bit subtract, t = (angle - lo) & 1023; and
`tl`, where the view's left edge sits in it (the right is 256 on), so two
compares give the column 0, the column 128, or the view angle, which is
the byte t + `lo8`, and `pt_view_col`. A child's window comes from its
parent's: `lo` moves by the portal's middle less 512, and `tl` and `lo8`
the other way.

A side's end is the next side's start, so the visit carries the current
side's two places and columns, and moving on costs one vertex: its place
and column. It used to work the columns out afresh each time it asked,
2.8 times a side, two vertices each.

### Facing away

A cell seen through a portal has a chain of sides facing away from the eye,
the portal among them. In the window, each one's end comes out at or left
of its start, so it spans nothing. The ones just after the portal end at or
left of its left column, so entering a visit skips them like any edge that
ends left of the span.

A side facing the eye turns by up to a half turn, and its end is taken as
its start plus that. In a child's window that's where the end is anyway.
In the eye's own cell, the walk's first vertex sits at both ends of the
window, and the last edge has to run on round to it, a turn up, to reach
the right of the view.

### The eye in the middle of its square

The vertex cache looks from the middle of the camera's whole-unit square,
so no vertex is ever exactly at the eye. The lines' view space now does
the same: offsets in odd half units, rotated, then rounded back to whole
units. Before, the walk and the lines looked from points 0.71 of a unit
apart. The comparer's reference moved too (below).

## The clip: bounds per column

### One pair of arrays

The walk finishes a column's whole subtree before it moves on. So one
global pair, `top[128]` and `bot[128]`, updated in place, does what chains
of pieces do now. When an edge's child is walked, its bounds overwrite the
parent's over the edge's columns. The parent never needs those columns
again, and its later edges are elsewhere.

Clipping a line is, column by column:

1. step its row;
2. compare it with two bytes;
3. toggle one pixel: at its own row where visible, at the bound's row where
   hidden;
4. for a line forming a bound, store its row as the child's.

(That was the plan. How it's done now, with fast paths and runs, is under
Drawing a line.)

No products, divisions, splits, twins or `yf`. And ties stop mattering: a
line on the same row as its bound toggles the same pixel either way.

The rest carries over:

- **The ceiling strip.** The camera cell's ceiling colour along row 0, at
  the start: the root's top bound.
- **The root's bottom.** Row 64, in the junk row, as now.
- **Closing.** A line that closes the child toggles `colour ^ cbfb` at the
  clamped row. The child is only walked over its open run, so nothing
  counts twice.

### Overdraw, measured

When I prototyped per-column bounds before, every clamped line overdrew
the bound. In the EOR fill a line only ever needs one toggle per column,
so the overdraw is exactly the hidden columns, one pixel each. Counted
from the C, against the pixels `main`'s plotter draws (which folds hidden
columns into the bound pieces' colours):

| per frame | typical | heavy | mean of 256 angles | `end_room_pit` |
|---|---|---|---|---|
| line-columns | 298 | 530 | 334 | 447 |
| hidden (clamped) | 28 | 67 | 118 | 67 |
| toggles, immediate (+128 for the strip) | 426 | 658 | 462 | 575 |
| pixels plotted on `main` | 397 | 591 | 344 | 538 |

7 to 34% more pixels, against everything the plot pass and the pool cost.
I'd thought about rendering bound pixels lazily: XOR hidden colours into a
byte per column, and plot each bound pixel once when it's final. It saves
about 15 cycles a hidden column, under 2k a frame, and needs a flush pass.
Not worth it.

### What it costs

My estimates before phase 3, per column (the measured version is under
Drawing a line):

- **Slow (clamped) column:** about 75 cycles. Step, two compares, a toggle,
  maybe a store.
- **Fast-path line,** wholly inside the visit's extents: about 25 a column
  through a byte-at-a-time plotter, and about 20 more where it forms a
  bound and its rows are stored. The fast path covers 240 of the typical
  frame's 298 line-columns, and 387 of the heavy one's 530.
- **Per line:** about 200 to set up.

That's about 13k on the typical frame against `main`'s 31k for clipping,
row evaluations and plotting, and about 31k against 73k on the heavy one.

### Gradients, and rows off the portal

Lines step by a gradient, which the log domain makes cheap: one subtract
and an exp.

Where a line's vertex lies outside the visit's span, its row at the span's
first column is interpolated from the projected vertices: row0 + g (c -
col0), with an exact multiply. The same vertices always give the same g,
so a line split across two visits carries on exactly where it left off,
with no line extension and no stored pieces.

Doom's clipper (the wall's depth along a ray, from its perpendicular
distance and two angles) does every end off the view, in front or behind.
Its weak spot is a grazing wall, measured under Drawing a line.

## Drawing a line

How a line gets from an edge to the screen in phase 3a, and how it gets
past the bounds without a compare for every pixel.

```
an edge's span, [xa, xb)
  |
  +-- make the line: its rows at the ends, a gradient, cut at the screen
  |
  +-- clip it, as plain, floor or ceiling:
        1. the screen's edge: the cut-off columns, no scan
        2. the fast path: both ends inside the extents, plotted whole
        3. the scan: a step and two compares a column
             visible stretches --> the line plotters
             hidden stretches  --> the flat plotter, or nowhere
```

### Half open: [sx, ex)

Still. A vertex's column is the first whose centre lies right of it
(`a2c` reads the middle of the vertex's angle step), and an edge covers
`[sx, ex)`: the columns whose centre rays cross it. The next edge starts
at `ex`, so each of a visit's columns belongs to exactly one edge. An edge
from column 40 to 60 does 40 to 59, and the next one picks up at 60.

`main` took `floor(64 + 64 y/x)`, the column the vertex is in, so its
edges sat up to half a column left of the pixel centres. The reference
casts through the centres, so the angles round the right way.

The walk cuts the edge's span to the visit's: `xa = max(sx, x)` and `xb =
min(ex, xr)`, `x` being where the visit's last edge ended and `xr` its
portal's right column. If that's empty, the edge is skipped. The side
clips make the only other ends: a vertex outside the view is clipped at
its ray, column 0 or 128.

### A line

```
pt_line   x0, x1   the columns it's on the screen over, within [sx, ex)
          y        its row at x0, u8.8, with the rounding half in
          g        rows a column, sign bit + u7.8
          below    the side it's cut off on: under the screen, or over
          ce, co   its colour: the even and odd scanlines' patterns
```

A line's row at `sx` is its start's own, to a 256th of a row, and the
gradient takes it to the end's at `ex`. With half a row in the fraction,
the nearest row is just the high byte.

`[x0, x1)` is usually `[sx, ex)`. A line nearer than x = 2h (h its height
off the eye) projects past the screen's top or bottom, so `make_line` stops
it at the column where it reaches the edge. The columns cut off read as
beyond the screen on that side: row 0 above, 0xFFFF below. An edge too near
at both ends makes a line with no columns at all, and that still matters:
every column of its span reads as off the screen.

### Ends from logs

An edge's two ends are worked out once, for all its lines, from the
vertex cache's polar forms:

- **In view,** an end is its vertex: the column its angle gives, and its
  depth's log, the length's less `lsec` of its view angle. A lookup and a
  subtract.
- **Out of view,** in front or behind, it's where the edge crosses the
  view's edge ray, column 0 or 128. That's Doom's clipper (Off the view).

Then each line, at height h in 8-unit steps, gets its ends' rows as
offsets from the screen's middle, and a gradient from their difference:

```
    offset = 2^(17 + log h - log depth)            ; 64 * 8h / depth, u8.8 rows
    g      = 2^(log |offset1 - offset0| - log (ex - sx))
```

The rows run down from the middle below the eye, and up above it.

An end with an offset of 32 rows or more is at or past the screen's edge:
nearer than 2h. The line runs off the screen between its ends, (32 rows -
the far end's offset) / g columns from the far end, which is one more log
and exp. That's screen space: a line stays straight when it's projected,
so no depth is needed. A near end past 256 rows saturates u8.8, and
flattened the gradient of any wall ending a unit or two from the eye, so
there the difference is taken in whole rows instead.

An end's at or past the screen's edge when its offset's log reaches 13 *
256, the high byte 13: `CMP #13`, and the carry says near, before any exp.
ceil(delta) is the high byte plus one for any fraction: `CMP #1` on the
low byte, `ADC #0` into the high. Delta is logs and can be a column over,
so the count comes back to the last column whose row is still on the
screen.

The columns kept are whole ones, counted from the far end. A floor line
whose far end is 20 rows below the middle at column 40, and whose near end
is 80 rows below at column 70, has g = 2 and delta = 6. It keeps columns
40 to 45, offsets 20 to 30; 46 would be on the screen's bottom edge.
Mirrored, near end at 40 and far end at 70, it keeps 65 to 69, and its
anchor is the far end's row stepped back 5 columns, one `pt_mulg_u16`.
(Column 70 is the next edge's: half open.)

Rows stay bytes wherever the clip and the plotters look: `top` and `bot`,
and a line's `y`, a row byte and a fraction byte, as `main`'s anchor
always was. Only line making goes wider: an offset is u8.8 up to 256 rows,
and the saturated path's whole rows are 16 bits.

On the 6502, my estimates: about 250 cycles a line (`log h`, two exps for
the rows, two logs and an exp for the gradient), and 150 more for a near
clip. An end is about 15 in view and 90 out of it. The typical frame's
15 lines come to about 5.5k, against `main`'s 20k for making lines and
transforming vertices. The polar forms come on top, about 280 a vertex,
but a frame that only turns doesn't redo them.

### Off the view

The eye's distance from the edge's line comes from an end in view: d = r
|sin(a - w)|, where r and a are the end's length and angle, and w the
edge's direction (its angle byte, with the quadrant from its vertices). A
ray at angle b meets the line d / |sin(b - w)| along, and on the view's
edge ray (b the camera's angle, less or plus 128) the depth is that times
cos 45 degrees. In logs:

```
    log depth = log r + log sin(a - w) - log sin(b - w) - 128
```

`lsec` read backwards is log sine, so that's a few lookups and adds:
about 90 cycles, and it's the same for an end behind the eye.

The alternative for an end in front but out of view was screen space:
project it anyway (its column 64 + 64 tan v from the logs of sin and cos,
its depth through the long `lsec`), then run its line to the screen's
edge. That's an exp for the column, the row as usual, and another log and
exp for the row at column 0 or 128, about 160 cycles more than an end in
view against Doom's 75. Ends behind the eye need Doom's anyway.

So I measured both, over map 0's 20,000 random poses. These are the row
errors where the line meets the view's edge, for a line 10 steps off the
eye (rows past the screen's edge count as on it), by the angle between the
ray and the edge:

| graze | ends | Doom's mean | worst | over a row | screen space mean | worst |
|---|---|---|---|---|---|---|
| 0-2 degrees | 6 | 1.06 | 4.66 | 17% | 1.21 | 3.22 |
| 2-5 | 106 | 0.31 | 2.78 | 7.6% | 0.42 | 2.96 |
| 5-10 | 446 | 0.18 | 1.73 | 3.4% | 0.22 | 1.40 |
| 10-20 | 1,762 | 0.08 | 1.59 | 0.3% | 0.11 | 1.39 |
| 20-45 | 10,262 | 0.04 | 0.81 | 0 | 0.06 | 0.71 |
| 45-90 | 45,582 | 0.03 | 0.60 | 0 | 0.03 | 0.32 |

(Screen space only counts ends whose other end is in view, a few
thousand fewer.) As accurate, cheaper, and one path for every end: Doom's.

Two half steps mattered. The edge's angle byte is floored, so the angle
between the ray and the edge is centred half a step under b - w, and the
lookup goes at b - w - 1, whose middle is there. The end's angle is floored
too, so a - w falls between two steps' middles, and it takes the average
of both lookups (about 20 cycles more, on the 4 or so clipped ends a
frame). Before those, the worst grazing error was 5.3 rows.

**Grazing walls behind the eye.** The ends behind the eye do better than
the ones in front:

| graze | ends | mean | worst | over a row |
|---|---|---|---|---|
| 2-5 degrees | 30 | 0.08 | 1.47 | 3.3% |
| 5-10 | 135 | 0.07 | 1.91 | 1.5% |
| 10-20 | 491 | 0.03 | 1.02 | 0.2% |
| 20-45 | 3,082 | 0.02 | 0.79 | 0 |
| 45-90 | 12,583 | 0.01 | 0.39 | 0 |

Under 2 degrees the line is past the screen's edge. Grazing is rare,
under 0.3% of clipped ends below 5 degrees, and it's a wall seen nearly
end-on at the view's edge, where its line is near the horizon anyway. I'll
take a row or two there.

### What an edge makes

A wall makes two lines, its ceiling and its floor. A portal from cell A
into cell B makes up to four, two a side. On the ceiling side:

| B's ceiling | plain line | bound line (B's top) |
|---|---|---|
| lower (a lintel) | A's ceiling, `A ceil ^ wall` | B's ceiling, `wall ^ B ceil` |
| level | A's ceiling, `A ceil ^ B ceil`, unless that's 0 | none: B keeps A's top |
| higher | none | A's ceiling, `A ceil ^ B ceil` |

The floor side is the same upside down: a riser (B's floor higher) makes a
plain line at A's floor, `wall ^ A floor`, and the bound at B's,
`B floor ^ wall`.

The plain lines go first, against the bounds as they are. Then the floor
bound writes B's bottom over A's, and then the ceiling bound writes B's
top, against B's bottom (it closes B where it meets it). B is the first
run of columns left open.

### Three clips

Each clip runs a line over `[xa, xb)` against `top[c]` and `bot[c]`. On a
bound counts as hidden:

| line | on or above the top | between | on or below the bottom |
|---|---|---|---|
| plain | toggle at the top | plot | toggle at the bottom |
| floor (B's bottom) | close B: toggle at the top, `colour ^ cbfb` | plot; it's B's bottom | lend A's bottom: toggle there |
| ceiling (B's top) | lend A's top: toggle there | plot; it's B's top | close B: toggle at B's bottom, `colour ^ cbfb` |

A toggle at a bound's row is the line's colour change pushed onto the
bound. The fill can't tell it from the line plotted there. `cbfb` is B's
ceiling ^ floor: where B is closed, its whole stack telescopes to that.
Where the floor line closed B, the ceiling line is above the top as well,
and merges into it with nothing worked out.

Later runs of open columns (a sliver pinched shut by rounding) are closed
after the fact, a `cbfb` toggle at B's bottom a column. So is a child past
the depth cap.

### The ways through

Cheapest first:

1. **No colour.** A plain line with nothing to change is dropped. (A level
   side whose shades match isn't even made.)
2. **The screen's edge.** If the visit's bound on the line's side is the
   screen's edge in every column (`tmax` 0 for a line above the eye,
   `bmin` 64 for one below), the columns the line was cut off over are
   known without a look. Off the top they're all hidden on row 0: one flat
   run. Off the bottom they're in the junk row: nothing. A floor line's
   stay open there, lending the junk row, or close B on row 0; a ceiling
   line's merge into row 0, open or not. That leaves the line's on-screen
   columns.
3. **The fast path.** If the line's row where what's left starts, and
   `rl` at its far end, are strictly inside the visit's extents, it's
   visible in every column, and plotted whole. A bound line stores its rows as it goes. The ceiling
   line's fast path also needs the floor line to have closed nothing, and
   tests against B's highest bottom.
4. **The scan,** for the rest.

### The extents

`tmax` is (at least) the lowest top row over the visit's portal, and
`bmin` (at most) the highest bottom row. A row with `tmax < row < bmin` is
inside in every column. A line's rows run from one end's to the other's,
so two compares settle the lot: its row at the span's start, and `rl`, a
row `make_line` keeps at or past its last column (the far end's, so no
multiply).

The bound lines give them as they go: a scanned line keeps its extreme
row, column by column, and a fast-path line's are its ends'. A level side
or a soft edge passes on the parent's. They stay true while the visit's
edges are walked: a child only writes its bounds over columns its parent
has finished with.

They're conservative. One deep notch in a portal's bottom takes the fast
path away from every line in it. Measured, three quarters of the columns
still go through it (below).

**A bound at the screen's edge isn't kept.** `tmax` 0 says the top is row
0 all along, and `bmin` 64 that the bottom is the junk row. The clips then
use those, and don't read `top` or `bot`, so the root's bounds are never
written, and nothing resets the arrays each frame (that was 256 stores).
A bound line that lends its parent's bound writes it out, since the child
may keep the rest of its bound in the array.

**Open columns.** A child's open columns are all of them until a bound
line closes one, and only then does `open[]` get filled and kept. Every
column open is the usual case, and needs no `open[]`, and no scan for the
child's run.

### The scan, by example

A wall's floor line from row 50 at column 40 to row 54 at column 60, so
g = 0.2, in a visit whose bottom is row 56 except for a notch at row 52
over columns 50 to 53 (a nearer step, say). `bmin` is 52, and the line
ends on 54, so no fast path:

```
column    40 .. 47  48  49 | 50  51  52  53 | 54 .. 59
row       50 .. 51  52  52 | 52  52  52  53 | 53 .. 54
bottom    56 .. 56  56  56 | 52  52  52  52 | 56 .. 56
          visible          | hidden         | visible
```

Three plotter calls: the line over 40 to 49, a flat run on row 52 over 50
to 53, and the line over 54 to 59, starting from the row and fraction the
scan stepped to. Nothing toggled a pixel at a time, and no multiply to
pick the line up again.

Hidden columns are gathered like visible ones: a run on one row in one
colour, ended when either changes. A run in the junk row is dropped.

On the 6502, a column of the scan is a 16-bit add, `CMP top,X`, `CMP
bot,X` and a branch or two: about 30 cycles. The runs' bookkeeping only
happens where visible meets hidden.

### Rows the plotter's way

The scan steps the row exactly as the plotter does, the same 16-bit add of
`g` a column from the same start. So the rows it compares are the rows
that get plotted, and a run starting mid-line takes the scan's row and
fraction as its own.

A multiply only comes in where a span starts away from the line's anchor:
`line_y` works out `y + g (c - x0)`, the sum the adds would make. That's a
line split across two visits, or an edge starting left of its portal. The
near clip's ends take one too. The fast path's test at the far end reads
`rl` instead.

The scan goes in three stretches: before the line's own columns and after
them the row is beyond the screen, the same all along, and between them
it steps. On the 6502 that's three loops, none comparing the column with
the line's. `make_line` keeps a line's own rows on the screen, 0 to 64,
so the step needs no saturating either: for a shallow line it's the
fraction's add and `ADC #0` into the row byte.

### The plotters

All of them move a screen cursor (a character block's address and Y),
placed once at the run's first pixel and then only moved: a row down or
up, a byte right, a pixel right. A toggle EORs both scanlines of its row,
the even and the odd pattern.

- **Flat** (g = 0: hidden runs, the ceiling strip, flat lines). Whole
  bytes on one row, the ends masked. No arithmetic at all.
- **Shallow** (|g| < 1). The row moves at most one a column, so it's a row
  byte and a fraction, and each step's carry says whether the next column
  is a row on. The odd pixels at either end go one at a time. Whole bytes
  go four pixels at once: the three carries inside a byte pick one of
  eight fixed routines, each with its row masks as constants, and a fourth
  says whether the next byte starts a row on. The same trees as `main`'s
  6502.
- **Steep** (|g| >= 1). A pixel pair a column, moving on by the gradient's
  whole rows plus the fraction's carry.

### Off the screen, measured

The worry: a line hidden behind a bound costs a compare and a toggle a
column, and that could eat what the angles save. So I counted, over map
0's 20,000 random poses. Per frame: 15.3 lines, 57% of them on the fast
path, 230 columns scanned, and 185 hidden columns toggled one at a time.
Of those 185:

| | |
|---|---|
| in the junk row | 108 |
| on row 0 | 60 |
| on the same row as the column before | 7 |
| no colour | 6 |
| anything else | 3 |

Nearly all of them are a line running off the screen against the root's
bounds. So:

- **Hidden runs.** Hidden columns on one row in one colour are gathered
  like the visible ones, and go to the flat plotter as a run, a byte at a
  time with the ends masked.
- **The junk row is dropped.** Nothing there is ever seen.
- **The screen's edge.** Where the visit's bound on a line's side is the
  screen's edge all the way across (`tmax` 0, or `bmin` 64: the root, and
  any visit that kept its bounds), the columns the screen pre-clip cut off
  are known without a scan. Off the top, a flat run on row 0. Off the
  bottom, nothing. A floor line's lend the junk row and stay open, or
  close the child on row 0; a ceiling line's merge into row 0.

| per frame | map 0 | map 9 |
|---|---|---|
| lines clipped (the rest all cut off) | 12.8 | 16.6 |
| on the fast path | 72% | 77% |
| columns plotted by the fast path | 167 | 111 |
| columns scanned | 57 | 42 |
| hidden runs plotted | 3.8 (5.7 bytes) | 4.9 (8.9 bytes) |

The pictures are byte-for-byte what they were. Scanning 57 columns a frame
is about 1.7k cycles, so the per-byte early-out I had in reserve (the
lowest top and highest bottom over each byte's columns) isn't needed.

### The review, measured

A sweep of the code for work done twice, types wider than they need, and
carries and tables that could save a byte or a step. Per frame over map
0's 20,000 random poses (map 9 in brackets), before and after:

| | before | after |
|---|---|---|
| vertex places worked out | 57 (79) | 16.2 (25.4) |
| polar forms, the camera having moved | 21.6 (29.4) | 11.8 (16.8) |
| logs | 42.2 (53.9) | 32.0 (42.8) |
| exps | 44.3 (57.3) | 39.7 (49.7) |
| multiplies for a span's start or a fast-path test | 12.8 (10.8) | 5.9 (1.5), near clip's too |
| columns passed over for a child's extents | 84.5 (17.0) | 0 |
| columns scanned for a child's open run | 84.5 (34.7) | 0 (19.2) |
| stores resetting `top` and `bot` | 256 | 0 |

My guess at the 6502 is about 10k cycles a frame the camera has moved,
nearer 7k when it has only turned. The pictures are byte-for-byte the
same: the sweep's framebuffers, every comparer record, and the collision
probe.

What it took: the vertex cache by vertex; a side's ends carried along the
walk; the extents from the bound lines; open columns kept only once one
closes; screen-edge bounds not kept; `rl` for the fast path's far end; the
scan in three stretches; a cell's heights and their logs once a visit, the
columns' log once an edge; the near test on the log, the ceil on the
carry; the byte exp. And the side's quadrant stored with its twin.

Not worth it, measured: a row-address table for the steep plotter (about
4 columns a frame are steep), per-side line colours in the map (2 KB for
about 0.5k cycles), a log sine at whole steps (512 bytes for about 90
cycles), and exp shift tables (2.5 KB).

## Buffers

### The toggle buffer

Plotting during capture means capture can't write into the back buffer:
that stays on screen until the pending swap. Overlapping capture with that
wait was worth half a vsync a frame on `main` (3.36 vsyncs a frame down to
2.85). A separate toggle buffer, T, keeps it:

1. capture plots into T while the swap is pending;
2. the fill reads T and writes the back buffer (`EOR T,X : STA S,X`, 9
   cycles a byte, as now), writing every byte, so the screens never need
   clearing;
3. T is cleared;
4. sprites go into the back buffer;
5. swap.

T in screen layout keeps the plotters' addressing and the fill as they
are. Only T needs junk rows for plot overruns: the screens only get the
fill (rows 0 to 63 exactly) and clipped sprites. So buffer B can sit right
up against &8000 without its overruns landing in the code bank.

On a 65C12 the fill could clear T as it goes with `STZ T,X`: 14 cycles a
byte against 14.3 for fill then clear. NMOS only, so no.

### Clearing T from the lines

A full clear is 21.8k. Not storing pieces seemed to rule out clearing only
the bytes the lines touched, but a small record per clipped line is
enough: x0, x1, the row at x0 (8.8) and the gradient, 6 bytes.

Why that covers everything: a hidden toggle lands on its bound's row, and
a bound's row in a column is always where some bound-forming line was
visible there, or the ceiling strip's row 0, or the junk row 64. So zeroing
every recorded line's own rows over its columns, plus row 0, clears every
toggle, hidden ones included. The junk rows are never read, so they never
need clearing.

The zeroing is plain `STA (blk),Y` with A = 0 on both scanlines of each row
a line touches in a byte column: no read, no mask, about 33 cycles a byte
column a line. About 4k on the typical frame and 9k on the heavy one.

### All together

Very roughly, for the typical frame: fill 37k, clear 4k, capture and plot
about 25 to 30k. Around 70k, 1.75 vsyncs, against 118.6k now. The heavy
frame comes down from 289k to perhaps 180k before the sprite change. All
estimates, from inner loops on paper; the C prototype and then the 6502
will say.

## Sprites after the fill

Drawn over the finished picture, far to near, by Baron's
`examples/spritescale` plotter. The C sprite pass reproduces its pixels and
rounding exactly: `PY` from a half-step start, columns stepped by the same
accumulator. Its two plotters (the stretch engine and the per-pair rebuild)
give the same bytes, so the C needs one.

**The clip rows.** Each visit to a sprite's cell copies `top[c]` and
`bot[c]` over the sprite's columns within the visit's span. Columns no
visit covers stay empty. A column's visible rows are `[top, bot)`, the
portal's own. The screen's top and bottom are the root's case of the same
thing.

**Into the plotter.** `plot_draw` is event-driven a strip at a time, so a
vertical clip is just more events:

1. a strip wholly inside its four columns' clip rows runs as it is (most
   strips);
2. if all four pixel columns share their clip rows, the strip's start and
   length are trimmed, as `skiploop` steps past transparent pairs;
3. otherwise each slot gets up to two clip events, in the change handler's
   min with the run ends, and a clip mask ANDs into the data bytes and ORs
   into `mask`.

The close-ups that cost up to 500k on `main` come down to the sprite's
area on screen: no toggle list to keep, no world toggles to erase under it.

## Considered and dropped

- **Dropping the vertical dither.** Fill the even scanlines and store each
  result to both (`EOR e,X : STA e,X : STA o,X`, 7 cycles a byte), clear
  and plot only the even ones: 58.7k down to about 40k, and the plot about
  halved. But checkerboards become stripes, and the dither is where a
  4-colour mode gets its colours. No.
- **A smaller screen.** It's as small as I dare.
- **A raycaster.** No heights.
- **Lazy bound pixels.** See the overdraw section.
- **An exact ratio division.** See the ratio byte.
- **Column to angle.** Not needed.

## The comparer

`src/tools/refcmp.c`, `portalc_refcmp`, renders against an exact reference:
every pixel centre's ray cast through the cells in floating point. A pixel
counts as wrong when it matches the reference neither on its own row nor a
row up or down. Sprites are off.

```
portalc_refcmp MAP OUT                      # every pose of map 0 or 9
portalc_refcmp MAP OUT.ppm X Y ANGLE CELL   # one pose: frame | reference | wrong
src/tools/refcmp.py OLD NEW                 # two runs against each other
```

The poses are every cell's centroid at 64 angles, 20,000 at random, and the
eye 1 and 6 units off every vertex; about a minute a map.

The reference casts from the engine's eye. On `polar` that's the middle of
the pose's unit square, (x + 1/2, y + 1/2) (`EYE_OFF` in `refcmp.c`), in
whichever cell holds it. A pose with that point in no cell (half a unit
into a wall: only the poses a unit off a vertex manage it, and the camera
keeps 40 clear) is skipped, recorded as 0xFFFF, and `refcmp.py` leaves it
out of both runs.

An eye exactly on a portal broke the reference: a ray heading out through
that side has to leave at once, at t = 0, and the cast only took exits
ahead of it. So from the eye, a ray also leaves by a side at t = 0 if it's
heading out through it. The first baseline had 117 whole frames like that
on map 0 (the eye on x = 512 and so on), with an all-black reference.
Rebuilt from `main`'s engine (commit c77115f, its eye at the square's
corner, `EYE_OFF` 0) with the fixed cast, in `build/refcmp/base2_0.bin`
and `base2_9.bin`:

| map | frames | pixels wrong, first baseline | rebuilt |
|---|---|---|---|
| 0 | 22,768 | 1,892,314 | 1,048,940 |
| 9 | 22,032 | 8,360,458 | 6,523,190 |

Map 9's is high because of the R1 shelf, where the eye is above its
ceiling. The new map format moved it from 8,152,722, all in 346 frames
with the eye in R1: from above a ceiling the reference sees faces from
behind, and it now colours them with the edge's one colour. With the old
per-side colours put back, it matches `main`'s records exactly.

A ray exactly through a vertex ties two edges, so the reference breaks
exact ties by the lower edge number, whatever order the ring is stored in.

### Phase 3a

On the frames both runs keep:

| map | frames (skipped) | `main` | phase 3a |
|---|---|---|---|
| 0 | 22,706 (62) | 1,036,731 | 418,022 |
| 9 | 22,017 (15) | 6,468,692 | 6,292,368 |

On map 0 it's better in 12,807 frames and worse in 4,651, and the frames
with nothing wrong went from 4,661 to 5,954. The regressions are small:
5 frames worse by more than 128 pixels, and none by 1,024. Two kinds. A
vertical edge a column out: atan's worst 1.32 steps is about half a column
mid-screen, and the comparer forgives a row, not a column. And the eye
within rounding of a portal between floors at different heights (one
was 0.042 of a unit off, its turn 511.31 steps rounding to 512): the tie
goes to the lower cell, and the whole view is two steps up or down.

Map 9's big regressions are R1 again. Corridor poses on its portal line,
x = 1192, which `main` kept in the corridor, have their middle-of-the-
square eye in R1, above its ceiling. The camera has no step rule, so
nothing stops a walk in there either, but the map was never meant to be
walked.

### Phase 3b

The lines from the logs, on the same frames:

| map | `main` | 3a | 3b |
|---|---|---|---|
| 0 | 1,036,731 | 418,022 | 368,080 |
| 9 | 6,468,692 | 6,292,368 | 6,238,134 |

Against 3a, map 0 is better in 11,707 frames and worse in 2,483, the
worst by 34 pixels. Against `main`, the frames with nothing wrong went from 4,661 to
8,317, and 3 are worse by more than 128 pixels. On map 9, 9,332 frames
have nothing wrong, against 6,210.

## The phases

0. **The branch and the comparer.** Done.
1. **Tables.** Done: built as the 6502 will, with tests against floating
   point, and `main`'s renderer unchanged.
2. **Map and collision.** Done: edges holding angle, colour and length,
   rings of sides stored backwards, the builder and checks, the comparer's
   reference; collision on dot products, the cell by angles, and the
   vertex cache.
3. **Capture and lines.** Done: (a) the walk on the vertex cache's
   angles, with a window a visit; `top` and `bot`; the ceiling strip; the
   clips, with the fast path, runs and the screen's edge; closing. (b) The
   lines from the logs, ends off the view by Doom's clipper, the near clip
   in screen space.
4. **Fill and clear.** A record per clip, T into the back buffer, then T
   cleared from the records.
5. **Sprites.** After the fill, spritescale's pixels exactly, clipped by
   the saved rows.
6. **Measure and write up.** Against `main`, then the journal and the spec.

The tests are rewritten as each part lands. `beeb/` stays as it is, and
won't build on `polar` until the port. The C comes first, looking right,
before any 6502; but everything is written to be 6502-shaped.

## Open questions

None for now.

Settled:

- **Columns by angle** are precise enough. A vertical edge a column out
  now and then is fine.
- **`pt_log2` the 6502's way,** through `nrm` and `k8`, built as the 6502
  will. Done.
- **Off-view ends** by Doom's clipper, measured against screen space above.
- **Grazing walls:** a row or two of error, taken over anything slower.
  Performance comes first.
- **The vertex cache** goes by vertex, made on need, like the visits.
- **A side's quadrant** is stored, as quadrant | (twin << 2).
- **The posts' division:** a byte table, the quickest, with error only on
  the side of more overcorrection.
