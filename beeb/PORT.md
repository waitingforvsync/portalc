# Porting to the 6502

How far the port has got, and what the C's names are called on the Beeb.
If you're carrying on with the port, read this first, then
`codingstandards.md`. Update it with every step.

The C in `src/` is the reference. The 6502 does the same thing, byte for
byte wherever `make test` can check it. It takes shortcuts where the C's
checks show they're safe: see [Where the 6502 differs](#where-the-6502-differs).

The demo walks: the camera turns and moves, slides along walls and steps
through portals, sprites drawn.

## How far it's got

"Done" means ported and checked by `make test` against the C reference
tool named, unless it says otherwise. Anything not done is an `RTS` with
ambitions.

### The loader and the main loop

| C | 6502 | State |
|---|---|---|
| `pt_arith_init` (arith.c) | `make_tables` (tables.6502), packed at assembly time by `pk_` and unpacked by `UNPACK` | done: every byte, `portalc_tables` |
| main.c's loop: `pt_camera_tick`, `pt_render_frame` | `start` (start.6502): `update_camera`, `capture`, wait, `fill`, `sprites`, swap, `unplot` | done: the demo runs, walking, sprites and all |
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
| `pt_mul_sin`, `pt_mul_cos`, with `sin_mag` | `MULTIPLY_SINE` (cosine by a flag), with `SINE`: \|v\| times the magnitude, the low byte's product rounded, then signed | done: every angle, 128 made-up values at each, summed by four angles, `portalc_arithref` |
| `pt_mul_sin` of whole units shifted up 8 (the move, the pushes) | `UNITS_SINE`: n times the magnitude, one multiply, exact | done: every angle, every n under 128 |
| `pt_dot_dir` | `DOT_DIRECTION` | done: every angle, 128 made-up vectors at each |
| `pt_mulu16_frac` | `MULTIPLY_FRACTION` | done: every fraction, 128 made-up values at each |
| `pt_rpost` | `LDA rpost,X`, X `d2 >> 3` | the table: done, `portalc_tables` |

### The map and the vertex cache

