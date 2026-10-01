# BBC Micro 2.5D portal engine — design spec, draft 9

Consolidates the original `portal.md`, the review, and the follow-up discussions on
edge finding and clipping. Decisions taken so far are stated as decisions; what is
still open is in §14.

---

## 1. Map format

- The scene is a set of convex **cells** ("subzones"), each a ring of vertices in
  clockwise order as viewed from above.
- Each cell has a floor height, a ceiling height, a floor colour and a ceiling colour.
- Each cell edge holds a wall colour, an adjacent-cell index, a hard/soft flag, and
  the index of its twin edge in the adjacent cell (so the child walk can start without
  searching).
  - **Soft edge**: the adjacent cell has identical attributes; the edge exists only
    to keep cells convex (capture), never to draw anything.
  - **Hard edge**: adjacent index 0 means a solid wall. Otherwise it is a portal into
    the adjacent cell, and any floor step up / ceiling step down is drawn as wall.
- Every edge has exactly one neighbour. The editor splits edges at T-junctions so the
  shared edge between two cells is a single edge in both (twin edges).
- Vertices are stored in a shared vertex table; rings are index lists.
- A cell may hold one **sprite** (index, 0 for none; §9): a base position in world
  units, standing on the cell's floor, a height in world units (the width follows its
  art's aspect), and an index into the map's sprite art. It lies wholly inside the
  cell, its width taken as a disc around the base, and below the cell's ceiling, so any
  ray that meets it passes through one of the cell's portals.

## 2. Definitions

- **Edge**: a cell edge (world space).
- **Line**: a screen-space line, half-open in x: covers columns `[x0, x1)`.
- **Piece**: a line, or a sub-range of a line produced by splitting. Same record type.
- **Portal**: a column span `[xl, xr)` plus a top bound chain and a bottom bound
  chain. The whole screen is the root portal.
- **Chain**: an ordered list of pieces covering a portal's span exactly, with no gaps
  or overlaps, linked left to right.
- **Column of a vertex**: its integer screen x, `floor(sx)`.

## 3. Coordinate systems and projection

### 3.1 World

Right-handed as viewed from above: **x up (the page), y right, z away** (into the
page). So z increases downwards: a cell's ceiling has a smaller z than its floor.

### 3.2 View

Camera at the origin, looking along +x. View coordinates are `(x, y)` with
`x` = depth (forward) and `y` = lateral (right), from the world position relative to
the camera by a 2×2 rotation:

```
x =  rx·cos + ry·sin
y = -rx·sin + ry·cos
```

Heights are relative to the eye: `h = z − z_eye`. Ceilings have `h < 0`, floors
`h > 0`.

Two derived quantities drive the whole walk and are computed with `(x, y)`:

```
a = x + y      // ≥ 0  ⇔  on or right of the left frustum ray
b = x − y      // ≥ 0  ⇔  on or left of the right frustum ray
```

| `a`   | `b`   | Where the vertex is                          |
|-------|-------|----------------------------------------------|
| ≥ 0   | ≥ 0   | In the frustum (`x > 0`, `|y| ≤ x`)            |
| < 0   | ≥ 0   | Outside, left of the left ray                 |
| ≥ 0   | < 0   | Outside, right of the right ray               |
| < 0   | < 0   | Behind the camera (`x < 0`, `|y| < −x`)         |

A vertex behind the camera always has `a < 0` or `b < 0`; "behind" never needs its
own test.

### 3.3 Screen

MODE 5, 128 pixels wide, 2:1 (double-width) pixels, H·2 scanlines. The canvas the
engine works in is **128 columns × H rows**, where `H = PT_SCREEN_HEIGHT` (80 as
built; 64 throughout the worked examples below): a row is a pair of scanlines (§5),
and every line endpoint, bound and sprite run is placed in rows. x right, y down.
Horizontal half-FOV is 45°: 64 pixels either side of centre.

```
sx = 64 + 64·y/x            // column, 0..128
sy = H/2 + 64·h/x           // row,    0..H when 64·|h| ≤ x·H/2
```

A row is as tall as a pixel is wide and the vertical scale matches the horizontal,
so the picture is undistorted at any H; a taller screen widens the vertical FOV
(atan(H/128), ≈ 26.6° at H = 64, ≈ 32° at H = 80) rather than stretching the view.
A line's endpoint is on screen vertically iff `x ≥ 128·|h|/H` — the 64-row
screen's classic `x ≥ 2|h|`, `8|h|/5` at H = 80 (§7.7). Rows fit in a byte; row H
is used as the off-screen marker.

### 3.4 Column convention

```
column(v) = floor(sx)
```

A line for edge `P→Q` covers `[column(P), column(Q))`. Consecutive edges share the
vertex between them and therefore partition the columns exactly.

## 4. Arithmetic and tables

- **Sine table**: `sin(θ)·256`, 256 entries for 360°. Indices 0 and 128 are special
  cased (result 0); a stored 0 means 256. Never read out as a value: multiplying by
  `sin`/`cos` looks up the magnitude and lets the index's half turn and the other
  operand's sign pick the positive or negated product. Used only for the camera
  direction; may be made coarser.
- **Multiply**: quarter-square (difference of squares) tables, with the extra
  `(x−255)²/4` tables. The table MSB pointers are set once at start-up and never
  changed.
- **Reciprocals**: one byte per entry, 128 entries: `round(0x10000/n) − 256` for
  `n` in `[128, 255]`. Every denominator is normalised into that window (left
  shifts are exact), so `1/n` is `1 + f/256` at a known power of two and a division
  is one add plus a single fractional multiply; the numerator is pre-shifted when
  the result scale calls for it, so truncation is never amplified. The 512 at
  `n = 128` wraps to a stored 0 and is taken at one shift less (512 = 256 << 1), so
  the reciprocal is always `256 + f` and no multiply has a special case. The shift
  is kept as `e = s + 8` (0..15), as the 6502 normaliser returns it, so every
  quotient is a right shift by a constant offset from `e`.
- **Normalising tables**, three of 256 bytes indexed by a byte `b` with `lz(b)` its
  leading zero bits: `b << lz(b)`, `8 − lz(b)` (its bit length), and `s + 8` for a
  byte denominator (the power-of-two case folded in). A byte denominator is then
  pure lookups; a u16 one shifts right by the high byte's bit length, its
  normalised byte being the high byte's bits moved up ORed with the low byte
  shifted down, a one-byte shift instead of a two-byte one.
- Prefer unsigned values of the smallest type that fits.

### 4.1 Fixed-point formats

| Quantity                       | Format               | Range / notes                                        |
|--------------------------------|----------------------|------------------------------------------------------|
| World vertex                   | s16 integer units    | Editor keeps cells small enough for s16 view coords   |
| Camera position                | s16.8                | Fraction for smooth motion                            |
| `x`, `y` (view)                | s16                  | s16 × s0.8, keep high 16                               |
| `a`, `b`                       | sign only            | Only the carry/sign is used                            |
| `h` per line                   | s16                  | `z − z_eye`, per cell per frame                        |
| Clip parameter `t`             | u0.8                 | `[0, 1)`; 1.0 cannot occur in the clips of §7.6/§7.7   |
| `1/n`                          | u16 + shift count    | Normalised                                             |
| `sx`, column                   | u8                   | `0..128`; a u8.8 form is used once, in §7.7            |
| `sy`, `y0`                     | u8                   | `0..H`; a piece at `H` that does not rise is off screen |
| Gradient `g`                   | sign + u6.8          | `|g| < 64` once `dx ≥ 2`; `dx = 1` never uses `g`       |
| Line evaluation `y(c)`         | u8.8 (transient)     | `y0 + (c − x0)·g`                                       |
| Crossing quotient (§7.9)       | u7 columns, clamped  |                                                        |

## 5. Fill technique

Double-buffered shrunken MODE 5, 128 wide × H·2 scanlines (H rows; 5K per buffer
at H = 80). In-place fill
and clear for now (§8.3).

**EOR vertical fill.** For each byte column, walk downwards doing
`EOR row,X : STA row,X`. A plotted pixel toggles every pixel below it in that pixel
column, so filling costs nothing beyond plotting the boundaries. Steep lines
(`|dy| > dx`) need only one plot per pixel column.

**2×2 dither.** A colour is a pair of byte patterns `(E, O)`, one for even scanlines
and one for odd scanlines. The fill sweeps even scanlines vertically, then odd
scanlines vertically. A canvas row `y` is the scanline pair `2y` (gets `E`) and
`2y+1` (gets `O`); every plotted pixel is such a pair, so the dither alignment is
fixed by construction. Ten checkerboard patterns are available, which with
transparent gives the eleven-entry sprite palette of §9.

**Line colour rule.** In any pixel column, the fill's running value at a row is the
EOR of every line pixel plotted above that row. A line therefore changes the running
value by exactly its own colour. For the fill to go from the surface colour above the
line to the surface colour below it, the line's colour must be

```
colour(line) = colour(surface above) EOR colour(surface below)     // per E and per O
```

