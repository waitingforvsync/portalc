# Porting to the 6502

How far the port has got, and what the C's names are called on the Beeb.
If you're carrying on with the port, read this first, then
`codingstandards.md`. Update it with every step.

The C in `src/` is the reference. The 6502 does the same thing, byte for
byte wherever `make test` can check it. It takes shortcuts where the C's
checks show they're safe: see [Where the 6502 differs](#where-the-6502-differs).

The first demo is a stationary camera with only the turn keys. There's no
moving, no collision, no stepping through portals and no sprites yet.

## How far it's got

"Done" means ported and checked by `make test` against the C reference
tool named, unless it says otherwise. Anything not done is an `RTS` with
ambitions.

### The loader and the main loop

| C | 6502 | State |
|---|---|---|
| `pt_arith_init` (arith.c) | `make_tables` (tables.6502), packed at assembly time by `pk_` and unpacked by `UNPACK` | done: every byte, `portalc_tables` |
| main.c's loop: `pt_camera_tick`, `pt_render_frame` | `start` (start.6502): `update_camera`, `capture`, wait, `fill`, `sprites`, swap, `unplot` | the loop is done; the stages are stubs |
| main.c's `gather_input` | `irq` (irq.6502): vsync count, keys as the `IN_` bits, screen swap | done, checked by hand in jsbeeb |
| — | `boot.6502`: finds a sideways RAM bank | Beeb only, in progress |
| — | `entry` (entry.6502): takes over the machine, copies the code out, sets up the screen | Beeb only |
| — | `clear_buffers` (clear.6502): T, its junk rows and both screens | Beeb only |

### The arithmetic

| C | 6502 | State |
|---|---|---|
| `pt_log2_8` | `LOG2_8` | done: every b, `portalc_arithref` |
| `pt_log2` | `LOG2_16 bits`: the low byte shifted down k - 1, through compares on the high byte into a run of shifts | done: every x at each bound from 9 to 16, summed |
| `pt_exp2` (`exp2_byte`) | `EXP2_8` | done: every l under 2048 |
| `pt_exp2` | `EXP2_16 bits`: `EXP2_8`, or 1.f shifted up, through compares into a run of shifts up to 2^13, and past that a branch into it self-modified from `exp2_skip` (arith.6502) | done, in `make_line`'s check |
| `pt_mulu8` | `MULTIPLIER` (a) then `MULTIPLY` (a * Y), by the quarter squares | done, in `make_line`'s check |
| `pt_polar_of` and `pt_quadrant`, called by `pt_vcache_vert` | `POLAR` | done: a near square and a far grid, summed |
| `pt_view_col` | `LDA a2c,X` | done (a lookup) |
| `pt_log_secant`, with the C's `view8` | `LOG_SECANT`: where in `lsec` it is | done, in `edge_ends`' check |
| `pt_log_sine` | `LOG_SINE`: where in `lsec16` it is, negated | done, in `edge_ends`' check |
| `pt_mul_sin`, `pt_dot_dir` and the rest | the tables only | not started |

### The map and the vertex cache

| C | 6502 | State |
|---|---|---|
| `pt_scene_init` (map.c) | mapdata.6502, made by `portalc_mapasm` (`make map`) | done, checked once by hand in b2 |
| `pt_render_init`, `pt_vcache_init`: emptied caches | `map_init` (map.6502): `vc_slot`, `cell_visit`, `vc_num` | done |
| `pt_vcache_vert` | `VCACHE_VERT_TO_X`, with `vcache_make` on a miss | done: every vertex from every vertex, `portalc_vcacheref` |
| `pt_vcache_reset` | `vcache_reset` | done |
| `pt_map_ring_next` (map.h) | `RING_NEXT` (map_macros.6502) | done |
| `pt_vcache_move`, `pt_vcache_locate` | — | not started |

### The camera

| C | 6502 | State |
|---|---|---|
| `pt_camera_tick` (camera.c) | `update_camera` (camera.6502) | stub. The turn keys come first |