| C | 6502 | State |
|---|---|---|
| `pt_scene_init` (map.c) | mapdata.6502, made by `portalc_mapasm` (`make map`) | done, checked once by hand in b2 |
| `pt_render_init`, `pt_vcache_init`: emptied caches | `map_init` (map.6502): `vc_slot`, `cell_visit`, `vc_num` | done |
| `pt_vcache_vert` | `VCACHE_VERT_TO_X`, with `vcache_make` on a miss | done: every vertex from every vertex, `portalc_vcacheref` |
| `pt_vcache_reset` | `vcache_reset` | done |
| `pt_map_ring_next` (map.h) | `RING_NEXT` (map_macros.6502) | done |
| `pt_vcache_locate`, `side_crossed` | `vcache_locate`: each portal side's angle from its start vertex to its end (`RING_NEXT`), 10 bits | done: the cells located at 8 by 8 positions round each place, -31 to 32 units off, from its cell: `portalc_vcacheref` (3,894 sides crossed, 26 half turns, 9 of them crossed, 353 locates of two hops or more) |
| `pt_vcache_move` | — | not ported: the camera empties the cache whenever it moves (the C's camera too); only `pt_render_frame`, the C's probes', compares |

### The camera

| C | 6502 | State |
|---|---|---|
| `pt_camera_tick` (camera.c) | `update_camera` (move.6502): the frame's ticks and keys, then `camera_tick` for each 4 of them: the turn, the move, the walls (`add_wall`, `add_post`), the resolve, the slide (`slide`), the posts (`corner_deflect`), the transition | done: from each place 32 frames of made-up keys and 1 to 4 ticks, and 32 frames into its corner, the camera after each summed by place: `portalc_cameraref` over the demo scene, `portalc_camerastressref` over the stress scene (whose wedge blocks a slide for three rounds). The camera's state is camera.6502's |
| `side_across`, `foot_in_range` | `wall_distance`, `wall_foot`, through `vertex_dot` and `dot_direction` | done, in the tick's checks |
| `corner_deflect`, with `mulu16x8_lo` | `corner_deflect`, with `quartered` | done, in the tick's checks |
| `apply` | `shift_camera` | done, in the tick's checks |

### The capture (render.c)

| C | 6502 | State |
|---|---|---|
| `pt_render_capture` | `capture` (walk.6502): `capture_root`, `walk`, `forget_visits` | done, the sprites' instances too: `portalc_walkref`, whole captures from the 146 places at 32 angles each; and `portalc_walk3ref`, the stress scene's 106 places, the depth capped at 3 |
| `pt_render_root`, `frame`'s setup | `capture_root` (capture.6502) | done: `portalc_captureref`, from 146 places, each cell's centroid and near each of its corners |
| `pt_render_root_side`, `root_screen_angle` | inline in `capture_root` | done |
| `root_walk` | inline in `capture_root` | done |
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
| `pt_mode5_zero`, and `pt_mode5_down`, `_up` for the unplot | `UNPLOT_ROW`, `UNPLOT_MOVE` | done, in the unplot's checks |
| `pt_mode5_plot` | — | not ported: only `close_columns` uses it (step 4), and it never ran in the sweep |
| `inst_new`, `inst_clip`, `inst_hide` | `instance_new`, `instance_clip`, `instance_hide` (instance.6502); `PLOTTED_SIZE` (instance_macros.6502), `inst_new`'s `pw` and `ph` | done: each frame's instances in the walks' checks (each column's top, and its bottom where the top isn't 0: a hidden column's bottom is the pool's leftovers, never read); and 1,024 cases of 3 to 6 made-up instances in the capture's, `portalc_captureref`, for the paths the walk never takes (a floor level with the eye or above it, the centre's offset saturating, the draw order, the pool full, a hide over no columns); and a near sprite on a floor far below the eye, its offset past 255 rows |

### After the capture

| C | 6502 | State |
|---|---|---|
| `pt_mode5_fill` (mode5.c), with `frame`'s start colour | `fill` (fill.6502), the camera cell's ceiling's colour, then on into `fill_colour`: `FILL_COLUMNS` (fill_macros.6502) through each screen's blocks, `FILL_BODY` and `FILL_TAIL` | done: 64 made-up frames, every end into each screen, half from a cell's ceiling, `portalc_fillref` |
| `pt_sprite_plot` (sprite.c): `setup` | `sprite_plot` (sprite.6502): the row edges in three parts (under the screen, on it, over it), the column walk to the first column on the screen, the strips' pairs the sprite's (a whole sprite's are never classed), then the plotter by the art's crossover, on into the engine | done: 1,024 made-up sprites, `portalc_spriteref`: a sixteenth to 16x, every other under 1x (an eighth of those 1x itself, a quarter at the right edge), those over 4x far off the screen one way (left, right, or over it, past 255 pairs); eight kinds of clip, held from varied first columns; eight arts (the demo scene's, the stress scene's, five made up and one made for swallowing) |
| `pt_sprite_plot_engine`: `assign`, `strip_engine`, `strip_engine_clip`, `fetch`, `nearest`, `nearest_event`, `stretch` | `sprite_engine`: the strip's slots, the clean strip's runs under the screen, the clipped strip's events (each slot's next, `NEAREST_EVENT`), the stretch and `events_due`; the handlers `change1` to `change4` and `swallow1` to `swallow4`, from `CHANGES` and `CHANGE_COLUMN` (sprite_macros.6502) | done, in the plotters' check: every instruction run and each branch both ways, counted on the cycle-exact model |
| `pt_sprite_plot_rebuild`: `strip_rebuild`, `strip_rebuild_clip`, `afetch`, `dummy`, `rebuild_pair` | `sprite_rebuild`; `REBUILD_FETCH`, `REBUILD_DUMMY`, and `REBUILD_PAIR`, inline in each strip's pair loop | done, in the plotters' check: every instruction run and each branch both ways |
| `classify`, `findcol` | `strip_window`, `find_column` | done, in the plotters' check |
| the sprite pass in `frame` | `sprites` (sprites.6502): each instance in the draw order to `sprite_plot`, whole if its visits showed every column, all of it (`whole && cols == width`) | done: whole frames (the capture, the fill, the sprites, the unplot) from the walk's places at 32 angles each, the screens summed by place: `portalc_frameref` over the demo scene, and `portalc_frame3ref` over the stress scene, the depth capped at 3; and the capture's made-up instance cases drawn, for sprites overlapping (`pt_render_sprites`, `portalc_captureref`) |
| `unplot` (render.c), with `pt_render_unplot` for the checks | `unplot` (unplot.6502), each record through `unplot_flat`, `unplot_shallow` or `unplot_steep`, then row 0 | done: the plotters' made-up runs cleared at each gradient, `portalc_plotref`; and each place's first frame in the walks' checks |
| `pt_unplot_shallow`, `pt_unplot_steep` (plot.c), with `unpart_`, `unbyte_` | `UNPLOT_SHALLOW`, `UNPLOT_STEEP` (unplot_macros.6502), with `UNPLOT_PART` and `UNPLOT_BYTES` | done, in the unplot's checks |

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
| `pt_camera.settled` | `camsettled` |
| `pt_camera_tick`'s `mx`, `my` | `move_x`, `move_y` |
| `cands[].v`, `.angle`, `.capped`, `.len` | `candidate_vertex`, `candidate_anglelo`/`hi` (bit 7 of `hi` capped), `candidate_lengthlo`/`hi` |
| `cur_y`, saturated to a byte | `candidate_distance` (`NOT_MADE` its `INT8_MIN`) |
| `posts` | `post_vertex` |
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
| `visits[i].inst` | `visit_instance` |
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
| `pt_mode5_zero`; the unplot's down and up | `UNPLOT_ROW`; `UNPLOT_MOVE` |
| `pt_rec.x0`, `.x1`, `.y`, `.g` | `rec_x0`, `rec_x1`, `rec_ylo`/`rec_yhi`, `rec_gradlo`/`rec_gradhi` |
| record's `toggle_end` update | `NOTE_TOGGLE` (A the row, X the byte column) |
| the plotters' `gm` | `grad`; and `grad2`, `grad3`, `grad4`, `grad_less1` |
| `pt_plot_steep`'s `gi`, `gf` | `grad_whole`; `gf` is `line_grad`'s low byte itself (`STEEP`'s `grad_fraction`) |
| `f` | `fraction` (rising, 255 less it): `run_y` itself |

### The sprites' instances

| C | 6502 |
|---|---|
| `pt_inst.sprite`, `.place` (`scale`, `x`, `y`) | `instance_sprite`, `instance_scalelo`/`hi`, `instance_xlo`/`xhi`, `instance_ylo`/`yhi` |
| `pt_inst.ld` | `instance_logdepthlo`, `instance_logdepthhi` |
| `pt_inst.cx0`, `.cx1`, `.p0`, `.p1` | `instance_x0`, `instance_x1`, `instance_pair0`, `instance_pair1` |
| `pt_inst.clip`, `.cols`, `.whole` | `instance_cliplo`/`hi`, `instance_columns`, `instance_whole` (1 or 0) |
| `num_insts`, `order`, `num_order` | `num_instances`, `draw_order`, `num_order` |
| `clipmem`, `clip_used` | `clip_rows`, `clip_used` |

### The sprite plotters

| C | 6502 |
|---|---|
| `pt_sprite_place.scale`, `.x`, `.y` | `sprite_plot.scale`, `.x`, `.y` |
| `pt_sprite_clip.lo`, `.hi`, `.x0`, `.whole` | `sprite_plot.bottoms`, `.tops`, `.x0`, `.whole`; no `x1` |
| `pt_sprite_setup.spr`, `.pal` | `art_data`; `art_colours`, the art's place in `art_pate` and `art_pato` (art * 8) |
| `.wid`, `.hgt` | `art_end` (the width + 3: past the last column's index byte), `sprite_plot.height` |
| `.py`, `.ph`, `.k0x8` | `row_edge` (in `Between`), `top_pair`, `under8` |
| `.sx0`, `.strips` | `first_pixel`, `strips_left` |
| `.c0`, `.rem0`, `.xf0`, and `pt_sprite_walk.c`, `.rem`, `.xf` | `source_column` (the column + 3: its index byte's place), `source_pixels`, `source_edge` (the edge's fraction): one walk, from the setup on |
| `pt_sprite_walk.slot` | `sprite_engine.slot`; the rebuild's `lead`, its first strip's padding |
| `pt_sprite_win.lo`, `.hi`, `.start`, `.end` | `window_bottom`, `window_top`, `strip_start`, `strip_end` |
| `pt_sprite_track.idx`, `.cp`, `.colmask` | `track_next`, `track_end`, `track_slots` |
| `pt_sprite_engine.n`, `.own` | `sprite_engine.tracked` (one less), `sprite_engine.owner` |
| `pt_sprite_engine.cnt`, `.mask`, `.de`, `.dodd` | `strip_pair`, `strip_mask`, `strip_even`, `strip_odd` |
| — | `strip_uncovered`: the slots the first tracked column doesn't show, the one-column handlers' mask |
| `strip_engine_clip`'s `ev`, `nextev`, `slot` | `sprite_engine.event_at`, `next_event`, `sprite_engine.next_slot` |
| `pt_sprite_slot.idx`, `.ev`, `.ce`, `.co`, `.cm` | `slot_next`, `slot_event`, `slot_even`, `slot_odd`, `slot_mask` |
| `slotbits_` | `mode5_pixel` (plot.6502's) |
| `pt_mode5_at` for a pair | `SCREEN_AT`: pair p's odd scanline is the column's s = 127 - 2p, at s AND 7 in block `shr3`[s] |
| a run byte's row, `b >> 3` | `shr3` (in the tables) |

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
| `sprites[i].v`, `.art`, `.ls` | `sprite_v`, `sprite_art`, `sprite_lslo`, `sprite_lshi`: the base's centre a vertex of its own, so the vertex cache sees it (in the C too) |
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
| — | `shr3`: b DIV 8, a sprite run byte's row (and `SCREEN_AT`'s block) |
| — | `fill_exitlo_a` to `fill_tailhi_b`: the fill's, by a column's end row, where each screen's body takes the RTS and where its tail starts |

### Constants

`PT_` comes off: `PT_SCREEN_WIDTH` is `SCREEN_WIDTH`, `PT_CAM_HEIGHT` is
`CAM_HEIGHT`, `PT_IN_FWD` is `IN_FWD`, `PT_MAX_VCACHE` is `MAX_VCACHE`,
`PT_MAX_RECS` is `MAX_RECS`, `PT_MAX_DEPTH` is `MAX_DEPTH`, `PT_MAX_KEPT` is
`MAX_KEPT`, `PT_MAX_SPRITES` is `MAX_SPRITES`, `PT_CLIP_POOL` is `CLIP_POOL`,
`PT_SPRITE_NEAR_L` is `SPRITE_NEAR_L`; and `PT_MAX_INSTS`, in whole words, is
`MAX_INSTANCES`. `PT_SIN_ONE` is `SIN_ONE`, worked out in arith_macros.6502
from the sine itself.
They're all in constants.6502. `POST_NEAR` and `POST_SCALE` name numbers the
C writes out: 256 and 1.25 * 2^15, in arith.h's notes on `rpost`.

## Where the 6502 differs

- **The facing is in 1024ths.** `camangle` holds what the C calls `cam4`.
  The C's camera keeps 256ths, and so does the map, so `entry` multiplies
  `CAM_START_ANGLE` by 4.
- **The vertex cache keeps no position.** Whatever moves the camera has to
  call `vcache_reset`, as the C's camera now does too; only the C's
  `pt_render_frame`, for its probes, compares positions (`pt_vcache_move`).
- **`vcache_locate` doesn't count its hops.** The C asserts 8 at most (a
  ring of a fan); a camera's move crosses a cell or two.
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
- **The fill goes through each screen's own unrolled blocks** (mode5.c's
  notes). The body EORs a canvas row's byte from T into the colour and
  stores it, rows 0 to 63, X the column's block (one on for its odd
  scanlines); an RTS patched over the column's end row's `EOR` stops it, and
  the tail stores the colour from there down. Tables by the end row say
  where the RTS and the tail go: ends 64 and 65 stop at the body's own RTS,
  so they patch `fill_spare`, and the tail starts at row 1, as no end is 0.
  `make_tables` steps them through the blocks, in builds with the fill.
  A byte above a column's end costs 9 cycles, one below 5, and a column 134
  more: 32.6k cycles a frame over walkref's frames.
- **The unplot holds its 0 in A.** Its row moves (`UNPLOT_MOVE`) step Y and
  the cursor without `mode5_down` or `mode5_up`, so A keeps the 0 it stores
  while X holds a shallow run's fraction. Each kind of record has its
  routine, which reads the record itself (X its number). The C asserts T's
  screen is clear after; the checks find its junk rows clear too. It costs
  5.7k cycles a frame over the sweep's frames, 4.5k of it shallow runs.
- **The interrupt gathers the keys.** It ORs the keys held at each vsync
  into `keys` and counts the vsyncs in `framectr`; `update_camera` takes both
  with the vsync held off and runs them through `camera_tick` 4 at a time
  (`CAM_MAX_TICKS`), the keys shared, as main.c does: 4 ticks at 3 units
  keep a call's move under 17 units, as `FAR_SLACK` needs. A frame of more
  than 4 vsyncs takes more calls, each a whole tick's cost.
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
- **The walk leaves out the C's checks** on side counts, a visit's columns
  running left to right, and the window's range.
- **`instance_new` leaves out two of the C's cases** that can't happen: the
  log of the centre's offset under 0 (it's at least 14 * 256 less the
  largest lsec, 1,447), and the floor's offset's under 0 (at least 17 * 256,
  the floor's least, less 12 * 256, the deepest). Its exps take their tight
  bounds: the scale's `EXP2_16 13` (the map's sprites asserted to fit), the
  far floor offset's rows `EXP2_16 12`, and `exp2_sat` for the centre's
  offset, which saturates as the C's does, and the near floor offset. The
  draw order compares depths' logs unsigned, as each is over
  `SPRITE_NEAR_L`.