For example the bottom ceiling line of a portal, with lintel wall `W` above and child
ceiling `CB` below, has colour `W ^ CB` — not `W` as in draft 1, which would have left
the fill at 0 inside the portal. A line whose colour is `(0, 0)` is not plotted but
still exists as a bound.

**Parity property.** EOR is commutative and self-inverse, so two toggles at the same
row of the same column equal one toggle with the EOR of their colours. Consequences
used throughout §7.9:

- Clamping a line onto a bound is the same as EORing the line's colour into the
  bound's piece over those columns.
- A ±1 error in a line's row costs one wrong pixel pair; a gap or overlap in x costs
  a full-height streak. **x ranges must partition exactly; y may be approximate.**

## 6. Line records

```
x0      u8      first column (inclusive)
x1      u8      last column + 1 (exclusive)
y0      u8      row at column x0; y0 >= H starts off screen (a falling piece may come on later)
g       s7.8    gradient, sign bit + u7.8 magnitude, rows per column
cE, cO  u8      colour patterns; cE | cO == 0 means unplotted
next    u8      next piece in this piece's chain (0 = end)
```

8 bytes; 256 records = 2K. The pool is reset every frame. Chains are built from pool
pieces by `next`; a piece belongs to at most one live chain.

- **Evaluate** at column `c`: `y(c) = y0 + (c − x0)·g`, kept as u8.8 while in use.
- **Split** piece `p` at column `c` (`p.x0 < c < p.x1`): allocate `q` with
  `q.x0 = c`, `q.y0 = round(y_p(c))`, `q.x1 = p.x1`, `q.g = p.g`, `q.colour = p.colour`,
  `q.next = p.next`; then `p.x1 = c`, `p.next = q`. Re-anchoring keeps only the
  integer row, so `q.y0` **rounds to nearest** (add `0x80` to the fraction byte and
  carry into the row): each split shifts `q`'s pixels by at most half a row in either
  direction, which the parity property makes harmless. Truncating instead would lose
  the fraction in the same direction every time, and chained splits compound that
  drift. Keeping the fraction as well (a u8.8 `y0`) was tried and measured: over the
  sweep it moved lines by one row in places (two where neighbouring lines both moved)
  and never opened a sliver of three rows or more, so it is not worth the byte.
- **Relink**: a piece owned by the current edge (§7.9.2) may be moved from a parent
  chain into a child chain by appending it (`tail.next = p; p.next = 0`). The piece
  keeps its geometry and colour and is plotted once, from the pool.

Invariants: x ranges are half-open and split only at integer columns; pieces in a
chain are contiguous over any portal span (`p.x1 == p.next.x0`); each chain covers
its portal's span exactly. A piece belongs to at most one chain that will still be
walked.

## 7. Capture: the recursive walk

### 7.1 Frame structure

```
render_frame():
    pool.reset()
    rootT = alloc(x0=0, x1=128, y0=0,  g=0, colour=camera_cell.ceiling)   // plotted: initialises the fill
    rootB = alloc(x0=0, x1=128, y0=H, g=0, colour=0)                      // dummy, never plotted
    walk(cell=camera_cell, x=0, xr=128, T=rootT, B=rootB, start=first_edge_camera())
                                   // recursive: a child is walked before its parent carries on
                                   // entering a portal records sprite clip pieces (§9.3), leaving it issues sprites (§9.4)
    plot every pool piece with colour != 0, except those at y0 = H with g ≥ 0
    sprite_pass()                  // §9.5: reads the plotted toggles, adds sprite toggles and cancels
    fill (even pass, odd pass)
```

The walk is recursive (§7.5): a child portal is walked by a nested call, and the only
state a portal keeps across it is small. The cell's position in its ring lives not
there but in the cell's **visit record** (below), shared by every visit of that cell
in the frame:

```
portal:                                    // kept across a child's walk (on the 6502: saved around the JSR)
    cell         cell index
    x            where the next edge's span starts: the end of the last one
                 (the portal's left column to begin with)
    xr           right column bound
    Tcur, Bcur   current piece of the top / bottom chain (the piece containing the next column)
    Tmax, Bmin   conservative row extents of the chains over the span   (fast path, §7.9)

visit:                                     // one per cell visited this frame
    cell         back-reference to the cell
    k            the edge being walked, as a ring index 0..num_edges-1
    xr           right column of the latest visit entered
    V0, V1       view data of edge k's start and end vertices
    active[4]    last VISIBLE piece of each of edge k's ≤ 4 lines (§7.10)
```

A portal keeps two columns: `x`, where its next edge's span starts, and `xr`. `x`
only increases: each non-empty span moves it to its end, so when a child's walk
returns the portal carries on from its own `x`, whatever column the child reached (a
child trimmed at its right end by closed columns stops short of it). Once `x` reaches
`xr` every later span is empty. A child portal is entered ready to walk, its cell's
record already holding the cursor on its first edge (§7.4).

Rendering is a depth-first, left-to-right sweep: every visit to any cell during a
frame happens in increasing x order, which is what makes line extension (§7.10) valid
and makes chain ownership (§7.9) safe. Visits of one cell never nest (a straight ray
crosses a convex cell once, so a cell never appears in its own subtree), which is what
lets them share one cursor.

Visit records are created when a cell's first visit is entered, from a small cache:
each cell has a one-byte cache index (0 = not visited), set on creation. At the end
of the frame the cache is emptied by walking its entries and clearing each one's
cell index through the back-reference, so the cost is proportional to the cells
actually visited, a fraction of the map. The camera cell's record is begun by the
root push, already started.

### 7.2 Vertex transforms

No per-frame vertex cache. Each vertex is transformed when the walk reaches it, and
the visit record carries the current edge's two vertices from one edge to the next,
so a walk transforms each vertex once, as the end of one edge. When a child is
entered, the parent's edge start vertex `P` is the child's start vertex, so its view
data is passed down. A cell visited again resumes with the vertices of the edge its
last visit ended on (§7.4), so only the first visit's search re-transforms vertices
a neighbouring cell already did. The arithmetic is deterministic, so any vertex
transformed twice gets identical numbers, which §7.10 relies on.

The reciprocal of a vertex's depth (normalised once, §4) is part of its view data:
the column projection and every row projection at that vertex reuse it.

### 7.3 Classifying a vertex against a column

The walk is driven by one predicate: is vertex `v` left of, at, or right of screen
column `c` (`0 ≤ c ≤ 128`)? `cmp(v, c)` returns the sign of `column(v) − c` in this
sense:

| Quadrant (§3.2)   | `cmp(v, c)`                                                       |
|-------------------|-------------------------------------------------------------------|
| in frustum        | `sign(column(v) − c)`                                             |
| outside left      | `−1`                                                              |
| outside right     | `+1`                                                              |
| behind            | `sign(c − column'(v))`, `column'(v) = floor(64 + 64·y/x)`         |

The behind case is the inverted frustum: reflect the vertex through the camera,
project it (the expression `64·y/x` is unchanged since both signs flip) and compare
the other way round. A behind vertex has `|y| < −x`, so `column'` is in `0..128`.

This is the sign of the cross product `64·y − (c − 64)·x`, i.e. which side of the ray
through column `c` the vertex lies on. It is valid for any vertex, in front or behind,
provided the cell's vertices span less than 180° of bearing around the camera, which
holds for every cell the camera is not inside. For the camera cell only `c = 0` and
`c = 128` are ever tested, where the predicate is the sign of `a` or `b`.

No facing tests are needed anywhere.

### 7.4 Finding the first edge

Rings are clockwise, so the visible part of any walk moves left to right on screen.
The first edge is the one crossing the portal's left column: its start is at or left
of `xl`, its end strictly right of it.

**Camera cell** (`xl = 0`). Every ray from the camera crosses the ring once, and the
line `x + y = 0` crosses it twice: on the left frustum ray in front, and behind-right.
Walking clockwise, `a` goes from ≤ 0 to > 0 exactly once, at the left ray:

```
first_edge_camera():
    n = any vertex with a(v[n]) ≤ 0            // one always exists
    while a(v[n+1]) ≤ 0: n = n + 1
    return n                                   // a(v[n]) ≤ 0 < a(v[n+1])
```

The end vertex may be outside the right ray or behind; the edge is then clipped at
both rays (§7.6).

**Child cell**, entered through parent edge `P→Q`. The twin runs `Q→P`; start at
`P`, take edges clockwise, never walking the twin:

```
first_edge_child(P):
    n = edge starting at P                     // twin index + 1, from the map
    while cmp(v[n+1], xl) ≤ 0: n = n + 1
    return n
```

`P` is at or left of `xl` because the span is the parent's edge clipped. From `P` the
walk covers any front-chain edges left of the portal (end vertices moving further
left), the leftmost silhouette vertex, then the back chain, whose vertices move
rightwards. The first end vertex right of `xl` is on the back chain. The child is
convex, so every ray through the portal exits through the back chain exactly once:
the edges from the first to the last are the whole visible set.

