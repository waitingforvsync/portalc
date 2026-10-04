# Unit-replay records

The capture gets checked one call at a time (PLAN.md §5.6): the C records
a call's inputs and outputs, the Beeb is handed the inputs, and its
outputs had better match.

`make record` writes them, one file per function, to
`build/golden/replay/<fn>.bin`, then replays every one on the host.

## How they're made

The traced build (`portalc_beebtool_trace`) compiles a copy of `render.c`
that `host/trace.py` writes at build time. Each target is renamed
`NAME__impl`, and a wrapper takes its name:

```c
static void clip_line(pt_render *r, pt_clip *cx, const pt_line *L, uint8_t c, uint8_t c1)
{
    tr_before_clip_line(r, cx, L, c, c1);
    clip_line__impl(r, cx, L, c, c1);
    tr_after_clip_line(r, cx, L, c, c1);
}
```

The engine source never changes. The hooks live in `host/trace_hooks.c`.

Recording walks every golden pose stream twice. The first pass picks the
calls worth keeping, per function:

- the first 64;
- every call that ran a basic block nothing had run before;
- every 1024th, for variety;
- 2000 at most.

The second pass renders the same poses again (it's deterministic, so the
same calls happen) and writes just those. About 5,000 records, 3 MB.

`replaycheck` then restores each record into a fresh renderer, runs the
`__impl` again and compares. Every record must reproduce itself, which
proves it holds everything the function reads.

## Files

Little-endian throughout. A file is an 8-byte header, then records until
the end:

```
"PCRP"  version (1)  function id  0  0
```

| id | function | frame state |
|---|---|---|
| 0 | `own_piece` | yes |
| 1 | `chain_add` | yes |
| 2 | `close_columns` | yes |
| 3 | `run_end` | yes |
| 4 | `merge` | yes |
| 5 | `flip` | yes |
| 6 | `hide` | yes |
| 7 | `meet` | yes |
| 8 | `clip_line` | yes |
| 9 | `merge_above` | yes |
| 10 | `inst_new` | yes |
| 11 | `seg_record` | yes |
| 12 | `inst_issue` | yes |
| 13 | `visit_of` | yes |
| 14 | `enter` | yes |
| 15 | `process_edge` | yes |
| 16 | `make_line` | no |
| 17 | `edge_line` | yes |
| 18 | `transform` | no |
| 19 | `col_gt` | no |
| 20 | `col_ge` | no |

A record is a u16 length, then two halves:

```
before:  CAM  ARGS     FS (if the function has frame state)
after:   FS (ditto)    RESULTS
```

To replay: set up the frame from CAM and FS, call with ARGS, then
encode FS and RESULTS the same way and compare with the after half.

## The pieces

**CAM** (9): the map (0, 7 or 9), `cam_x` u16, `cam_y` u16, `eye_z`,
`angle`, `cam_cell`, the sprites flag.

**VV**, a `pt_vv` (8): `x` s16, `y` s16, `sx`, `flags`, `rx.f`, `rx.e`.

**LINE**, a `pt_line` (9): `x0`, `x1`, `y` u16, `g` u16, `below`, `ce`,
`co`.

**EP**, a `pt_ep` (7): `x` u16, `y` s16, `sx`, `rx.f`, `rx.e`.

**EL**, a `pt_edgelines` (34): `v0` VV, `v1` VV, `sx0`, `sx1`, `have_ep`,
`e0` EP, `e1` EP, and the visit's index.

**CX**, a `pt_clip` (19):

```
tp bp tmax bmin vis slot shut child tail own lc ly(u16) cbfb.e cbfb.o rl rr ol orr
```

**FS**, the frame state: everything the walk builds, counts first, entry
0 of each table included.

```
pool_num u16    pool_num pieces:    x0 x1 y0 yf g(u16) ce co next          (9 each)
num_visits      num_visits visits:  cell k x xr tcur bcur tmax bmin
                                    VV VV inst active[4]                   (29 each)
cell_visit[32]
num_insts       num_insts insts:    sprite cx0 cx1 seg seg_tail prev next
                                    scale(u16) left(s32) top(s32)          (17 each)
order_head order_tail
seg_num u16     segs[seg_num]
num_pend        pend[num_pend]
```