- **The one-column handlers fetch unasked.** As spritescale's do, `change1`
  and `swallow1` take the column's next run whenever they're entered, and
  set the data to its pattern under the column's slots and the mask to
  `strip_uncovered`, with no merges. A transparent run sets the mask to &FF
  and leaves the data: under &FF nothing writes it, a clip event then can
  only blank a slot, and the next fetch writes it whole.
  An event between the column's ends would have them fetch early, so
  `events_due` goes straight to the stretch when there's one column and its
  run doesn't end at the pair. The C tests each column's end every time.
- **The rebuild skips two tests it can't need**: the padding at the last
  slot (it's 3 slots at most), and the columns running out at the first (no
  strip begins once they have).
- **`sprite_plot` has no `x1`.** It reads the clip for each covered slot,
  from the sprite's first column on the screen to its last; the caller
  holds the clip for every column the sprite covers, as the C asserts.
- **A check with the capture has the instances and the clips too**, as
  `capture_root` makes the root's instance, and `instance_clip` reads the
  bounds.
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
- ~~Then `update_camera`'s turn keys. That's the first demo.~~ Done: it runs.
  Over the sweep's frames (the cycle-exact model; the fill from its
  `toggle_end`) a frame is 66.9k cycles on average: capture 29.2k, fill
  32.6k, unplot 5.7k. A quarter of the frames take one vsync's 40k, 44% two,
  and 32% three or more (up to 165k); those average a 55.5k capture, a
  36.6k fill and a 10.9k unplot. Turning on the spot at the start, 66% of
  the angles take two vsyncs or fewer.