**Revisited cell.** A cell visited earlier in the frame resumes instead. Its last
visit stopped on edge `k`, the first whose end vertex was at or right of that visit's
`xr`; every earlier edge of the back chain ends left of `xr ≤ xl`, so none of them can
be the new first edge. The search therefore continues from `k`, with `V0` and `V1`
already in the record, and usually stops at once:

```
child_portal(cell, xl, xr, T, B, P):       // the cursor is set up here; the portal is walked next
    V = cell's record
    if V exists:                               // an earlier visit, finished: resume
        if V.xr != xl: clear V.active          // V.xr < xl, a gap: nothing extends
    else:                                      // a first visit: search from P
        V = new record
        V.k = edge starting at P ; V.V0 = P ; V.V1 = transform(v[V.k + 1])
    while cmp(V.V1, xl) ≤ 0: advance V         // next edge; clears V.active
    V.xr = xr
    return portal(cell, x=xl, xr, T, B)
```

The resume always finds the edge the search from `P` would (checked over every
resume in the sweep and tests).

Since each edge has at most one child (§7.8), and a cell never appears inside its own
subtree, a child portal always finds its cell's record finished. Portals
never overlap horizontally and each new one starts at or right of the last, so a
revisit's `xl` is never left of the record's `xr`: equal means contiguous, greater
means a gap. There is no fallback for anything else.

### 7.5 The walk, the last edge, zero-width spans, termination

```
walk(p, depth):
    loop:                                      // one portal per pass
        V = p.cell's record ; steps = num_edges + 1
        loop:                                  // its edges
            if steps == 0: return ; steps = steps − 1
            last = cmp(V.V1, p.xr) ≥ 0         // end vertex at or right of the right column
            child = process_edge(p, V.k, V.V0, V.V1)   // §7.6–7.9
            if last:
                if no child: return
                p = child ; next pass          // tail call: the child replaces this portal
            if child: descend(child, depth + 1)
            advance V                          // V.k + 1, V0 = V1, V1 = transform(v[V.k + 1])

descend(child, depth):
    if depth > MAX_DEPTH: close child's columns (§7.9.4 fix-up) ; return
    walk(child, depth)
```

The recursion needs a stack only for children through edges before the last: the last
edge's child replaces its parent (on the 6502 a JMP), and the cursor stays on that
edge for a later visit to resume from. So the depth grows only with portals nested
along a line of sight, not with the number of edges walked. In the demo the deepest
frame nests 11 (typically 2–4); at about 10 bytes of 6502 stack per level (the portal
state above plus the return address) the cap of 16 fits in page 1 beside the MOS.
Past the cap a child's columns are closed instead of walked: the closed-column fix-up
(§7.9.4) stands in for its whole stack, so the opening shows its ceiling colour flat,
a draw-distance cutoff rather than a glitch.

For the camera cell the stop test is `b(V1) ≤ 0`.

- Only the first edge's start and the last edge's end can be outside the frustum: the
  first start is either in the frustum (child cells) or has `a < 0`; the last end is
  either in the frustum or has `b < 0`. All other vertices are in the frustum.
- **Zero-width spans**: after clipping to `[xl, xr)` an edge may cover no columns
  (consecutive vertices in the same column, or a vertex exactly on `xl`/`xr`). It
  generates no lines and no child, but the walk continues; the stop test is on the
  vertex, not the span. The first edge is never zero-width (its end column is
  strictly greater than `xl`).
- **Termination**: the back chain reaches a vertex at or beyond `xr` before it can
  reach the twin, so each walk ends. Each child span is contained in its parent's,
  and a straight ray cannot re-enter a convex cell, so no cell recurs along its own
  portal chain; skipping zero-width spans removes the rounding cases. The depth cap
  (above), the steps count and the pool size are the safety net.

### 7.6 Frustum (side) clipping

```
clip_left(V0, V1):                       // a(V0) < 0 ≤ ... ; a(V1) > 0 guaranteed
    t  = a0 / (a0 − a1)                  // u0.8, in [0, 1)
    x* = x0 + t·(x1 − x0)
    return endpoint(x = x*, sx = 0)      // sx assigned, not derived

clip_right(V0, V1):                      // b(V1) < 0; b(V0) > 0 guaranteed
    t  = b0 / (b0 − b1)
    x* = x0 + t·(x1 − x0)
    return endpoint(x = x*, sx = 128)
```

These are the two formulas from draft 1 (with `px` = depth, `py` = lateral),
verified numerically. The denominators are never zero and `t` is strictly inside
`[0, 1)` in the situations §7.5 allows. Only the depth `x*` is needed; the lateral
coordinate is implied by the ray (`y* = −x*` or `y* = x*`).

An endpoint needs only `(x, sx)` after this step; the `h/x` projections of §7.7 use
`x`, and everything else is in screen space.

### 7.7 Near clipping (bounding rows to 0..H)

A line at relative height `h` has row `H/2 + 64·h/x` at depth `x`, which leaves the
screen exactly when

```
x < 128·|h|/H      // off the top for h < 0 (ceilings), off the bottom for h > 0 (floors)
```

(`2·|h|` on the 64-row screen; `8·|h|/5` at H = 80, shifts and adds.)

`make_line` produces one line from the side-clipped endpoints `E0` (left, columns
from `E0.sx`) and `E1` (right), for a height `h` and colour `col`, pre-clipped to the
screen: where an endpoint is too near, the line stops at the column where it reaches
the screen edge.

```
make_line(E0, E1, h, col):
    d     = 128·|h|/H, rounded up
    edge  = (h < 0) ? 0 : H                          // the screen edge this line can leave by
    near0 = E0.x < d ; near1 = E1.x < d
    if near0 and near1:
        return empty                                 // x0 = x1: the whole span is cut off
    if not near0 and not near1:
        sy0 = H/2 + 64·h/E0.x ; sy1 = H/2 + 64·h/E1.x
        return line(E0.sx, E1.sx, sy0, (sy1 − sy0)/(E1.sx − E0.sx), col)
    // exactly one endpoint is too close: clip the edge at depth d
    (N, F) = near0 ? (E0, E1) : (E1, E0)             // near and far endpoints
    t   = (d − N.x) / (F.x − N.x)                    // u0.8
    y*  = N.y + t·(F.y − N.y)                        // lateral view coordinate at depth d
    sx* = 64 + 64·y*/d                               // u8.8, keep the fraction; 1/d cached per (cell, ceiling|floor) per frame
    syF = H/2 + 64·h/F.x
    c*  = ceil(sx*)
    if near0:                                        // near end on the left
        g   = (syF − edge) / (E1.sx − sx*)           // fractional dx: same reciprocal routine, extra shift
        y0  = edge + (c* − sx*)·g,  rounded to nearest and clamped to 0..H
        return line(c*, E1.sx, y0, g, col)
    else:                                            // near end on the right
        g   = (edge − sy0) / (sx* − E0.sx)
        return line(E0.sx, c*, sy0, g, col)
```

A line whose span comes out empty (`x0 ≥ x1`) is cut off entirely. The lateral
coordinate `N.y` of a side-clipped endpoint is `∓N.x` (on the ray); of an in-frustum
endpoint it is the transformed `y`.

The columns of `[E0.sx, E1.sx)` the pre-clip cut off are not dropped: there the line
is off the screen on `edge`'s side, beyond every portal bound, so it is clamped to the
bound on that side like any hidden range (§7.9), and the bound piece takes its colour.
That matters wherever the bound is on screen above the cut-off columns: looking down a
step into a cell whose far wall is inside that cell's (larger) near distance, the
wall's bottom line is off the screen while the step's lip, nearer but at a smaller
relative height, is on it; the lip's bottom bound must take `W^FB`, or the parent's
floor below it is miscoloured. `clip_edge_line` (§7.9.3) handles the cut-off columns
from the x spans alone: nothing is evaluated there.

Starting the sloped line at `ceil(sx*)` keeps its `y0` inside the screen and exact
at that column; the column `floor(sx*)` is cut off, at most `|g|` rows off, in one
column. After this step every `y0` is in `0..H` and every gradient of a line wider
than one column fits sign + u6.8.

Because the near clip runs after the side clip, the depths it sees are the
side-clipped ones, so it stays well conditioned when the camera stands against a
wall.

### 7.8 Generating lines per edge

Cell A is the current cell (ceiling z `cA`, floor z `fA`, ceiling colour `CA`, floor
colour `FA`), cell B the adjacent cell, `W` the edge's wall colour, `^` component-wise
EOR on `(E, O)` pairs. Remember z increases downwards.

| Edge type               | Lines, top to bottom                                        | Child top bound     | Child bottom bound |
|-------------------------|-------------------------------------------------------------|---------------------|--------------------|
| Solid wall              | `CA^W` at `cA`; `W^FA` at `fA`                               | —                   | —                  |
| Portal, `cB > cA`       | `CA^W` at `cA` (top ceiling); `W^CB` at `cB` (bottom ceiling)| bottom ceiling line | see floor rows     |
| Portal, `cB ≤ cA`       | `CA^CB` at `cA` (unplotted if `CA = CB`)                    | that line           |                    |
| Portal, `fB < fA`       | `FB^W` at `fB` (top floor); `W^FA` at `fA` (bottom floor)    |                     | top floor line     |
| Portal, `fB ≥ fA`       | `FB^FA` at `fA` (unplotted if `FB = FA`)                    |                     | that line          |
| Soft edge               | none                                                        | parent's, restricted| parent's, restricted |