An instance's sprite-pass fields (`rows[]` onwards) aren't there: nothing
in the walk touches them.

## Per function

| function | ARGS | RESULTS |
|---|---|---|
| `own_piece` | CX LINE r0 r1 st y(u16) | CX ret |
| `chain_add` | CX q r1 y(u16) | CX |
| `close_columns` | head l rr cbfb.e cbfb.o | |
| `run_end` | CX | CX |
| `merge` | CX LINE r0 r1 st own yl(u16) yp(u16) | CX |
| `flip` | CX LINE p c e st in yl(u16) yp(u16) | CX ret |
| `hide` | CX LINE c c1 st yl(u16) yp(u16) | CX ret |
| `meet` | CX LINE p c c1 st yl(u16) yp(u16) | CX ret |
| `clip_line` | CX LINE c c1 | CX |
| `merge_above` | CX LINE c c1 | CX |
| `inst_new` | cell | ret |
| `seg_record` | vi | |
| `inst_issue` | pend | |
| `visit_of` | cell | ret |
| `enter` | vi ei xl xr VV | ret |
| `process_edge` | vi | ret |
| `make_line` | EP EP h.m h.below col.e col.o | LINE |
| `edge_line` | EL slot h.m h.below col.e col.o | EL LINE |
| `transform` | w.x w.y (the camera from CAM) | VV |
| `col_gt` | VV c | ret |
| `col_ge` | VV c | ret |

The line's row cursor (`lc`, `ly`) is in the CX, so a call is compared
on it too: a function asking for the line's rows in a different order
would show, even with every row right.

## On the Beeb (Phases 5-7)

The Beeb keeps the same state in its own shapes, so the host does the
translating (`host/replay.py`, run by `b2.py phase5`, `phase6` and
`phase7`):

- the pool as nine pages, a field each (`p_x0` to `p_next`, wherever the
  memory map puts them, D13);
- the visits as 29 tables of `VISITS` (the map's cell count), a field
  each: `vs_cell` to `vs_inst`, then the active pieces' four tables end
  to end in `vs_act`, a slot each;
- `cell_visit` (the map's cells; the record's last entries are 0), the
  instances' tables, the segments and the pending stack;
- every table poked whole, zero past the record's entries, as the C
  replays into a fresh renderer, so a visit the call makes starts from
  zeros on both sides;
- the counts that live in zero page staged in `rp_pool` (`pool_last`, the
  C's `pool_num` - 1) and `rp_nvis` (`num_visits`, `num_insts`, the
  order's head and tail, `seg_next`, `num_pend`); the CAM in `rp_cam`;
  the ARGS, as the record has them, in `rp_buf`. `test/s5.6502`'s shim
  unpacks them into zero page (`cx_*`, `l_*`, `rg_*`, `rn_*`, `ev_*`,
  `ep_*`, `fr_*`) before the call and packs the RESULTS into `rp_res`
  after. Staging rather than poking zero page keeps every write in code
  Baron can see. A CX's visit and slot are `cx_act` on the Beeb (the
  slot's table plus the visit), and come back as they went in. `col_gt`
  and `col_ge` test a visit's end vertex in place, so their VV goes in as
  visit 0's (no real visit is 0).

The shim times the call alone (`now` round the `JSR`), so each record's
replay also gives its cycles. The host peeks everything back, re-encodes
it as above and compares the after half byte for byte. A 24-bit `left` or
`top` is sign-extended first.

A record's map is its CAM's first byte, and the Beeb holds one map a
build, so each record replays on the build with its map (`harness0`,
`harness0_map7`, `harness0_map9`).

## Synthetic records

`close_columns_synth.bin` and `run_end_synth.bin` aren't recorded:
`beebtool synthclose` (the trace build) writes them, from random bottom
chains and column ranges, in the same format. The first has the splits at
`l` and `rr` that no golden pose reaches; the second a bound line's open
run, taken as the child or, after a first one, closed after the fact,
which only a pinched opening would do.