- Sprites, in three pieces, each with its check:
  1. ~~The instances in the capture: `inst_new`, `inst_clip`, `inst_hide` and
     the draw order.~~ Done.
  2. ~~The plotters: spritescale's engine and rebuild, as sprite.c clips
     them.~~ Done, and gone over once for speed: a review against
     spritescale, all of it taken bar a second copy of the stretch loops.
     On the cycle-exact model the demo's boulder (14 by 16), whole and
     mid-screen, now takes 3.9k cycles at a quarter size, 17.3k at 1x, 22.4k
     at 2x, 35.3k at 4x and 68.4k at 8x (112 by 64), 6 to 16% less than
     before. Classed against a full clip it's 10 to 19% more, and a ragged
     clip (bottoms and tops changing column to column) up to 1.75 times as
     much. Against spritescale itself (Baron's example, its boulder and
     creature whole, the same pixels drawn) it's 10 to 21% faster under 1x,
     2 to 3% from 1x to 3x, and 8% at 4x. The arts' runs set the price: at
     1x half the boulder's time goes on its runs' changes, and at a quarter
     size on runs passed as they shrink away.

     The crossover isn't spritescale's density rule any more (320 times the
     run ends over the groups' rows), which kept the creature off the
     rebuild from 0.79x though it was cheaper all the way to 1x, and put
     narrow arts on the rebuild that the engine beat at every scale.
     `crossover` (sprite.c) counts what each plotter does at every scale
     under 1x and prices it: over 259 arts its pick costs 0.03% more than
     the best, where the old rule's cost 1 to 2.2% more, and up to 22% on
     a narrow art.

     The code is 4,319 bytes. `shr3` is made by `make_tables`, the tables a
     page lower to fit (&3A00), and `MainCode` has 3,991 bytes left.
     `mode5_pixel` is plot.6502's; the sprite check has a copy, as it
     doesn't assemble plot.6502.
  3. ~~`sprites` in the frame, and a whole-frame check.~~ Done. Over the
     frame check's 4,672 frames (the walk's places at 32 angles each) the
     pass costs 2.3k cycles a frame on average: 22.3k in the one frame in
     ten that shows the boulder, and 50.9k at most. The demo's start view
     is the C's byte for byte, boulder and all. Neither scene has two
     sprites in one frame, so the capture's check draws its made-up
     instance cases too (3 to 6 boulders a case, 1,828 of its 2,794 in an
     order of more than one): drawn near to far, 47 of its 64 groups'
     screens differ. Neither scene has a second art, so nothing checks
     which art `sprites` picks.