A child floor lower than the parent's shows no drop face (it faces away), and a child
ceiling higher shows no riser: a single line each. Within one edge the four lines are
always ordered top ceiling ≤ bottom ceiling < top floor ≤ bottom floor at every
column, because they are projections of the same vertical segment.

```
process_edge(f, n, V0, V1):
    // 1. column span, then side clip
    if a(V0) < 0 and a(V1) ≤ 0: return                   // wholly outside the left ray
    xa = max(start column, f.x)                         // §7.1: the previous span's end
    xb = min((b(V1) < 0) ? 128 : column(V1), f.xr)      // the end's column needs no clip
    if xa ≥ xb: return                                  // zero width
    f.x = xb
    E0 = (a(V0) < 0) ? clip_left(V0, V1)  : (x=V0.x, y=V0.y, sx=column(V0))   // sx ≤ xa
    E1 = (b(V1) < 0) ? clip_right(V0, V1) : (x=V1.x, y=V1.y, sx=column(V1))
    // 2. take ownership of the bound sub-chains over [xa, xb)   (§7.9)
    T = cut(f.Tcur, xa, xb) ; B = cut(f.Bcur, xa, xb)
    e = edge[n] ; A = f.cell
    if e.soft:
        push_child(e.adj, xa, xb, T, B, P=V0) ; return
    if e.adj == 0:
        clip_edge_line(make_line(E0, E1, cA − eye, CA^W), T, B, none)
        clip_edge_line(make_line(E0, E1, fA − eye, W^FA), T, B, none)
        return
    Bc = cell[e.adj]
    // 3. non-bound-forming lines first: they only recolour, and need T and B intact
    if cB > cA: clip_edge_line(make_line(E0, E1, cA − eye, CA^W), T, B, none)
    if fB < fA: clip_edge_line(make_line(E0, E1, fA − eye, W^FA), T, B, none)
    // 4. bound-forming lines last, floor first (§7.9.3):
    //    F builds childB against T and B; C builds childT against T and childB
    F = (fB < fA) ? make_line(E0, E1, fB − eye, FB^W) : make_line(E0, E1, fA − eye, FB^FA)
    C = (cB > cA) ? make_line(E0, E1, cB − eye, W^CB) : make_line(E0, E1, cA − eye, CA^CB)
    closed = [] ; childB = chain() ; childT = chain()
    clip_edge_line(F, T, B,      build=childB, role=BOTTOM)
    clip_edge_line(C, T, childB, build=childT, role=TOP)
    // 5. the child portal: [xa, xb) less its closed ends   (§7.9.4)
    (l, r) = widest open range of [xa, xb) ; close any other open columns
    if l < r: push_child(e.adj, l, r, piece_at(childT, l), piece_at(childB, l), P=V0)
```

`cut` and `clip` are in §7.9. The child's start vertex is `P = V0`, the parent edge's
start vertex; its first edge is found by `first_edge_child` (§7.4). An edge pushes at
most one child: a portal is a single column span, and the vertical extent is all
clamping to the bounds.

### 7.9 Clipping lines against the portal bounds

#### 7.9.1 The rule

Every line, over its span `[x0, x1) ⊆ [xa, xb)`, is clamped between the top chain `T`
and the bottom chain `B`:

```
y_eff(c) = clamp(y_L(c), y_T(c), y_B(c))
```

Where `y_L ≤ y_T` the line is hidden above the portal (`ABOVE`); where `y_L ≥ y_B` it
is hidden below (`BELOW`); otherwise `VISIBLE`. Comparisons are inclusive: a line on
a bound is merged, which gives the same pixels with one fewer plot.

By the parity property, plotting `L` at `y_T(c)` is the same as EORing `L`'s colour
into `T`'s piece at that column. So a hidden range is handled by **splitting the bound
piece at the ends of the range and EORing the line's colour into the piece inside
it**. Nothing is plotted for the hidden range. A line hidden below the dummy bottom
bound is merged into an off-screen piece, i.e. dropped.

Worked example, one column through a doorway: parent ceiling `CA` from row 0, top
ceiling line `CA^W`, lintel `W`, bottom ceiling line `W^CB` at row `yb`, child ceiling
`CB`, then the child's far wall top `CB^W2` at row `y2`. If `y2 < yb` the far wall top
is hidden behind the lintel; merging gives the bound at `yb` the colour
`(W^CB)^(CB^W2) = W^W2`, so the fill goes straight from lintel to far wall.

#### 7.9.2 Chain ownership

Before an edge is processed, the parent's chains are split at the edge's span ends
and the parent's cursors move past the span:

```
cut(cur, xa, xb):                       // cur: the frame's Tcur or Bcur
    while cur.x1 ≤ xa: cur = cur.next   // (x ranges are monotone; this is the piece containing xa)
    if cur.x0 < xa: split(cur, xa); cur = cur.next
    head = cur
    p = head
    while p.x1 < xb: p = p.next
    if p.x1 > xb: split(p, xb)
    frame cursor = p.next               // the piece starting at xb (or 0 if xb == xr)
    return head
```

The new piece starting at `xb` is the parent's bound for its next edge, and for any
sibling child portal. Everything left of it, from `head` to the piece ending at `xb`,
is now owned by this edge: the parent walks monotonically rightwards and never
consults these pieces again, so the edge may split, recolour and relink them freely.
Until relinked, the sub-chain's last piece still links onwards to the parent's
continuing pieces; walks over a sub-chain stop at column `xb`, never on a null link.
A soft edge hands the two sub-chains to the child unchanged.

#### 7.9.3 The clip walk

A bound-forming line builds its child chain out of the very pieces the walk touches:
its own visible pieces, and the parent's bound pieces over the ranges where it is
hidden, relinked out of the parent chain. Relinking consumes the parent chain, so
the order of the two bound-forming lines is fixed: the **floor-forming line goes
first**, building the child's bottom chain against the parent's `T` and `B`; the
**ceiling-forming line goes second**, against the parent's `T` and the *child's*
bottom chain (which lies at or above `B` everywhere, so nothing is lost). After that
nothing walks the parent's sub-chains again. The mirror order would work equally
well; what matters is that the second line uses the chain the first one built.

```
clip_edge_line(L, T, Bc, build, role):  // over the visit's columns [xa, xb), left to right
    side = (L's height is below the eye) ? BELOW : ABOVE      // the edge the pre-clip cut it at
    l0 = max(L.x0, xa) ; l1 = min(L.x1, xb)
    if l0 ≥ l1: l0 = l1 = xb                                  // cut off over the whole visit
    if xa < l0: clip([xa, l0), side)                          // cut-off columns: clamped, nothing evaluated
    if l0 < l1: clip(L restricted to [l0, l1), OPEN)          // re-anchored at l0 if L starts left of it
    if l1 < xb: clip([l1, xb), side)

clip(L, T, Bc, build, role, st):        // Bc: the bottom chain to use (B, or childB for the ceiling-forming line)
                                        // st: OPEN to resolve, or the side a cut-off range is known to lie on
    // fast path: the line lies wholly inside the row extents of the two chains, compared
    // unclamped (a line carried past its pre-clip column runs off the screen and fails it)
    if st == OPEN and min(L.y0, y_L(L.x1−1)) ≥ Tmax(T) and max(...) ≤ Bmin(Bc):
        emit(L.x0, L.x1, VISIBLE) ; return
    c  = L.x0
    tp = piece of T containing c ; bp = piece of Bc containing or after c     // split at c if needed
    yL = L.y0 ; yT = y_tp(c) ; yB = y_bp(c)                                   // u8.8
    while c < L.x1:
        if c < bp.x0:                                                 // gap in Bc (closed range, §7.9.4)
            e = min(L.x1, tp.x1, bp.x0)                               // L is ABOVE T throughout such a range
            emit(c, e, ABOVE) ; advance ; continue
        if st != OPEN:                                                // a cut-off range: its own side's chain only
            e = min(L.x1, (st == ABOVE) ? tp.x1 : bp.x1)
            emit(c, e, st) ; advance ; continue
        e   = min(L.x1, tp.x1, bp.x1)                                 // one linear function each over [c, e)
        yL1 = y_L(e−1) ; yT1 = y_tp(e−1) ; yB1 = y_bp(e−1)
        // resolve against the top: D = yL − yT changes sign at most once
        if yL ≤ yT and yL1 ≤ yT1:       ranges = [(c, e, ABOVE)]
        elif yL > yT and yL1 > yT1:     ranges = [(c, e, OPEN)]
        else:
            c1 = crossing(c, e, yL − yT, L.g − tp.g)
            ranges = (yL ≤ yT) ? [(c, c1, ABOVE), (c1, e, OPEN)] : [(c, c1, OPEN), (c1, e, ABOVE)]
        // resolve each OPEN range against the bottom the same way, giving BELOW / VISIBLE
        for (r0, r1, s) in ranges:
            if s != OPEN: emit(r0, r1, s) ; continue
            (evaluate yL, yB at r0 and r1−1; same three-way split with crossing(..., yL − yB, L.g − bp.g))
            emit each sub-range as BELOW or VISIBLE
        advance:  c = e ; yL = yL1 + L.g ; yT = yT1 + tp.g ; yB = yB1 + bp.g     // or re-evaluate at a new piece
                  if c == tp.x1: tp = tp.next
                  if c == bp.x1: bp = bp.next

crossing(c, e, D0, dg):                 // D0: signed difference at c; dg: signed gradient difference
    n = floor(−D0 / dg) + 1             // first column at or after which the sign has changed
    return clamp(c + n, c + 1, e − 1)   // |dg| may be tiny: the reciprocal is normalised and n clamped

emit(r0, r1, status):
    if status == VISIBLE:
        q = extend_or_alloc(r0, r1, y0 = round(y_L(r0)), g = L.g, colour = L.colour)    // round to nearest, as §6's split; §7.10
        if build: append(build, q)
        return
    p = (status == ABOVE) ? tp : bp
    // isolate exactly [r0, r1) within p; the cursor tp/bp follows p
    if p.x0 < r0: split(p, r0) ; p = p.next
    if p.x1 > r1: split(p, r1)
    p.colour ^= L.colour                                  // clamp = recolour
    closed = (role == BOTTOM and status == ABOVE) or (role == TOP and status == BELOW)
    if closed:                                            // §7.9.4
        p.colour ^= CB ^ FB                               // stands in for the child's whole stack
        closed_ranges.add(r0, r1)                         // p stays where it is; the child chain gets a gap here
    elif build:
        append(build, p)                                  // relink: p leaves the parent chain, joins the child's
```

