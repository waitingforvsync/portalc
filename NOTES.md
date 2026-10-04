# Notes: the polar branch

The design notebook for `polar`, the rebuild of capture and rendering for
frame rate. `main` keeps the old engine, and I switch branches in place to
compare. JOURNAL.md stays the history; this is where the thinking lives,
tried and kept or tried and dropped.

## Where it stands

- **Phase 0, done.** The branch, and the ray-cast comparer in `src/tools`
  (below).
- **Phase 1, done (awaiting commit).** The tables, built the way the 6502
  will build them, and the polar front end's primitives in `arith.c`.
- **Next, phase 2.** The map in the new format (shared edges) and collision
  on the new angles.

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

That's against about 900 now. With map offsets under 2,048, a component's
high byte is 0 to 7, so normalising for the log is a lookup and at most
two shifts.

**Turning is free.** A vertex's world angle and log length depend only on
where the camera stands, not which way it faces. Cache them per vertex
while the camera stays put, and a frame that only turns does no polar work
at all: its cached angle less the camera's, and a column lookup, about 25
cycles a vertex. The cache also stops a vertex being worked out twice in a
frame. About 4 bytes a vertex. Planned for phase 3.

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
| `a2c` | 256 | view angle to column | 0 |

`lsec` only covers the view, 45 degrees either side. Projecting vertices
that are in front of the eye but out of view would need it towards 90,
where the deltas get far too big for a bit each. That's for phase 3 to
settle.

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

- **Phase 2:** `pt_dir8`, `pt_div_q8`, and the builder's and collision's
  `pt_rot`.
- **Phase 3:** `sin_`, `pt_rot`, `pt_ray_basis`, `recipf`, `sbyte`, and the
  reciprocal, division, gradient and projection helpers.

`nrm` and `k8` stay, for normalising logs. `pt_mulg_u16` may stay too: a
line restarting partway along wants an exact gradient times columns.

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

My estimates, per column:

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
distance and two angles) is kept for vertices behind the eye only. Its
weak spot is a grazing wall: the cos of the angle between the ray and the
wall's normal is near 0, and 1024 steps a turn magnify the error there
(Doom wobbles even at 8192). First thing to check in phase 3.

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
eye 1 and 6 units off every vertex; about a minute a map. `main`'s
baseline:

| map | frames | pixels wrong |
|---|---|---|
| 0 | 22,768 | 1,892,314 |
| 9 | 22,032 | 8,152,722 |

Map 9's is high because of the R1 shelf, where the eye is above its
ceiling. The reference walks today's per-cell edges and twins, so it moves
to the new map format with phase 2.

## The phases

0. **The branch and the comparer.** Done.
1. **Tables.** Done: built as the 6502 will, with tests against floating
   point, and `main`'s renderer unchanged.
2. **Map and collision.** Edges as their own array, shared by both cells
   (vertices, the angle byte, each side's cell and colour); cells as rings
   of edge indices with a direction bit; the builder, the checks and
   `camera.c` on the new angles. The comparer follows.
3. **Capture and lines.** Each vertex's polar form, column and log depth,
   with the vertex cache. The walk; `top` and `bot`; the ceiling strip; lines
   clamped and toggled into T as they're made, with the fast path; closing;
   a record per clip.
4. **Fill and clear.** T into the back buffer, then T cleared from the
   records.
5. **Sprites.** After the fill, spritescale's pixels exactly, clipped by
   the saved rows.
6. **Measure and write up.** Against `main`, then the journal and the spec.

The tests are rewritten as each part lands. `beeb/` stays as it is, and
won't build on `polar` until the port. The C comes first, looking right,
before any 6502; but everything is written to be 6502-shaped.

## Open questions

- **Off-view vertices.** For one in front of the eye but out of view:
  project it (and extend `lsec` towards 90 degrees, built some other way
  than a bit a delta), or clip at the view's edge?
- **Grazing walls behind the eye.** How bad is Doom's clipper at 1024
  steps?
- **`pt_log2` the 6502's way.** Switch the C from its shift loop to the
  `nrm` and `k8` tables now, or leave that for the port?