- Moving, in steps, each checked against the C:
  1. ~~The C: a frame's ticks as one (`pt_camera_tick`'s `ticks`, up to
     `PT_CAM_MAX_TICKS`), sharing its keys, the turn and the move that many
     ticks' worth; 1 unit a tick, so a frame's move stays under 12 units
     and nothing's capped; the vertex cache emptied whenever the camera
     moves, the whole units compared or not.~~ Done.
  2. ~~The arithmetic: `pt_mul_sin` and `pt_mul_cos` (a byte magnitude where
     the C's is a whole number of units shifted up, as the move's and the
     pushes' are), `pt_dot_dir`, `pt_mulu16_frac`, `pt_rpost`.~~ Done:
     macros, which the tick will call through routines, as it's not hot.
     The turn's 4 1024ths a tick is the C's too now (`PT_CAM_TURN_SPEED`
     1): slower on purpose.
  3. ~~`pt_vcache_locate` and the transition. No `side_v1` table: a side's
     end comes from the ring.~~ Done: `vcache_locate`; the transition is
     `vcache_reset` then it, which the tick calls when it moved.
  4. ~~The tick: the move, the candidates and posts, the resolve, the slide,
     the corners. Not a hot path (once a frame), so widths as narrow as
     consistency allows, the C changed to match where the 6502 narrows.~~
     Done: move.6502, 2.4K, which leaves sideways RAM 330 bytes; the
     camera's state stays in camera.6502, which every check takes. The
     slide's distances from its start are a byte, saturated (in the C too).
     Every path runs in the two checks together, bar the sine's 1 in two of
     the macros' expansions, which no wall or facing in either map meets
     (`test_arith` takes every angle). Two breaks can't show: a
     neighbour's wall from behind with the camera's foot on it (a solid wall
     between two cells joined round a corner, which neither map has: the
     foot test turns away every other), and the saturation's exact 128s
     (no wall's ever that far, and one 128 off can't be met). On the
     cycle-exact model a tick is 8k cycles on average over the made-up
     frames, 11k to 13k pressing into corners, 33k at most; the transition's
     polar forms are ones the capture would make anyway.
  5. ~~`update_camera` and the demo, with frame times while moving.~~ Done:
     it walks in jsbeeb at a unit a vsync. Timed on the cycle-exact model
     from a jsbeeb snapshot at `.frame`, the driver doing the irq's work
     (vsyncs every 40,000 cycles, 230 for the irq), over 2,734 vsyncs of
     scripted keys (the corridor's length to the wall in cell 8, then
     turning, backing off, strafing and going diagonally): 1,323 frames
     shown, 24.2 fps on average, 8.3 at worst (six vsyncs) and 50 at best.
     31% take one vsync, 43% two, 18% three, 9% four or more. A frame's
     work is 81.1k cycles on average: the tick 6.8k, capture 32.6k, fill
     33.5k, sprites 2.6k, unplot 5.5k. The worst is 217k, walking into the
     pit at the boulder: 89k of sprites and 71k to 79k of capture. Standing
     at the start it's 12.9 fps (154k: capture 74k, fill 39k, sprites 29k,
     unplot 12k); walking from there the capture's 75.4k, so emptying the
     vertex cache every frame costs it little. If sideways RAM runs out,
     `sprites.6502` or `capture.6502` can move to main RAM.
  6. ~~Twice the speed: 2 units a tick.~~ Done. A call takes 4 ticks at
     most now (`PT_CAM_MAX_TICKS`, `CAM_MAX_TICKS`), so its move stays under
     12 units and `FAR_SLACK` holds (camera.c asserts it). A frame's ticks
     past 4 take more calls, in main.c and `update_camera` both, the keys
     shared; main.c no longer drops a stall's ticks past the cap, but runs
     up to 255 of them, the Beeb's byte. The checks' made-up frames take 1
     to 4 ticks, the corners 4. A move exactly tangent to a post
     (`corner_deflect`'s zero magnitude) no longer comes up in either check:
     it's a shortcut, the correction 0 either way.
  7. ~~3 units a tick.~~ Done. `FAR_SLACK` is 21 now (camera.c and
     move.6502), for a call's move under 17 units (4 ticks at 3, a strafe's
     diagonal 16.97), which camera.c asserts. `corner_deflect`'s bounds hold
     at 17: its products fit 16 bits to about 24 units a part, and `f` stays
     under 1.33 in q14. The move's 3 a tick and the turn's 4 are an
     `ASL:ADC` and an `ADC`. The camera can end a move up to 17 units inside
     its radius, near a wall's end (judged from where the move starts, and
     never pushed back out) or a neighbour's wall (for a tick):
     `probe_collision` saw 22.74 at closest, and its tripwire came down from
     28 to 21. Its 29.17 before was the same thing at 11.3 units, not drift.