`append` links a piece to the tail of the chain being built (`tail.next = p;
p.next = 0`). Pieces are split at the range ends first, so the parent's chain to the
right of `xb` (§7.9.2) is never disturbed. Nothing is plotted twice, and no piece is
ever in two chains that will both be walked.

Per bound piece the walk costs one evaluation of each of the three lines (the value
at the previous range end is reused or stepped by the gradient) and two compares; a
division only when a crossing is detected. An off-by-one in the crossing column costs
a sliver of at most `|dg|` rows in one column, never a streak, so `n` may be truncated
freely as long as the sub-ranges partition `[c, e)` exactly.

Fast path bookkeeping: `Tmax` is the largest row of any top-chain piece over the
span and `Bmin` the smallest of any bottom-chain piece; both are maintained while
appending (the endpoint rows are already computed during the walk), and the
ceiling-forming line uses `childB`'s `Bmin`. For the root portal `Tmax = 0` and
`Bmin = H`, so every line of the camera cell skips the walk (a reused line carried
past its pre-clip column is the exception: it runs off the screen).

#### 7.9.4 Closed columns

Within one edge the ceiling-forming line is always above the floor-forming line
(§7.8), so the child's bounds can only meet through clamping. A column is **closed**
iff the floor-forming line is `ABOVE` the parent's top bound there, or the
ceiling-forming line is `BELOW` the child's bottom bound there. In a closed column
nothing of the child is visible, and the bound piece the hidden line merged into
would have to serve as both the child's top and bottom bound, which a single `next`
cannot express. So:

**Closed columns are never part of a child span.** `emit` records each closed range
and leaves the piece in the chain it is already in, so the chain being built has a
gap there (which the second bound-forming line's walk steps over, see `clip`). The
child portal is the span `[xa, xb)` less its closed ranges, its chain heads found by
column in `childT` and `childB`; over it both chains are contiguous. A span that is
closed everywhere pushes nothing.

Skipping the recursion in closed columns is only correct with one fix-up. Every line
the child's subtree would have generated there is hidden and would merge into the
same piece, and a complete stack of lines from a cell's ceiling to its floor
telescopes to `ceiling ^ floor`. So `emit` EORs `CB ^ FB` into the piece when it
detects closure; this is exactly what the skipped subtree would have contributed
(checked in §13).

Most spans have no closed columns; a partly closed span is closed at one or both
ends (a step rising into a lintel at the far end of a doorway). Closure can never
split a span in the middle, because every portal opening is **convex** on screen: the
root is the whole screen, and a child's opening is its parent's intersected with the
region between the edge's two bound-forming lines, which are straight (a straight
edge at constant height projects to a straight line; near clipping only clamps them
to the screen edge, `max(line, 0)` or `min(line, H)`). The child's chains are exactly
that intersection, top `max(C, T)` and bottom `min(F, B)`, so by induction every
opening is convex, and a convex region's columns are one interval. (A top bound is a
maximum of lines in rows, so it can only bow upward on screen; the "V" a middle
closure would need is the opposite curvature.)

Rounding can still disagree on a nearly shut opening: the chains are within about half
a row of the true lines, so an opening under a row or two tall can read as closed in
its middle columns. The opening's height is concave along x (largest in the middle),
so a pinch in the middle means the whole opening is under about two rows. The portal
is then the widest open range, and any other open columns are closed after the fact
with the same fix-up (`CB ^ FB` EORed into the bottom chain's pieces over exactly
those columns): at most a two-row sliver shows flat colour, which the parity property
absorbs. (Neither case has occurred in the demo; the fix-up path was exercised by
force.)

### 7.10 Line extension across visits ("active lines")

A line's visible range is emitted in several pieces: once per clip block within a
visit (the block boundaries come from the bound chains' pieces, not from the line),
and again on each visit of a cell seen several times (a wall split by soft-edge
decomposition, or seen through adjacent portals). Because visits occur in increasing
x order, a range that begins exactly where the same line's last visible piece ended
is that line continuing, and the piece simply grows: with the gradient already known,
extension is just a new `x1`. The visit record keeps, for the edge being walked:

```
active[4]       last emitted VISIBLE piece of each of the edge's ≤ 4 lines (0 if none)
```

Each line of an edge has a fixed slot, the same on every visit: the line at the
cell's own ceiling (a wall's top, or a portal's top ceiling), the child's top bound,
the child's bottom bound, and the line at the cell's own floor. The slots are cleared
when the cursor moves to a new edge; a visit that resumes on the previous visit's last
edge keeps them. A contiguous visit extends them across the seam; after a gap
(foreground geometry in between) the `x1 == r0` test fails and new pieces start.

**Lines are generated once per edge.** An active piece is also its line: a piece's
`x0`, `y0` and `g` never change, and a line runs on to the edge's end
column, which the end vertex gives without clipping. So a later visit resuming on the
edge uses `(piece.x0, end column, piece.y0, piece.g)` as the line, and `clamp_line`
starts it at the new span's left column: no side clip, projection or gradient
division. The piece's start row is rounded (§6), so a restart past an occluder can sit
a row off the regenerated line; that is accepted. Lines pre-clipped at the screen edge
(§7.7) need no exception. The reused line carries on past the column where it left the
screen, with rows off the screen and beyond every bound (the row arithmetic saturates
at the bottom and clamps at the top, so it keeps comparing as outside); the clip finds
that and clamps it, recolouring the same bound pieces the cut-off columns would have.
This needs the fast path's rows unclamped: clamped to `0..H`, a line running off the
screen would pass the root's extents and be kept whole. The cut-off columns themselves
never produce a visible piece, so a line cut off at its start leaves no active piece
there, and a later visit regenerates it. Only a slot with no active piece (0: the line
not yet generated on this edge, or wholly hidden so far) is projected, and only then
are the edge's endpoints side-clipped, once for all its lines.

```
emit_visible(r0, r1, L):
    q = V.active[slot(L)]
    if q != 0 and q.x1 == r0 and q.g == L.g and q.colour == L.colour
       and (no chain is being built, or it is empty, or q is its tail):
        q.x1 = r1                    // the same line continuing: extend, keeping q's anchor
        if building: widen the chain's extents over [r0, r1), appending q if it is not the tail
        return
    q = alloc(r0, r1, round(y_L(r0)), L.g, L.colour) ; V.active[slot(L)] = q
    if building: append q
```

The gradient and colour checks catch what the child walks between visits can do to
a bound-forming line's piece: a split shortens it (`x1` no longer matches), and a
merge recolours it; either way a fresh piece starts. A piece carried over from an
earlier visit ends at this visit's first column, so it can only open the chain being
built, never follow its tail; it then starts left of the span, which chain walks
tolerate (the child's first cut splits it at the child's left column). Such a piece
was in a previous child's chain at most, and that child was popped before this visit
started, so re-linking it is safe: pieces only ever move down into child chains,
never up into a live ancestor's. A pillar between two visits leaves a gap, so `x1`
does not match and the later visit gets fresh pieces.

Extension keeps the piece's original anchor instead of re-rounding at every seam.
That is more exact than splitting: each re-anchor drops up to half a row of
fraction, and on a shallow line the drops add up to a whole row, so a split wall edge
could meet the next wall a row off where the extended piece joins it exactly.

## 8. Rendering

### 8.1 Plotting lines

Iterate the pool; skip pieces with `colour == 0`, and pieces at `y0 = H` whose
gradient is not negative: they are wholly below the screen (one that rises comes on
screen where its row drops under H − ½). The plotted row is the **nearest** row,
`(y + 0x80) >> 8` — the same convention as every re-anchor (§6 split, §7.9 emit), so
a piece split at a cut plots exactly the rows the unsplit line would have and a chain
seam cannot notch the boundary.

Nothing is bounds checked. Every piece starts in `0..H`, but the rounding accumulated
along a line can carry it a row past the top or bottom (row 65 at worst over the
sweep, nothing above row 0). The back buffer therefore has a junk character row either
side (§8.3), canvas rows `H..H+3` below and `−4..−1` above (the row byte wraps to
`0xFC..0xFF`, so the row-to-address tables simply cover those values). Toggles there
are never displayed or filled; the fill starts at row 0 as always. Branch on direction
(falling or rising) and on `|g| < 1` or not:

- Steep lines (`|g| ≥ 1`): one pixel pair per column, `y ± g` in u8.8.
- Shallow lines (`|g| < 1`, the gradient one byte): the row moves at most one per
  column, so the row is a byte and a fraction, and the carry (borrow, rising) out of
  `fraction ± g` says whether the next column is a row on. A whole byte of the line
  takes three steps inside the byte, and branches on each carry in turn (the step
  after the leftmost pixel first) into one of eight fixed routines, each plotting the
  byte's rows with its masks as constants:

  ```
  000 FF             100 88 77
  001 EE 11          101 88 66 11
  010 CC 33          110 88 44 33
  011 CC 22 11       111 88 44 22 11
  ```

  Each mask row is EORed as `pattern AND mask` into the row's two scanlines (`FF`
  needs no `AND`), moving a row (down falling, up rising) between masks; a fourth
  step's carry moves the row on for the next byte. On the 6502 the routine takes that
  fourth step first and carries it through the plotting in C, and each step's add
  uses `g` or `g − 1` as the known carry-in requires, so the loop has no `CLC`. The
  partial bytes at either end of the piece go a pixel at a time with the same step.
  The pixels are exactly the steep loop's: the carry out of the fraction byte is the
  u8.8 row's increment. Falling and rising lines are separate branches (add and carry
  down, subtract and borrow up).