### The capture (render.c)

| C | 6502 | State |
|---|---|---|
| `pt_render_capture` | `capture` (walk.6502): `capture_root`, `walk`, `forget_visits` | done, sprites aside: `portalc_walkref`, whole captures from the 146 places at 32 angles each; and `portalc_walk3ref`, the stress scene's 106 places, the depth capped at 3 |
| `pt_render_root`, `frame`'s setup | `capture_root` (capture.6502) | done, sprites aside: `portalc_captureref`, from 146 places, each cell's centroid and near each of its corners |
| `pt_render_root_side`, `root_screen_angle` | inline in `capture_root` | done |
| `root_walk` | inline in `capture_root` | done, sprites aside (`inst_new`, `inst_clip`) |
| `visit_of` | `visit_of` | done |
| `height_of`, `pt_render_height` | `HEIGHT_OF` | done: every z at every eye height |
| `vert_to_screen_angle` | `VERT_TO_SCREEN_ANGLE` | done |
| `screen_angle_to_column` | `SCREEN_ANGLE_TO_COLUMN` | done |
| `side_start` | `side_start`, which runs on into `side_end` | done |
| `side_end` | `side_end` | done |
| `side_next` | `side_next`, which runs on into `side_end` | done: the eye's cell walked side by side to the portal's right column, as `walk` walks it |
| `edge_ends` | `edge_ends` | done: each of those sides with columns |
| `make_line` | `make_line` | done: those sides' lines at the cell's ceiling and floor and the eye's height, and 65,536 made-up lines for the paths the walk never takes |
| `exp2_sat`, `row_move` (`pt_mulg_u16`) | `exp2_sat`, `row_move` | done, in `make_line`'s check |
| `frame`'s visit cache clear | `forget_visits` | done |
| `walk`, `descend`, `enter` | `walk` (walk.6502), with `descend`'s cut-off inline; `enter` | done, in the walk's checks |
| `process_edge`, `edge_line` | `process_edge`, `edge_line`; this cell's lines through `ceiling_line` and `floor_line` (`ceiling_line_xy` and `floor_line_xy` colour them first) | done, in the walk's checks: every path but a level ceiling that changes colour, which neither scene has |
| `clip_plain`, `clip_floor`, `clip_ceil` | the same (clip.6502) | done: 256 made-up lines from `make_line`, each clipped 16 times, every kind against every combination of the bounds' edges, `portalc_clipref` |
| `scan_of`, `scan` | `SCAN_EDGES`, `SCAN` (clip_macros.6502) | done, in the clips' check |
| `plain_col`, `floor_col`, `ceil_col` | `plain_columns`, `floor_columns`, `ceil_columns`; and a plain line's own columns in `plain_own`, through `PLAIN_OWN` | done, in the clips' check |
| `floor_fast_col`, `ceil_fast_col` | `WRITE_ROWS`: X counts up to the page's end or middle, the store's base and the gradient patched in | done, in the clips' check |
| `col_visible`, `col_hidden`, `run_end`, `hrun_end` | `stretch_visible`, `stretch_hidden`, `stretch_end`, `visible_end`, `hidden_end` | done, in the clips' check |
| `fast_inside`, `on_screen`, `flat_top` | `COLUMNS`, `ROW_AT` and `ROWS_INSIDE`; `ON_SCREEN`, `flat_top` | done, in the clips' check |
| the C sweep's 19,162 clip calls | replayed through the assembled clips on a cycle-exact model: the same as the C's, 5,210 cycles a frame | checked once, 2026-10-14 |
| `close_columns` | `close_columns` | done, in the clips' check |
| `plot_run`, with `record` | `plot_run` (plot.6502) | done: 9,600 made-up runs at 75 gradients, each run's record and `toggle_end`, and T after each gradient's, `portalc_plotref` |
| `pt_plot_shallow`, `pt_plot_steep` | `plot_shallow`, `plot_steep`, through `SHALLOW` and `STEEP` (plot_macros.6502), each expanded falling and rising | done, in `plot_run`'s check |
| `pt_plot_flat` | `plot_flat` | done: every x0 < x1 |
| mode5.h's cursor | `MODE5_AT`, `MODE5_EOR` (and `MODE5_EOR_X`, the mask in X), `MODE5_DOWN`, `MODE5_UP`, `MODE5_NEXT` | done, in the plotters' checks |
| `pt_mode5_plot` | — | not ported: only `close_columns` uses it (step 4), and it never ran in the sweep |
| `inst_new`, `inst_clip`, `inst_hide` | — | not started (sprites) |

