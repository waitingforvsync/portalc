# Unit-replay records

The clip group and the walk's insides get checked one call at a time
(PLAN.md §5.6): the C records a call's inputs and outputs, the Beeb is
handed the inputs, and its outputs had better match.

`make record` writes them, one file per function, to
`build/golden/replay/<fn>.bin`, then replays every one on the host.

## How they're made

The traced build (`portalc_beebtool_trace`) compiles a copy of `render.c`
that `host/trace.py` writes at build time. Each target is renamed
`NAME__impl`, and a wrapper takes its name:

```c
static void clip_line(pt_clip *cx, const pt_line *L, uint8_t st)
{
    tr_before_clip_line(cx, L, st);
    clip_line__impl(cx, L, st);
    tr_after_clip_line(cx, L, st);
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
same calls happen) and writes just those. About 3,100 records, 2.6 MB.

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
| 0 | `emit` | yes |
| 1 | `resolve_block` | yes |
| 2 | `clip_line` | yes |
| 3 | `clip_edge_line` | yes |
| 4 | `cut` | yes |
| 5 | `inst_new` | yes |
| 6 | `seg_record` | yes |
| 7 | `inst_issue` | yes |
| 8 | `child_portal` | yes |
| 9 | `next_open` | no |
| 10 | `close_columns` | yes |
| 11 | `process_edge` | yes |
| 12 | `make_line` | no |
| 13 | `edge_line` | yes |
| 14 | `transform` | no |
| 15 | `col_gt` | no |
| 16 | `col_ge` | no |

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

**LINE**, a `pt_line` (7): `x0`, `x1`, `y0`, `g` u16, `ce`, `co`.

**EP**, a `pt_ep` (7): `x` u16, `y` s16, `sx`, `rx.f`, `rx.e`.

**EL**, a `pt_edgelines` (34): `v0` VV, `v1` VV, `sx0`, `sx1`, `have_ep`,
`e0` EP, `e1` EP, and the visit's index.

**PORTAL**, a `pt_portal` (7): `cell`, `x`, `xr`, `tcur`, `bcur`, `tmax`,
`bmin`.

**CLOSED**, a `pt_closedl` (17): `l[8]`, `r[8]`, `num`.

**CX**, a `pt_clip` with its pointers resolved (35):

```
thead bhead tmax bmin role
has_build  build.head build.tail build.tmax build.bmin
cbfb.e cbfb.o
has_closed CLOSED
vis        (the visit's index; &FF for none)
slot tp bp_set bp
```

A missing builder or closed list is a 0 flag, and zeros after it.

**FS**, the frame state: everything the walk builds, counts first, entry
0 of each table included.

```
pool_num u16    pool_num pieces:    x0 x1 y0 g(u16) ce co next     (8 each)
num_visits      num_visits visits:  cell k xr VV VV inst active[4] (24 each)
cell_visit[24]
num_insts       num_insts insts:    sprite cx0 cx1 seg seg_tail prev next
                                    scale(u16) left(s32) top(s32)  (17 each)
order_head order_tail
seg_num u16     segs[seg_num]
num_pend        pend[num_pend]
```

An instance's sprite-pass fields (`rows[]` onwards) aren't there: nothing
in the walk touches them.

## Per function

| function | ARGS | RESULTS |
|---|---|---|
| `emit` | CX LINE r0 r1 st | CX |
| `resolve_block` | CX LINE c e | CX |
| `clip_line` | CX LINE st | CX |
| `clip_edge_line` | CX LINE h.m h.below xa xb | CX |
| `cut` | cursor xa xb | ret cursor |
| `inst_new` | cell | ret |
| `seg_record` | PORTAL | |
| `inst_issue` | pend | |
| `child_portal` | ei xl xr thead bhead tmax bmin VV | ret PORTAL |
| `next_open` | CLOSED c xb | ret c ol orr |
| `close_columns` | head l rr cbfb.e cbfb.o | |
| `process_edge` | PORTAL | ret PORTAL(p after) PORTAL(child) |
| `make_line` | EP EP h.m h.below col.e col.o | LINE |
| `edge_line` | EL slot h.m h.below col.e col.o | EL LINE |
| `transform` | w.x w.y (the camera from CAM) | VV |
| `col_gt` | VV c | ret |
| `col_ge` | VV c | ret |

Where the C leaves an output undefined, the record has zeros and the
comparison skips it. That's `child_portal`'s portal and `process_edge`'s
child when `ret` is false, and `next_open`'s `ol` and `orr` likewise.

## On the Beeb (Phases 5-7)

The Beeb keeps the same state in its own shapes, so the host does the
translating (`host/replay.py`, run by `b2.py phase5`):

- the pool as eight parallel pages, poked straight into BSS;
- the visits' active pieces as `vis_act`, slot × 24 + visit;
- `pool_last` (the C's `pool_num` − 1), the CX, the line and the
  arguments into a staging block (`rp_*`), which `test/s5.6502`'s shim
  unpacks into zero page (`cx_*`, `cb_*`, `l_*`, `ev_*`, `ep_*`, `fr_*`)
  before the call and packs back after. Staging rather than poking zero
  page keeps every write in code Baron can see.

The shim times the call alone (`now` round the `JSR`), so each record's
replay also gives its cycles. The host peeks everything back, re-encodes
it as above and compares the after half byte for byte. A 24-bit `left` or
`top` is sign-extended first (Phase 7).

The walk's targets (Phase 6) take the visits too:

- the visits as the Beeb's 19 tables of 24 (`spare.6502`: cell, k, xr,
  then v0's and v1's eight bytes each) and `cell_visit`, poked in place;
- `num_visits` staged, like `pool_last`;
- each visit's sprite instance, and the sprite state after `cell_visit`,
  taken from the C's after half until Phase 7 gives the Beeb sprites.

A record's map is its CAM's first byte, and the Beeb holds one map a
build, so each record replays on the build with its map (`harness0`,
`harness0_map7`, `harness0_map9`). Phase 5's targets never read the map,
which is why nobody noticed the records mix them.

`close_columns_synth.bin` isn't recorded: `beebtool synthclose` (the
trace build) writes it, from random bottom chains and column ranges, in
the same format. Its splits at `l` and `rr` are the ones no golden pose
reaches.