- Flat lines (`g = 0`), a third branch: no steps at all, whole bytes on the one row
  with mask `FF`, the end bytes masked to the piece's pixels (`FF 77 33 11` from the
  first pixel's position in its byte, `88 CC EE FF` up to the last's; both at once
  when the piece lies within one byte). Through the tree a flat byte would still take
  its four steps (77 cycles against about 48), and its end bytes would go a pixel at a
  time; flat lines are the commonest kind, so the branch saves over 1,000 cycles a
  frame.

Each plot writes rows `y` and `y+1` with `E` on the even row and `O` on the odd row.

The screen is addressed as the 6502 does, `EOR (blk),Y : STA (blk),Y`: `blk` in zero
page holds a character block's address (256 bytes per character row, 8 per byte
column) and `Y` the canvas row's even scanline within the block, `0, 2, 4, 6`, its odd
partner at `Y + 1`. A piece places the cursor once, at its first pixel (row-indexed
tables give the character row's address and `Y`; the byte column adds `8·bc`), and
then only moves it:

- a row down: `Y += 2`, and on reaching 8, `Y = 0` and `blk += 256`;
- a row up: at `Y = 0`, `Y = 8` and `blk −= 256`; then `Y −= 2`;
- a byte right: `blk += 8`;
- a pixel right: the mask shifts right (`LSR`), and past the byte's last pixel it
  resets to `88` with `blk += 8`.

A steep line moves its whole rows per column one at a time, as a shallow one does.

### 8.2 Fill

Two passes (even rows, odd rows), column-major, unrolled `EOR row,X : STA row,X`,
with X the byte column's block so no operand crosses a page. 9 cycles a byte:
37.0k cycles (18.5 ms) for 4K, measured on the Beeb. The two passes do not change
the total.

### 8.3 Buffers and clearing

In place: plot into the back buffer, fill it in place, and clear it before the next
frame's plot. There are two buffers, A at &5800 and B at &7000. Both start on a
multiple of &800, so the CRTC's start address (the address DIV 8) has a low byte of
0: a swap only writes R12. Each buffer has a 256-byte junk character row above and
below it for plot overruns (§8.1), which are never cleared, filled or displayed:
A's are &5700 and &6800, B's above is &6F00, and B's below is the paged ROM at
&8000, where stores go nowhere. Only the plotters write the RAM junk rows, so
outside the plot they're 768 bytes of scratch; &6900-&6EFF between them is never
written at all, 1.5K of ordinary RAM.

Each buffer has its own clear and fill bodies, with its addresses as operands, so
neither needs an index register or a pointer to say which buffer. The clear stores
zero to the buffer's 16 pages, 256 times round: 21.8k cycles (11 ms). The fill is
§8.2's. The four bodies take 1.7K.

A separate 4K line buffer was considered: plot into it, fill from it into the back
buffer, and clear it by re-plotting the frame's pieces with EOR, which would save
much of the clear. It is ruled out on memory: it needs about 4.5K with its junk rows,
and the Beeb's whole headroom is about 3.2K (beeb/PLAN.md §2.4).

## 9. Scaled sprites

Sprites are drawn into the toggle image between line plotting and the fill, as
change points, so that the fill does the work of colouring them. The cost is then
proportional to the number of colour changes and crossing lines, not to the sprite's
area, and overlapping sprites and occlusion by nearer geometry come out right.

### 9.1 Art format

A sprite is stored column-wise as change points, top-down, over a per-sprite palette
of seven catalogue shades (§5) plus transparent. The runtime format is the Baron
sprite demo's, read top-down:

```
byte 0          width w in columns (up to 32)
byte 1          height h in rows (up to 32)
bytes 2..w+1    each column's offset from the art's start (the art stays under 256 bytes)
per column      one byte per run, top to bottom: row·8 + code
0               after the last column
```

`row` (5 bits) is where the run starts, 0 for the column's first, and `code` (3 bits)
its colour: 0 transparent, 1..7 the palette's shades, whose `(E, O)` byte pairs form
`pal[]`. A run ends where the next begins; a column's runs end at a byte whose row is
0 (the next column's first run, or the final 0), and its last run at row `h`.
Consecutive opaque runs form a **silhouette run** with internal colour changes, and
transparent gaps separate silhouette runs. The encoder (the assembler's job on the
6502, startup in the C) emits a byte at every row where the colour changes.

### 9.2 Instances

A sprite **instance** is a cell's sprite as projected for one frame, on the cell's
first visit. Its base centre is transformed like a vertex; nearer than
`PT_SPRITE_NEAR` (32 units; behind included) it is skipped, popping out rather than
filling the screen. Otherwise, from the depth `x`:

```
scale  = 64·size / (h·x)                 u8.8 screen pixels per source pixel (size: the world height)
centre = 64 ± 64·|y|/x                   the base's column, u8.8
bottom = H/2 ± 64·8·|floor − eye|/x      the base's row, u8.8
left   = centre − scale·w/2              the top-left corner, u8.8; 24 bits, as it can lie
top    = bottom − scale·h                well off the screen
[cx0, cx1) = [round(left), round(left + scale·w))   clamped to the screen
```

`h·x` is one 16×8 product, and each division is the reciprocal projection of §4
(`pt_project_q8`). The depth's reciprocal is formed here, not taken from the vertex
transform, which forms one only inside the view cone: a sprite can be on screen with
its centre outside it. A pixel belongs to the sprite when its centre lies inside it, so a
source column's screen columns run from the rounded position of its left edge to that
of its right, and likewise rows; the sprite pass steps these edges from the corner by
`scale`, with no per-column division. An instance with no columns, or wholly above
or below the screen, is not kept. Record: sprite, `cx0, cx1`, segment list head and
tail, draw order links, scale, corner (~16 bytes).

### 9.3 What capture records: clip segments

A visit of cell X through some portal has a span `[l, r)` and, at the moment it is
pushed, complete chains `T` and `B` over that span (§7.8 step 5). Within the span an
instance is visible exactly between `T(c)` and `B(c)`; outside it, it is hidden by
whatever bounds the portal. Those chains already incorporate every nearer portal,
so they are the whole occlusion answer — but they are only walkable at push time,
because the visit's own edges cut them and children relink their pieces (§7.9.2).
What survives is the pieces themselves: a piece's `x0`, `y0` and `g` never change
after allocation (splits shrink `x1` and allocate a new piece; relinking changes
`next`; merges change colour), and the pool lives until the frame is drawn. So a
visit records *which pieces* bound each instance, and the sprite pass evaluates them.

```
on entering a portal of cell X, span [l, r), chains T, B:   (root: rootT, rootB)
    if X's instance exists and [cx0, cx1) ∩ [l, r) is non-empty:
        seg = alloc segment over the overlap [s0, s1)
        for each piece of T covering a column in [s0, s1), in x order: append its index to seg.T
        for each piece of B likewise: append to seg.B
        append seg to the instance's segment list
        push the instance on the pending stack                (§9.4)
```

A portal is entered at `walk`'s start, or at its tail call to the last edge's child.

A **segment** is `{x0, x1, next, nT, nB, T[nT], B[nB]}`: 5 bytes plus one per bound
piece, typically 7–10 bytes, allocated from a per-frame byte pool of 256 (offsets are
a byte, 0 the null). Consecutive
recorded pieces meet at the later one's `x0`, and the last runs to `seg.x1`, so no
per-piece extent is stored. Visits occur in increasing x, so an instance's segments
are appended in x order and never merged; a cell seen through two doorways gives its
sprite two segments whose bounds meet at the boundary column. Columns of the
instance covered by no segment are hidden.

At draw time each live segment carries a DDA cursor per chain: the current piece
and its row in u8.8, initialised with one multiply `y0 + (s0 − x0)·g` at the
segment's first column, stepped by `g` per column, and switched to the next recorded
piece when `c` reaches its `x0`. This is the plotter's own evaluation, so the clip
rows are the plotted bound rows exactly, which §9.5 relies on.

### 9.4 Order

Along any column the depth order is: camera-cell sprites, the first portal's bound
lines, the child's sprites, the child's walls, the grandchild's sprites, and so on.
An instance must be drawn after every sprite in every visit *descended from* the
visits of its cell, and before those of its cell's ancestors. Since a descendant
visit always pops before its ancestor, and visits that are neither share no columns,
issuing instances at **pop** time gives that order with no sorting:

```
on pop of a visit of cell X:
    if X's instance received a segment from this visit:
        if the instance is already in the order list: unlink it
        append it to the tail of the order list
```

A visit pops when `walk` returns, but a call walks a chain of portals, each the tail
call of the one before, and they all pop at that return, the innermost first (the
tail child is its parent's last, so its pop comes just before its parent's). So each
entry pushes its instance on a **pending stack** (§9.3), and the call's return issues
the entries above the height it began with, top first. Children walked by nested calls
have issued theirs by then. (On the 6502 the pending stack can be the hardware stack.)

An instance is therefore issued once, at the pop of the *last* visit of its cell
that overlaps it. That is after the descendants of all its visits (farther) and
before its ancestors (nearer). With one sprite per cell there is no order within a
cell to keep. The order list is doubly linked through the instance records.

### 9.5 The sprite pass

For each byte column that any instance covers, read the plotted toggle image top to
bottom once, into one sparse list of the non-zero rows, each entry holding the whole
byte pair exactly as the plotter wrote it:

```
build_list(bc):                               ; byte column bc = pixel columns 4bc..4bc+3
    n = 0
    for row in 0..ymax:                       ; ymax = lowest sprite row in this byte column
        e = screen[2*row][bc] ; o = screen[2*row+1][bc]
        if (e | o) != 0: append (row, e, o)   ; ~11 cycles per zero row, ~13 per non-zero
```

The list is an exact model of the byte column: for any pixel, the fill value at a
row is the EOR of the entries at or above it, masked to that pixel. At most one entry
exists per row. Then, for each instance in order-list order (§9.4) whose column range
covers the byte column, and for each of its pixel columns `px` in it: advance the
instance's segment cursor to `c` (segments are in x order; the DDA cursors of §9.3
give `top = T(c)` and `bot = B(c)`); if a segment covers `c` and `top < bot`, each
silhouette run of the sprite column at `c`, scaled and clipped to `[top, bot)`, is
one `paint`. Different
pixels' bits are independent, so interleaving pixels across instances is harmless;
what matters is that each pixel sees the instances far to near.

```
paint(bc, px, ya, changes, yb):               ; changes = (row, colour) after the first, in row order
    m = pixmask[px]                           ; the two bits of this pixel, per scanline byte
    acc = 0 ; i = 0
    while i < n and list[i].row <= ya:        ; prefix, whole bytes: mask only when used
        acc ^= list[i] ; i++
    S = first colour
    tog = (acc ^ pal[S]) & m
    put(ya, tog) ; plot(bc, ya, tog)
    for (y, S') in changes, then (yb, none):
        last = (S' is none) ? y - 1 : y       ; a change row's own world toggle is covered too;
        while i < n and list[i].row <= last:  ; the one at yb is below the sprite, and stays
            bits = list[i] & m
            if bits != 0:                     ; a toggle this pixel covers: erase it
                acc ^= bits
                plot(bc, list[i].row, bits)   ; annuls the plotter's pixel
                list[i] &= ~m                 ; other pixels keep their bits
                if list[i] == 0: remove(i) ; continue
            i++
        tog = ((S' is none) ? (pal[S] ^ acc) : (pal[S] ^ pal[S'])) & m
        put(y, tog) ; plot(bc, y, tog)
        S = S'

put(y, tog):                                  ; keeps the one-entry-per-row invariant
    if an entry at row y exists: entry ^= tog ; remove it if it becomes 0
    else insert (y, tog) at position i, sorted ; i++

plot(bc, row, bits):                          ; bits = (E, O), already masked to the pixel
    EOR E into scanline 2*row, O into scanline 2*row+1, of byte column bc
```

`acc` is always the world value at the current row *before this sprite*: the prefix
at `ya`, then each erased toggle folded in, so the closing toggle at `yb` returns the
pixel to what lies below. A world toggle exactly on an internal change row is erased
like any other the run covers; leaving it (erasing only above the change) would leave
it live under the sprite and out of `acc`, wrong below it and at `yb` alike, and a
world line lands on a change row often enough that the reference comparison (§13)
failed in most frames with a sprite until this was fixed. Insertions never touch `acc`, and masking distributes over
EOR, so `acc` can stay a whole byte pair and be masked only where a toggle is
written. After `paint` the list is exact again, so a nearer sprite sees this one as
background, erases its toggles where it covers them (including its start or end
toggle if a run straddles it), and needs nothing else. A flat sprite edge across
several pixels merges into one entry through `put`.

Erasing works because EOR commutes: the line plotter has already written the pixel,
and writing the same bits again removes it. A wall line passing behind a sprite
therefore costs one extra EOR per covered column.

Clipping to the segment's bounds is consistent with the list: a run clipped to start
at `top` includes the bound's own toggle in `acc` (entries at `row ≤ ya` are folded
in), and a run clipped to end at `bot` leaves the bound's toggle in place (entries at
`row == yb` are kept).

