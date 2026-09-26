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
- **Reciprocals**: `0x10000/n` lo/hi, 256 entries. Larger `n` is normalised by
  counting shifts into range and shifting the result down afterwards. Entries 0 and 1
  are `0xFFFF`.
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
| `sy`, `y0`                     | u8                   | `0..H−1` plotted; `H` marks an off-screen piece        |
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
  the fraction in the same direction every time, and chained splits (a bound cut once
  per child portal) compound that drift by up to a row each — enough to carry a bound
  past a line it should hide, opening a wrongly coloured sliver.
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
    push(cell=camera_cell, xl=0, xr=128, T=rootT, B=rootB, start=first_edge_camera())
    while stack not empty:
        walk(top)                  // may push; a pushed child runs before the parent resumes
                                   // push records sprite clip pieces (§9.3), pop issues sprites (§9.4)
    plot every pool piece with colour != 0 and y0 < H
    sprite_pass()                  // §9.5: reads the plotted toggles, adds sprite toggles and cancels
    fill (even pass, odd pass)
```

Recursion is an explicit stack of portal frames rather than JSR recursion:

```
frame:
    cell         cell index
    xl, xr       column span
    Tcur, Bcur   current piece of the top / bottom chain (the piece containing the next column)
    Tmax, Bmin   conservative row extents of the chains over [xl, xr)   (fast path, §7.9)
    n            index of the edge being walked
    V0           view data (x, y, a, b) of vertex n, carried across edges
```

Rendering is a depth-first, left-to-right sweep: every visit to any cell during a
frame happens in increasing x order, which is what makes line extension (§7.10) valid
and makes chain ownership (§7.9) safe.

### 7.2 Vertex transforms

No per-frame vertex cache. Each vertex is transformed when the walk reaches it, and
the walk carries the current vertex's view data from one edge to the next. When a
child is pushed, the parent's edge start vertex `P` is the child's start vertex, so
its view data is passed down. A cell visited twice through different portals
re-transforms its vertices; the arithmetic is deterministic, so both visits get
identical numbers, which §7.10 relies on.

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

### 7.5 The walk, the last edge, zero-width spans, termination

```
walk(f):
    loop:
        V1 = transform(v[f.n + 1])
        process_edge(f, f.n, f.V0, V1)         // §7.6–7.9; may push a child and return
        if cmp(V1, f.xr) ≥ 0: pop(); return    // end vertex at or right of the right column
        f.V0 = V1; f.n = f.n + 1
```

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
  portal chain; skipping zero-width spans removes the rounding cases. A depth cap and
  the pool size are the safety net.

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

`make_line` produces one or two pieces from the side-clipped endpoints `E0`
(left, columns from `E0.sx`) and `E1` (right), for a height `h` and colour `col`:

```
make_line(E0, E1, h, col):
    d     = 128·|h|/H, rounded up
    edge  = (h < 0) ? 0 : H                          // the screen edge this line can leave by
    near0 = E0.x < d ; near1 = E1.x < d
    if near0 and near1:
        return [ flat(E0.sx, E1.sx, edge, col) ]     // y0 = 0 → merges into the top bound; y0 = H → off screen
    if not near0 and not near1:
        sy0 = H/2 + 64·h/E0.x ; sy1 = H/2 + 64·h/E1.x
        return [ line(E0.sx, E1.sx, sy0, (sy1 − sy0)/(E1.sx − E0.sx), col) ]
    // exactly one endpoint is too close: clip the edge at depth d
    (N, F) = near0 ? (E0, E1) : (E1, E0)             // near and far endpoints
    t   = (d − N.x) / (F.x − N.x)                    // u0.8
    y*  = N.y + t·(F.y − N.y)                        // lateral view coordinate at depth d
    sx* = 64 + 64·y*/d                               // u8.8, keep the fraction; 1/d cached per (cell, ceiling|floor) per frame
    syF = H/2 + 64·h/F.x
    if near0:                                        // near end on the left
        c*  = ceil(sx*)
        g   = (syF − edge) / (E1.sx − sx*)           // fractional dx: same reciprocal routine, extra shift
        y0  = edge + (c* − sx*)·g,  rounded to nearest and clamped to 0..H
        return [ flat(E0.sx, c*, edge, col), line(c*, E1.sx, y0, g, col) ]
    else:                                            // near end on the right
        c*  = ceil(sx*)
        g   = (edge − sy0) / (sx* − E0.sx)
        return [ line(E0.sx, c*, sy0, g, col), flat(c*, E1.sx, edge, col) ]