### After the capture

| C | 6502 | State |
|---|---|---|
| `pt_mode5_fill` (mode5.c) | `fill` (fill.6502) | stub |
| `pt_sprite_plot` (sprite.c) | `sprites` (sprites.6502) | stub |
| `unplot` (render.c), plot.c's `pt_unplot_` | `unplot` (unplot.6502) | stub. The records it reads are made |

## Names

How the C's names come out on the Beeb. A struct's fields become tables
(struct of arrays), and a 16-bit value becomes a `lo`/`hi` pair of tables,
or two bytes of zero page.

### The camera and the frame

| C | 6502 |
|---|---|
| `pt_camera.x` (u16.8) | `camxf` (the fraction), `camx` (2 bytes) |
| `pt_camera.y` | `camyf`, `camy` |
| `pt_render.cam4`: the facing, in 1024ths | `camangle` |
| `pt_camera.angle`, in 256ths | — (only `CAM_START_ANGLE`, which `entry` multiplies by 4) |
| `pt_camera.cell`, `pt_render.cam_cell` | `camcell` |
| `pt_render.vl` | `viewleft` |
| `pt_render.eye_z` | `eye_z` |
| `num_visits`, `num_kept`, `num_recs` | the same |
| `pt_render.cell_visit` | `cell_visit` |
| `pt_render.toggle_end` | `toggle_end` |

### Visits and the walk