In the implementation (`sprite_pass`, run between plotting and the fill):

- The byte column's scan stops at the lowest row any of its sprites reaches, and
  never reads the junk rows: bounds clamp to `0..H`, and a closing toggle at row `H`
  is simply not written, as nothing below the screen is shown.
- Each instance keeps the screen row of every source row edge, rounded and clamped to
  `0..H` (`h + 1` bytes, from the corner stepped by `scale`), and steps the source
  column along its screen columns with the same rounding, so no division is left in
  the pass.
- Its bound cursors are the segment's pieces and their biased u8.8 rows, stepped by
  the pieces' gradients exactly as `row_step` would for one column, with one
  evaluation on entering a segment or a piece.
- A silhouette run is the consecutive opaque runs of a source column, ending at a
  transparent run or the column's end; its first colour is the last run starting at
  or above the clipped `ya`.

### 9.6 Cost and memory

Per covered byte column: ~11 cycles per scanned row (so at most ~700), plus per
opaque run in each pixel column a short list walk (a few cycles per entry, including
entries that belong to other pixels), two pixel plots at ~35 each, one plot per
erased toggle, and ~30 per internal colour change. This is independent of sprite
height. Direct masked plotting over the filled screen costs ~26 cycles per pixel per
row (two read-modify-writes), i.e. ~104 per row per fully covered byte column, so
the two break even at about seven rows of sprite height; a 16-row sprite is ~1.5×
cheaper this way, a 32-row one ~3×, a full-height one ~6×.

Scratch, reused per byte column: one list of (row, E, O), 32 × 3 = 96 bytes; the cap
bounds how many distinct toggle rows one byte column may hold (world lines plus every
opaque run edge of every overlapping sprite). The sweep peaks at 20; exceeding it is a
panic for now.
Per frame: instance records (~18 bytes each including DDA cursors), clip segments
(5 bytes + 1 per bound piece, typically 7–10), `pal[8]` per sprite (16 bytes, entry 0
unused), `pixmask[4]`, and the scale tables. Nothing is added to the piece record; the pool must stay
intact until the sprite pass has run, which it does anyway. Evaluating the bounds
costs ~10 cycles per column per chain, the same DDA the plotter uses.

Sprites straddling a portal edge are assigned to a cell by centre and accepted as an
artefact. Interpenetrating sprites sort by centre depth.

## 10. Camera movement and collision