```

Empty pieces (`x0 ≥ x1`) are dropped. The lateral coordinate `N.y` of a side-clipped
endpoint is `∓N.x` (on the ray); of an in-frustum endpoint it is the transformed `y`.

Starting the sloped piece at `ceil(sx*)` keeps its `y0` inside the screen and exact
at that column; the column `floor(sx*)` goes to the flat piece, at most `|g|` rows
off, in one column. After this step every `y0` is in `0..H` and every gradient of
a piece wider than one column fits sign + u6.8.

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
    // 1. side clip and column span
    E0 = (a(V0) < 0) ? clip_left(V0, V1)  : (x=V0.x, y=V0.y, sx=column(V0))
    E1 = (b(V1) < 0) ? clip_right(V0, V1) : (x=V1.x, y=V1.y, sx=column(V1))
    xa = max(E0.sx, f.xl) ; xb = min(E1.sx, f.xr)
    if xa ≥ xb: return                                  // zero width
    // 2. take ownership of the bound sub-chains over [xa, xb)   (§7.9)
    T = cut(f.Tcur, xa, xb) ; B = cut(f.Bcur, xa, xb)
    e = edge[n] ; A = f.cell
    if e.soft:
        push_child(e.adj, xa, xb, T, B, P=V0) ; return
    if e.adj == 0:
        for L in make_line(E0, E1, cA − eye, CA^W) : clip(L, T, B, none)
        for L in make_line(E0, E1, fA − eye, W^FA) : clip(L, T, B, none)
        return
    Bc = cell[e.adj]
    // 3. non-bound-forming lines first: they only recolour, and need T and B intact
    if cB > cA: for L in make_line(E0, E1, cA − eye, CA^W) : clip(L, T, B, none)
    if fB < fA: for L in make_line(E0, E1, fA − eye, W^FA) : clip(L, T, B, none)
    // 4. bound-forming lines last, floor first (§7.9.3):
    //    F builds childB against T and B; C builds childT against T and childB
    F = (fB < fA) ? make_line(E0, E1, fB − eye, FB^W) : make_line(E0, E1, fA − eye, FB^FA)
    C = (cB > cA) ? make_line(E0, E1, cB − eye, W^CB) : make_line(E0, E1, cA − eye, CA^CB)
    closed = [] ; childB = chain() ; childT = chain()
    for L in F: clip(L, T, B,      build=childB, role=BOTTOM)
    for L in C: clip(L, T, childB, build=childT, role=TOP)
    // 5. one child portal per open sub-range of [xa, xb)   (§7.9.4)
    for (l, r) in open_ranges(xa, xb, closed):
        push_child(e.adj, l, r, piece_at(childT, l), piece_at(childB, l), P=V0)
```