- ~~The fill's tables, made by the loader.~~ Done: 528 bytes that were
  `EQUB`s in sideways RAM, where any code before them could push them onto
  new pages (the tick's loop cost 104 bytes of padding). `make_tables`
  steps them through the fill's blocks (`STEPPED`), and `test_tables`
  checks them against the blocks' labels. `TABLES_ORG` came down three
  pages to &3700 to fit them: sideways RAM has 910 bytes left, `MainCode`
  3,321 and `Tables` 124. On the way: a check now stops in `test`, not
  back in the loader, which the T-clearing checks wipe (the fill check's
  return landed on a `BRK` once the loader moved).
- To revisit: the camera ending a move inside its radius, by up to the
  move's length, near a wall's end or a neighbour's wall (Moving step 7). A
  bigger radius gets some back: 44 at most while `rpost` takes d2 / 8 as a
  byte (and `trowhi` would need a `FITPAGE`), about 27 at closest. Judging
  posts and neighbours' walls from the radius plus the move, and a post's
  approach along it, gets it all back, for work on every moving tick.
- To revisit, if the cycles are wanted: two copies of the stretch loops,
  one going on to the handler and one, for the stretch to a clip event or
  the strip's end, to `events_due`. The handler's `CMP next_event` then
  picks the copy, and `.stretched` goes: 8 to 11 cycles a stretch, 2.8% of
  the boulder at 1x, for 194 bytes. Left out for the bytes.
- To revisit when the plotters change: `crossover`'s prices. They were
  fitted to these plotters by timing both, forced (the art's crossover byte
  0 or 255), at every eighth scale under 1x for 159 arts on the cycle-exact
  model, and least-squaring `work`'s counts against the cycles.
- To revisit: the capture's check steps its LFSR once a value (`LFSR`,
  `UP_TO_64`), so each value is the last one shifted a bit. Stepping a byte
  a value, as the sprite check does, would spread its cases; all its
  answers change, so it waits for a reason to touch that check.
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