The camera is a circle of radius `r` centred at `P` (s16.8), and is always assigned
to one cell, the **current cell** `C`: the one containing `P`. The current cell
decides the root of the walk, the eye height (`z_eye = floor_z(C) − eye_height`) and
which edges are tested for collision.

### 10.1 Per-edge data

The editor precomputes for each edge, from its vertices `V0 → V1`:

```
e   = unit direction (V1 − V0) / |V1 − V0|         s0.8, s0.8
n   = inward unit normal = (−e.y, e.x)              s0.8, s0.8   (interior is on the right of a clockwise edge)
d0  = dot(V0, n)                                    s16
len = |V1 − V0|                                     u16 (or u8 if cells are small)
```

so that for any point `Q`:

```
dist(Q, edge) = dot(Q, n) − d0      // signed; > 0 inside the cell's half-plane
t(Q, edge)    = dot(Q − V0, e)      // position of the foot of the perpendicular along the edge, 0..len on the segment
```

### 10.2 Invariant and the two kinds of test

The invariant is: `dist(P, w) ≥ r` for every solid wall `w` of the current cell.
Every wall — the current cell's and a neighbour's — gets the **capped test**: a hit
only if `dist(Q, w) < r` and `0 ≤ t(Q, w) ≤ len` (the foot lies on the segment). A
neighbour's wall additionally needs `dist(Q, w) ≥ 0` (the centre on its inner side),
since its line extended passes through the current cell where it is not a wall. An
infinite-line test for the current cell's walls would seal a doorway collinear with
one of them (a T-junction split), and any flat extension of the cap wedges movement
aimed at the wall's end, because such a facet's slide direction is parallel to the
wall.

The segment ends are covered instead by treating every candidate wall **endpoint as
a point obstacle**. With `v = P − V` the offset from the endpoint (whole units are
enough), when the circle overlaps it (`v.x² + v.y² < r²`, squares from the
quarter-square table) and the move approaches it (`dot(M, v) < 0`), the move loses
its radial component:

```
M −= v · dot(M, v) / dot(v, v)        // the division via the reciprocal table
```

slightly overcorrected (say by a quarter) so a grazing arc drifts back out instead
of grinding inward chord by chord. What remains is tangent to the circle round the
corner, so the camera deflects past a doorway jamb and slides on instead of
wedging. In the corner regions the r-invariant softens to "never closer than
before".

A neighbour is tested at all only while the circle overlaps its portal:
`dist(P, portal) < r`. Any edge with a non-zero adjacent index can be crossed; solid
walls (adjacent index 0) collide. (A floor step too high to climb can be treated as
solid in this step by comparing floor heights when the candidate list is built; not
required.)

### 10.3 Per frame

```
move(M):                                       // M = desired displacement this frame, s16.8
    // 0. candidates: the current cell's solid walls (flagged OWN), those of every
    // neighbour the circle overlaps (flagged CAPPED), and the endpoints of all of them
    cand = solid walls of C, flagged OWN
    for each portal edge p of C with dist(P, p) < r:
        add solid walls of cell[adj(p)], flagged CAPPED
    verts = the deduplicated endpoints of every wall in cand

    // 1. resolve: restore the invariant if something (a slide, a transition) broke
    // it; neighbour walls count too, or a corner shared with the next cell wedges
    // the slide before its own resolve can see it
    for w in cand:
        d = dist(P, w)
        if t(P, w) < 0 or t(P, w) > len_w: continue
        if w is CAPPED and d < 0: continue
        if d < r: P += n_w · (r − d)

    // 2. move with sliding: a wall counts only when the move brings the centre
    // closer than it already is, so a tangent graze never wedges
    for iter in 1..3:
        Q = P + M
        hit = none ; worst = r
        for w in cand:
            d = dist(Q, w)
            if t(Q, w) < 0 or t(Q, w) > len_w: continue
            if w is CAPPED and d < 0: continue
            if d + eps ≥ dist(P, w): continue          // not approaching
            if d < worst: worst = d ; hit = w
        if hit != none:
            M = dot(M, e_hit) · e_hit                  // slide along the wall
            continue
        deflect M at each overlapped, approached vertex of verts (§10.2)
        if nothing deflected: break
    if the last iteration still saw a wall hit: M = 0   // wedged in a pocket: stay put
    P = P + M

    // 3. transition: the centre has crossed a portal line
    repeat:
        for each portal edge p of C:
            if dist(P, p) < 0: C = adj(p) ; continue repeat     // at most a couple of steps near a vertex
        break
    z_eye = floor_z(C) − eye_height              // optionally eased over a few frames on a step
```

Sliding along a wall keeps `dist` to that wall unchanged, so it never breaks the
invariant; it may create a hit against another candidate, which the next iteration
handles, and three iterations are enough for any corner. The resolve step exists
because a transition can land the centre inside the new cell at less than `r` from a
wall whose foot was off-segment (allowed by the capped test, disallowed by the line
test the new current cell will apply); it nudges the centre out along the normal
rather than leaving the mover wedged.

The centre can only cross a portal *line* within the portal *segment*, because the
rest of that line lies outside the convex current cell and the walls keep the centre
inside, so the transition test needs no segment check. Keeping the centre at least `r`
inside its cell also guarantees no vertex coincides with the camera and no cell edge
passes through it, which the walk assumes.

### 10.4 Cost

Per frame: two dot products per candidate wall per iteration (one for `CAPPED` walls
that pass the distance test), with s16.8 × s0.8 products. A cell of 4–8 edges with
one overlapped neighbour gives 8–16 candidates, so a few hundred cycles in the
common case and under a thousand when sliding round a corner.

## 11. Editor requirements

- Convex cells, clockwise rings, shared vertex table, twin edges with T-junctions
  split, twin edge indices stored per edge.
- Cells sized so view coordinates fit s16.
- Soft edges emitted by the convex decomposition, with identical attributes on both
  sides.
- Per-edge collision data: unit direction `e`, inward normal `n`, offset `d0`, length (§10.1).
- Sprite art in the column-wise change-point format of §9.1, plus scale tables.
- Optionally: per-edge precomputed colour pairs (`CA^W` etc.) and a "which of the
  four lines this edge needs" mask.

## 12. Costs (estimates)

| Item                          | Cost                                                    |
|-------------------------------|---------------------------------------------------------|
| Fill                          | ~18 ms per 4K frame                                     |
| Clear                         | ~10 ms in place                                          |
| Vertex transform              | 4 table multiplies + 2 adds per vertex visit             |
| Vertex projection             | 1 reciprocal + 1 multiply per vertex in the frustum      |
| Line endpoints                | 1 multiply per endpoint per line (`h/x`)                 |
| Near clip                     | 1 division + 2 multiplies, only when triggered            |
| Bound clipping                | ~1 multiply per bound piece crossed, 1 division per crossing; skipped entirely by the fast path |
| Sprites                       | ~700 per covered byte column + ~100 per opaque run per pixel column (§9.6) |

## 13. Verification done so far

The edge-finding rules (§7.4, §7.5), the classification with mirrored projection for
vertices behind the camera (§7.3), the side-clip formulas (§7.6) and the (then
`x = 2|h|`, H = 64) near-clip threshold (§7.7) were checked against exact ray
casting on 60,000 random
convex cells in double precision, including ~10,000 vertices behind the camera and
cases where a single edge is clipped at both frustum rays. No failures.

The closed-column telescoping argument (§7.9.4) was checked by hand on the
lintel-over-step case: parent lines `CA^W`, `W^CB`, `FB^W` all above the parent's top
bound `T` merge to `CA^CB^FB^W`; the child's stack contributes `CB^FB`; the fill below
`T` is then `CA ^ (CA^CB^FB^W) ^ (CB^FB) = W`, the step face, as it should be.

The sprite pass (§9.5) is checked against a direct reference (`test_sprite.c`): the
same frame rendered without the pass and filled, then each instance's opaque pixels
painted straight over it, far to near, inside the bounds its segments recorded
(evaluated exactly), each source pixel covering the screen pixels whose centres it
covers. Over every cell's centroid at 64 angles and a 4000-tick random walk, with the
boulder alone and with six more sprites overlapping it, 4688 frames (891 drawing
sprites) match pixel for pixel. The capture's bookkeeping (§9.3, §9.4) is checked
separately: segments and their pieces in x order, and, for 2045 columns two sprites
share, the one whose cell the column's centre ray enters later drawn first.

## 14. Open questions

1. Fixed-point details in §4.1 to be confirmed against the existing multiply and
   reciprocal routines, in particular the fractional-`dx` gradient in the near clip
   and the crossing quotient.
2. Line pool size, recursion depth cap, and the size of the per-edge closed-range
   list (two entries is almost certainly enough).
3. Whether `Tmax`/`Bmin` should be per portal (as specified) or dropped in favour of
   always walking; the root-portal case alone probably justifies keeping them.
4. Sprite details: the column-list cap, the horizontal step and vertical scale table
   set (which scales are supported), and whether the scan of a byte column can be
   shared when several byte columns are covered by the same instances.