`cut` and `clip` are in §7.9. The child's start vertex is `P = V0`, the parent edge's
start vertex; its first edge is found by `first_edge_child` (§7.4). Where the span is
split by closed columns the children are pushed right to left so that the leftmost
runs first, keeping the frame's left-to-right order.

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
clip(L, T, Bc, build, role):            // Bc: the bottom chain to use (B, or childB for the ceiling-forming line)
    // fast path: the line lies wholly inside the row extents of the two chains
    if min(L.y0, y_L(L.x1−1)) ≥ Tmax(T) and max(...) ≤ Bmin(Bc):
        emit(L.x0, L.x1, VISIBLE) ; return
    c  = L.x0
    tp = piece of T containing c ; bp = piece of Bc containing or after c     // split at c if needed
    yL = L.y0 ; yT = y_tp(c) ; yB = y_bp(c)                                   // u8.8
    while c < L.x1:
        if c < bp.x0:                                                 // gap in Bc (closed range, §7.9.4)
            e = min(L.x1, tp.x1, bp.x0)                               // L is ABOVE T throughout such a range
            emit(c, e, ABOVE) ; advance ; continue
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
`Bmin = H`, so every line of the camera cell skips the walk.

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
child span `[xa, xb)` minus the closed ranges is a set of open sub-ranges, and one
child portal is pushed per sub-range, its chain heads found by column in `childT` and
`childB`. Within any sub-range both chains are contiguous. A span that is closed
everywhere pushes nothing.

Skipping the recursion in closed columns is only correct with one fix-up. Every line
the child's subtree would have generated there is hidden and would merge into the
same piece, and a complete stack of lines from a cell's ceiling to its floor
telescopes to `ceiling ^ floor`. So `emit` EORs `CB ^ FB` into the piece when it
detects closure; this is exactly what the skipped subtree would have contributed
(checked in §13).

Most spans have no closed columns; a partly closed span is typically closed at one
end (a step rising into a lintel at the far end of a doorway), giving one child.
Closure in the middle of a span needs a V-shaped bound and is rare; it simply gives
two children.

### 7.10 Line extension across visits ("active lines")

A cell can be visited several times per frame (a room seen either side of a pillar,
or a wall split by soft-edge decomposition). Each visit produces pieces of the same
screen lines, and because visits occur in increasing x order, the pieces from
consecutive visits can be joined. Each cell keeps:

```
last_edge       edge index of the previous visit's last generated lines
last_xr         right column of the previous visit
active[4]       last emitted VISIBLE piece of each of that edge's ≤ 4 lines (0 if none)
```

```
extend_or_alloc(r0, r1, y0, g, colour):
    k = which of the ≤ 4 lines of this edge L is
    q = cell.active[k]
    if cell.last_edge == n and cell.last_xr == r0 and q != 0 and q.x1 == r0:
        q.x1 = r1                    // same edge, same line, contiguous: extend
        return q
    q = alloc(r0, r1, y0, g, colour) ; cell.active[k] = q ; return q
```

The geometry is identical across visits because the same edge transforms to the same
numbers (§7.2). An extended piece may have been in a previous child's chain; that
child was popped before this visit started, so re-linking it is safe. Once a visit
ends the cell's `last_xr` is set; a later visit that does not start exactly there
gets fresh pieces.

## 8. Rendering

### 8.1 Plotting lines

Iterate the pool; skip pieces with `colour == 0`, and rising pieces with `y0 ≥ H`
(a falling piece starting at H comes on screen where its row drops under H − ½).
The plotted row is the **nearest** row, `(y + 0x80) >> 8` — the same convention as
every re-anchor (§6 split, §7.9 emit), so a piece split at a cut plots exactly the
rows the unsplit line would have and a chain seam cannot notch the boundary; a
toggle that rounds past the screen top still clamps to row 0. Branch on direction
(up/down) and on `|g| ≤ 2` versus `> 2`:

- Steep lines: one pixel pair per column, `y += g`.
- Shallow lines: add the gradient three times, rotating the overflow bits into an
  index selecting a byte-pattern "sprite" for the 4 pixel columns of the byte, so a
  whole byte is plotted at a time. (Existing 6502 implementation.)

Each plot writes rows `y` and `y+1` with `E` on the even row and `O` on the odd row.

### 8.2 Fill

Two passes (even rows, odd rows), column-major, unrolled `EOR row,X : STA row,X`.
About 9 cycles per byte, roughly 18 ms for 4K; the two passes do not change the total.

### 8.3 Buffers and clearing