| C | 6502 |
|---|---|
| `visits[i].cell`, `.side`, `.xr`, `.kept` | `visit_cell`, `visit_side`, `visit_xr`, `visit_kept` |
| `visits[i].hc` (`pt_height`) | `visit_hclo`, `visit_hchi`, with `below` in bit 7 of `hchi` |
| `visits[i].hf` | `visit_hflo`, `visit_hfhi`, likewise |
| `visits[i].split_angle` | `visit_split_anglelo`, `visit_split_anglehi` |
| `visits[i].inst` | — (sprites) |
| `pt_walk.vi`, `.x`, `.tmax`, `.bmin`, `.split_angle` | `walk_vi`, `span_x0`, `walk_tmax`, `walk_bmin`, `walk_split_angle` |
| `pt_walk.u0`, `.u1`, `.c0`, `.c1` | `walk_u0`, `walk_u1`, `walk_c0`, `walk_c1` |
| — | `walk_e0`, `walk_e1`: the ends' vertex cache entries |
| `walk`'s `depth` | `walk_levels`: the walks that may still nest, counting down from `MAX_DEPTH` - 1 |
| `process_edge`'s child walk (`cw`) | its visit in X, and `enter.tmax`, `enter.bmin` |
| `pt_edgelines.kept`, `.use`, `.have` | `edge_line.kept` (the entry * 4), `edge_line.use`, `edge_line.ends` (bit 7) |
| `r->kept[k][slot]` (`pt_kept`) | `kept_x0`, `kept_x1`, `kept_ylo`, `kept_yhi`, `kept_gradlo`, `kept_gradhi`, `kept_endy`, by entry * 4 + slot |
| `PT_K_CEIL`, `_CEIL_ADJ`, `_FLOOR`, `_FLOOR_ADJ` | `KEPT_CEILING`, `KEPT_CEILING_ADJACENT`, `KEPT_FLOOR`, `KEPT_FLOOR_ADJACENT` |
| `pt_map_ring_next` after `pt_map_twin` | `side_entry`, a map table |
| `pt_shade(i)`, `pt_pattern_xor` | `shade_even`, `shade_odd` (map tables, from palette.c), and `PATTERN_XY`, or `pattern_xy` for the rarer lines |
| `pt_ends.logdepth0`, `.logdepth1`, `.logcolumns` | `logdepth0`, `logdepth1`, `logcolumns` |
| `pt_line.x0`, `.x1`, `.y`, `.below` | `line_x0`, `line_x1`, `line_y`, `line_below` (&80 below, else 0) |
| `pt_line.g`, `.rl` | `line_grad`, `line_endy` |
| `pt_line.ce`, `.co` | — (the caller sets the colour) |
| `PT_EDGE_OFF`, `PT_EDGE_L` | `EDGE_OFFSET`, `EDGE_LOG` (13, the log's high byte) |

### Plotting

| C | 6502 |
|---|---|
| `pt_run.x0`, `.x1`, `.y0` and `.yf` | `run_x0`, `run_x1`, `run_y` (the row in the high byte) |
| `pt_run.g` | `line_grad`: a run's gradient is always its line's |
| `pt_pattern.e`, `.o` | `pattern_even`, `pattern_odd` |
| `pt_mode5_cur` | `cursor` (the block), with Y |
| `pt_mode5_at`, `_eor`, `_down`, `_up`, `_next` | `MODE5_AT`, `MODE5_EOR`, `MODE5_DOWN`, `MODE5_UP`, `MODE5_NEXT` |
| `pt_rec.x0`, `.x1`, `.y`, `.g` | `rec_x0`, `rec_x1`, `rec_ylo`/`rec_yhi`, `rec_gradlo`/`rec_gradhi` |
| record's `toggle_end` update | `NOTE_TOGGLE` (A the row, X the byte column) |
| the plotters' `gm` | `grad`; and `grad2`, `grad3`, `grad4`, `grad_less1` |
| `pt_plot_steep`'s `gi`, `gf` | `grad_whole`; `gf` is `line_grad`'s low byte itself (`STEEP`'s `grad_fraction`) |
| `f` | `fraction` (rising, 255 less it): `run_y` itself |

### The clips

| C | 6502 |
|---|---|
| `pt_render.top`, `.bot` | `bound_top`, `bound_bottom` |
| a clip's `xa`, `xb` | `span_x0`, `span_x1`: the walk's |
| `tmax`, `bmin` | `walk_tmax`, `walk_bmin`; `clip_ceil`'s `cbmin` comes in A |
| `screen_top_`, `screen_bot_` | — (`scan_edges`, from the extents: bit 7 the top's the screen's, bit 6 the bottom) |
| `pt_scan.v`, `.h` | the open stretch: `run_x0` its start, and `run_y` (visible) or `stretch_row` (hidden); `stretch` says which |
| `pt_scan.ext`, `.room` | the extent in A, and `clip_floor.room`, `clip_ceil.room` (bit 7) |
| `scan`'s `l`, `rr` | `own_x0`, `own_x1` |
| `step1`'s y | Y, the row, and `scan_fraction`; `scan_whole` the gradient's whole rows |

### The vertex cache

| C | 6502 |
|---|---|
| `pt_vcache.slot`, `.vert` | `vc_slot`, `vc_vert` |
| `pt_vcache.num`: entries made, the null one included | `vc_num`: the last entry made, one less |
| `pt_vcache.polar[i].angle` | `vc_anglo` (the step within the quadrant), `vc_anghi` (the quadrant) |
| `pt_vcache.polar[i].lr` | `vc_logdistlo`, `vc_logdisthi` |
| `pt_vcache.x`, `.y` | — (the cache reads `camx` and `camy`) |

### The map

| C | 6502 |
|---|---|
| `verts[i].x`, `.y` | `vert_xlo`, `vert_xhi`, `vert_ylo`, `vert_yhi` |
| `cells[i].first` | `cell_first`, with `MAP_SIDES` after the last cell |
| `cells[i].floor_z`, `.ceil_z` | `cell_floorz`, `cell_ceilz` |
| `cells[i].floor`, `.ceil`, `.sprite` | `cell_floor`, `cell_ceil`, `cell_sprite` |
| `edges[i].angle`, `.colour`, `.len8` | `edge_angle`, `edge_colour`, `edge_len8` |
| `sides[i].v`, `.edge`, `.adj`, `.anghi` | `side_v`, `side_edge`, `side_adj`, `side_anghi` |
| `pt_map_ring_next(across, pt_map_twin(m, s))` | `side_entry`, made by `portalc_mapasm`: the side after the twin, where a walk through the portal begins; 0 for a wall |
| `sprites[i].x`, `.y`, `.art`, `.ls` | `sprite_xlo` and so on, `sprite_art`, `sprite_lslo`, `sprite_lshi` |
| `arts` | `art_pate`, `art_pato`, `art_lo`, `art_hi`, `art0`... |
| `num_verts`, `num_cells`... | `MAP_VERTS`, `MAP_CELLS`... (assembly-time constants) |

### The tables

They have the same names as `pt_arith_tables`, except:

| C | 6502 |
|---|---|
| `lsec16`, `sq1`, `sq2` | `lsec16lo`/`lsec16hi`, `sq1lo`/`sq1hi`, `sq2lo`/`sq2hi` |
| `lsec` | the first 128 bytes of `lsec16lo` |
| `lsx` | only the loader's, in `Scratch`, to make `lsec16` |
| `rpost` | `rpost`, which is `rpostmem` - 32: it starts at index 32 |
| — | `trowhi`: row y's page in T |

### Constants

`PT_` comes off: `PT_SCREEN_WIDTH` is `SCREEN_WIDTH`, `PT_CAM_HEIGHT` is
`CAM_HEIGHT`, `PT_IN_FWD` is `IN_FWD`, `PT_MAX_VCACHE` is `MAX_VCACHE`,
`PT_MAX_RECS` is `MAX_RECS`, `PT_MAX_DEPTH` is `MAX_DEPTH`, `PT_MAX_KEPT` is
`MAX_KEPT`.
They're all in constants.6502. `POST_NEAR` and `POST_SCALE` name numbers the
C writes out: 256 and 1.25 * 2^15, in arith.h's notes on `rpost`.

## Where the 6502 differs

- **The facing is in 1024ths.** `camangle` holds what the C calls `cam4`.
  The C's camera keeps 256ths, and so does the map, so `entry` multiplies
  `CAM_START_ANGLE` by 4.
- **The vertex cache keeps no position.** Whatever moves the camera has to
  call `vcache_reset`. The C's `pt_vcache_move` compares positions instead.
- **Fewer checks.** `vcache_make` doesn't check for a full cache, and
  `visit_of` doesn't check for too many visits. The C panics on both.
  `side_next` doesn't check that it isn't leaving the eye's cell's last
  side, a turn on, which the C asserts.
- **The root side search doesn't test the last side.** If no earlier side
  is the one, the last one is. The C asserts that it gets that far.
- **A height is two bytes.** `below` is bit 7 of the log's high byte, which
  is otherwise 0 or 17 to 24.
- **`LOG2_16` and `EXP2_16` take bounds.** `LOG2_16`'s is `MAP_OFFSET_BITS`
  (map_macros.6502), from the map's span; `make_line` works out an end on the
  screen's offset with `EXP2_16 13`, inline, and the rest with `exp2_sat`'s
  `EXP2_16 16`. The bound decides how they shift. The C's have none.
- **Tables numbered from 1 keep entry 0, unused:** the visits and the
  vertex cache's entries. There's never a `table-1` base.
- **The walk keeps its side's ends' vertex cache entries** (`walk_e0`,
  `walk_e1`), so `edge_ends` reads their polar forms straight off. The C
  looks the vertices up again, finding the end's round the ring.
- **`make_line` only makes what a line uses.** A line with no columns
  gets only its columns and its side, not its rows. `row_move` doesn't
  check the row stays on the screen, and its multiply doesn't saturate:
  the C asserts the row stays on, which rules both out.
- **`edge_ends` adds log sines negated** (`LOG_SINE` points at `lsec16`), so
  where the C halves a sum of two log sines, rounding down, the 6502 halves
  their negations' sum + 1.
- **The plotters work out `toggle_end`, not `record`.** They pass every byte
  column's lowest row anyway: a falling run's at the byte's last column, a
  rising run's at its first. They count the row as they move, and
  `NOTE_TOGGLE` it. So `plot_run` only stores the record, and a colourless
  run (a bound's line) goes through its plotter EORing nothing, where the C
  records it and plots nothing. `plot_flat` itself notes nothing, as the C's
  hidden runs record nothing; `plot_run` notes a visible flat run's columns.
- **Rising runs keep 255 less the fraction**, so a step's borrow up is a
  carry, and the two ways share their adds.
- **Shallow whole bytes don't all take the tree.** Under a gradient of 64 a
  byte's four steps carry once at most: one add of 4g gives the fraction,
  and if it carried, compares with g, 2g and 3g give the step that did. From
  64 the tree runs, and a step after a carry adds g - 1, the carry known.
- **`plot_flat` keeps Y as the offset in the row's page**, the cursor's low
  byte 0, so a byte on is `TYA:ADC #7:TAY` from the odd scanline.
- **`record` doesn't check for too many records.** The C panics past
  `MAX_RECS`.
- **`plot_run` corrupts `run_y`.** A shallow or steep run counts its
  fraction and row in it as it goes (rising, 255 less the fraction), rather
  than copy them. Nothing reads it after; the record is taken first.
- **No constant rows for the screen's edges.** The C points a scan at
  `screen_top_` or `screen_bot_` where an extent says a bound is the
  screen's edge all along. The 6502 picks code instead, once a scan: a
  plain line's own columns go through one of eight variants of
  `PLAIN_OWN`, falling or rising with each bound the screen's or the
  array's. A screen edge needs no lookup (row 0 is `BEQ`, the junk row
  `CPY #64`). As a line's rows only go one way, a visible line only meets
  the edge it runs towards; and as its rows are on its own side of the
  middle row (`make_line`'s, which the C asserts in `scan`), with both edges
  the screen's a line above falling, or below rising, is visible to the end
  once visible, and one that meets its edge stays on it. The four variants
  with the bottom in the array have their own loop over hidden columns; the
  rest, and the rarer column code, go through `plain_columns` and test
  `scan_edges`.
- **A clip starts from the span's columns.** `COLUMNS` puts the span in
  `run_x0` and `run_x1` and asks, in three compares, whether it's all the
  line's; if so, and the line's inside the extents (`ROWS_INSIDE`), it's
  `JMP plot_run` with nothing copied, and a slow one scans its own columns
  (`SCAN_OWN`). A line that doesn't cover the span, with the bound on its
  side the screen's edge, has the columns cut off flat on row 0 or left
  alone, and the rest as before; only with that bound in the array does it
  get the C's whole scan.
- **The open stretch is kept in the plotters' inputs** (`run_x0`, `run_y`),
  so ending it is just the call.
- **`WRITE_ROWS` patches its store's base and the gradient into the loop**,
  so X counts up to the page's end (`bound_top`, at a page's start) or its
  middle (`bound_bottom`, at its middle) with no compare a column.
- **`toggle_end` and `num_recs` are plot.6502's**, as the records are; the
  capture only resets them.
- **A child walk keeps only three bytes of its parent's.** The C's walk has
  the child in a walk of its own, `cw`; the 6502 has one walk, in zero page,
  and pushes the parent's `walk_vi`, `walk_tmax` and `walk_bmin` round the
  child's. The rest comes back: `span_x0` is the side's end's column, the
  window is the visit's (`visit_split_anglelo`, `visit_split_anglehi`, made
  at its first visit, as the C now makes it too), and the parent goes on to its next side
  from that side's start, as a later visit would. The C asserts all three.
  That's 5 bytes of stack a level with the return, 75 at the cap.
- **`process_edge` doesn't enter the child.** It returns with C set, the
  child's visit in X and its extents in `enter.tmax` and `enter.bmin`, and
  the walk saves itself and calls `enter`; through the last side, it calls
  `enter` in its own place, as the C's tail call does, nothing saved.
- **The depth counts down**, in `walk_levels`, from `MAX_DEPTH` - 1: past 0,
  the child's cut off.
- **Entries and shades are tables.** `portalc_mapasm` writes `side_entry`, the
  side after the twin, where the C searches the cell across's ring for the
  twin and steps on; and `shade_even` and `shade_odd`, palette.c's catalogue.
- **The walk works out the span.** Its side loop sets `span_x1`, the side's
  end or the portal's right column, and hands `process_edge` C set if the
  side ends within the portal; `span_x0` is where the last span ended. It
  skips two tests the C keeps as asserts: the last side's span is never
  empty (the last span ended left of the portal's right column), and the
  walk never meets a side facing away with columns.
- **A first visit keeps the parent's start.** The side after the twin starts
  where the portal does, at the same screen angle in the new window, so the
  parent's `walk_u0`, `walk_e0` and `walk_c0` stay, and only `side_end` runs.
  The C's `enter` does the same, and asserts the angle.
- **The walk leaves out sprites** (`inst_new`, `inst_clip`, `inst_hide`), and
  the C's checks on side counts, a visit's columns running left to right,
  and the window's range.
- **The capture's split**: `capture_root` is capture.6502's, `capture`, which
  walks, walk.6502's, so the checks that only want the capture's parts
  needn't take the walk and the clips.

## Next

- The walk proper, a step at a time, each checked against the C:
  1. ~~The ends of a side: `edge_ends`, and `side_next`.~~ Done.
  2. ~~Lines: `make_line`, which needs a 16-bit exp and a multiply.~~ Done.
  3. ~~The plotters into T, and `record` with `toggle_end`.~~ Done.
  4. ~~The clips: `scan`, `clip_plain`, `clip_floor`, `clip_ceil`,
     `close_columns`.~~ Done.
  5. ~~The walk: `walk`, `process_edge`, `enter`, `descend`, the kept
     lines.~~ Done.
- The records stay as the C has them for now.
- Then `update_camera`'s turn keys, the fill and the unplot. That's the
  first demo.
- Later: sprites, then moving, collision and `pt_vcache_locate`.
- To revisit, if the cycles are wanted: a `side_v1` map table, each side's
  end vertex (`pt_map_side_v1`), written by `portalc_mapasm`. `side_end`
  would then be `LDX side_v1,Y`, instead of finding the next side through
  `walk_vi`, `visit_cell` and `RING_NEXT` and reading its `side_v`: 228
  cycles a frame and 16 bytes of code, for `MAP_SIDES` bytes of map (119 in
  the demo map). Left out for now, as it repeats `side_v`. Copying each
  ring's first side after its last, for `side_v+1,Y`, costs more: the slot
  is a hole in every side table, 5 bytes a cell.
- To revisit: once the fill exists, it could put each column's
  `toggle_end` back as it reads it, and `capture_root`'s reset loop goes.
- To revisit: wiping all of `Persist` in one go in `map_init` (15 bytes
  against 23), which would mean nothing in `Persist` is set before it runs.
- To revisit: the plotters' inputs (`run_x0`, `run_x1`, `run_y`) are file
  scope. Declared in a routine's own scope instead (`plot_run.x0`), as
  Baron's guide encourages, once the scan shows who calls what.
