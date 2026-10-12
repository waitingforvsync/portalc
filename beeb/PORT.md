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
| `pt_log2` | `LOG2_16 bits` | done: every x at each bound from 9 to 16, summed |
| `pt_exp2` (`exp2_byte`) | `EXP2_8` | done: every l under 2048 |
| `pt_polar_of` and `pt_quadrant`, called by `pt_vcache_vert` | `POLAR` | done: a near square and a far grid, summed |
| `pt_view_col` | `LDA a2c,X` | done (a lookup) |
| `pt_lsec`, `pt_logsin`, `pt_mul_sin`, `pt_dot_dir`, `pt_mulu8` and the rest | the tables only | not started |

### The map and the vertex cache

| C | 6502 | State |
|---|---|---|
| `pt_scene_init` (map.c) | mapdata.6502, made by `portalc_mapasm` (`make map`) | done, checked once by hand in b2 |
| `pt_render_init`, `pt_vcache_init`: emptied caches | `map_init` (map.6502): `vc_slot`, `cell_visit`, `vc_num` | done |
| `pt_vcache_vert` | `VCACHE_VERT_TO_X`, with `vcache_make` on a miss | done: every vertex from every vertex, `portalc_vcacheref` |
| `pt_vcache_reset` | `vcache_reset` | done |
| `pt_vcache_move`, `pt_vcache_locate` | — | not started |

### The camera

| C | 6502 | State |
|---|---|---|
| `pt_camera_tick` (camera.c) | `update_camera` (camera.6502) | stub. The turn keys come first |

### The capture (render.c)

| C | 6502 | State |
|---|---|---|
| `pt_render_root`, `frame`'s setup | `.capture`'s start (capture.6502) | done, sprites aside: `portalc_captureref` |
| `pt_render_root_side`, `root_screen_angle` | inline in `.capture` | done |
| `root_walk` | inline in `.capture` | done, sprites aside (`inst_new`, `inst_clip`) |
| `visit_of` | `visit_of` | done |
| `height_of`, `pt_render_height` | `HEIGHT_OF` | done: every z at every eye height |
| `vert_to_screen_angle` | `VERT_TO_SCREEN_ANGLE` | done |
| `screen_angle_to_column` | `SCREEN_ANGLE_TO_COLUMN` | done |
| `side_start`, `side_end` | inline in `.capture` | done. `side_end`'s near case is untested: see [Next](#next) |
| `frame`'s visit cache clear | `.capture`'s end | done |
| `walk`, `side_next`, `descend`, `enter` | — | not started |
| `process_edge`, `edge_ends`, `edge_line`, `make_line` | — | not started |
| `scan_of`, `scan` and the column functions, `close_columns`, `flat_top` | — | not started |
| `record`, `plot_run`, plot.c's `pt_plot_` | — | not started |
| `inst_new`, `inst_clip`, `inst_hide` | — | not started (sprites) |

### After the capture

| C | 6502 | State |
|---|---|---|
| `pt_mode5_fill` (mode5.c) | `fill` (fill.6502) | stub |
| `pt_sprite_plot` (sprite.c) | `sprites` (sprites.6502) | stub |
| `unplot` (render.c), plot.c's `pt_unplot_` | `unplot` (unplot.6502) | stub |

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
| `visits[i].inst` | — (sprites) |
| `pt_walk.vi`, `.x`, `.tmax`, `.bmin`, `.wrap` | `walk_vi`, `walk_x`, `walk_tmax`, `walk_bmin`, `walk_wrap` |
| `pt_walk.u0`, `.u1`, `.c0`, `.c1` | `walk_u0`, `walk_u1`, `walk_c0`, `walk_c1` |

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
`CAM_HEIGHT`, `PT_IN_FWD` is `IN_FWD`, `PT_MAX_VCACHE` is `MAX_VCACHE`.
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
- **The root side search doesn't test the last side.** If no earlier side
  is the one, the last one is. The C asserts that it gets that far.
- **A height is two bytes.** `below` is bit 7 of the log's high byte, which
  is otherwise 0 or 17 to 24.
- **`LOG2_16` takes a bound.** It's `MAP_OFFSET_BITS` (map.6502), from the
  map's span, and the bound decides how it shifts. The C's `pt_log2` has no
  bound.
- **Tables numbered from 1 keep entry 0, unused:** the visits and the
  vertex cache's entries. There's never a `table-1` base.

## Next

- The walk proper: `walk`, `side_next`, `process_edge` and everything
  under it, then `descend` and `enter` for the cells beyond the portals.
  One thing to decide first: where a child walk's state goes, since the
  `walk_` zero page holds one walk.
- The kept lines and the records stay as the C has them for now.
- Then `update_camera`'s turn keys, the fill and the unplot. That's the
  first demo.
- Later: sprites, then moving, collision and `pt_vcache_locate`.
- A testing gap: `side_end`'s near case only matters on the eye's own
  cell's last side. There the end has to be a turn on, column 128, not
  column 0. The walk only gets to that side through `side_next`. Once
  that's in, `make test` needs camera positions whose view's right edge
  is on that side, such as near a corner looking across the cell. From a
  cell's centre it hardly ever is.