In-place for now: plot into the back buffer, fill in place, clear it for the next
frame (about 10 ms for 4K). A separate 4K line buffer, cleared by re-plotting the
frame's pieces with EOR, would likely save 5 ms or more per frame and can be adopted
later without changing the capture.

## 9. Scaled sprites

Sprites are drawn into the toggle image between line plotting and the fill, as
change points, so that the fill does the work of colouring them. The cost is then
proportional to the number of colour changes and crossing lines, not to the sprite's
area, and overlapping sprites and occlusion by nearer geometry come out right.

### 9.1 Art format

A sprite is stored column-wise as a list of change points `(y, colour)` per column,
`colour` a palette index 0..10 where 0 is transparent and 1..10 are the ten 2×2
dither patterns of §5, given as `(E, O)` byte pairs in `pal[]`. A column is a
sequence of runs; consecutive opaque runs form a **silhouette run** with internal
colour changes, and transparent gaps separate silhouette runs.

### 9.2 Instances

A sprite **instance** is a sprite placed in a cell. Once per frame, when its cell is
first visited, it is projected: depth, screen column range `[cx0, cx1)`, base row and
scale. Per screen column the sprite column comes from a horizontal step table for the
scale; per change point the screen row is `base + vscale[y]`. Both tables are per
scale, not per instance. A cell's instances are sorted far to near at the same time.

Per-frame instance record (~12 bytes): sprite, depth, `cx0, cx1`, base row, scale,
head of its segment list, link in the order list, and during the sprite pass the
DDA cursors of its current segment (~6 bytes).

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
on push of a visit of cell X, span [l, r), chains T, B:     (root: rootT, rootB)
    if X has instances:
        if first visit of X this frame: project and sort X's instances
        for each instance of X with [cx0, cx1) ∩ [l, r) non-empty:
            seg = alloc segment over the overlap [s0, s1)
            for each piece of T covering a column in [s0, s1), in x order: append its index to seg.T
            for each piece of B likewise: append to seg.B
            append seg to the instance's segment list
```

A **segment** is `{x0, x1, next, nT, nB, T[nT], B[nB]}`: 5 bytes plus one per bound
piece, typically 7–10 bytes, allocated from a per-frame byte pool. Consecutive
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
    for each instance of X that received a segment from this visit, far to near:
        if the instance is already in the order list: unlink it
        append it to the tail of the order list
```

An instance is therefore issued once, at the pop of the *last* visit of its cell
that overlaps it. That is after the descendants of all its visits (farther) and
before its ancestors (nearer); the instances of X keep their far-to-near order among
themselves because they are re-appended in that order. The order list is a linked
list through the instance records; unlinking needs either a back link or a
move-to-tail over a small array, either is fine for the number of instances a frame
holds.

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
        while i < n and list[i].row < y:
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
pixel to what lies below. Insertions never touch `acc`, and masking distributes over
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

### 9.6 Cost and memory

Per covered byte column: ~11 cycles per scanned row (so at most ~700), plus per
opaque run in each pixel column a short list walk (a few cycles per entry, including
entries that belong to other pixels), two pixel plots at ~35 each, one plot per
erased toggle, and ~30 per internal colour change. This is independent of sprite
height. Direct masked plotting over the filled screen costs ~26 cycles per pixel per
row (two read-modify-writes), i.e. ~104 per row per fully covered byte column, so
the two break even at about seven rows of sprite height; a 16-row sprite is ~1.5×
cheaper this way, a 32-row one ~3×, a full-height one ~6×.

Scratch, reused per byte column: one list of (row, E, O), e.g. 24 × 3 = 72 bytes;
the cap bounds how many distinct toggle rows one byte column may hold (world lines
plus every opaque run edge of every overlapping sprite), and `put` clamps beyond it.
Per frame: instance records (~18 bytes each including DDA cursors), clip segments
(5 bytes + 1 per bound piece, typically 7–10), `pal[11]` (22 bytes), `pixmask[4]`,
and the scale tables. Nothing is added to the piece record; the pool must stay
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
