# portalc on the BBC Micro

The 6502 port of portalc, for a 32K Model B, in [Baron](../../baron). The C in
`../src` is the reference: for the same pose, the port's frame buffer must be
byte-identical to `pt_render_frame`'s. The plan is `PLAN.md`; progress is in
`JOURNAL.md`.

## Build and run

```
make                          # the release disc and the harness discs
make run                      # start b2 (if ours isn't running) and boot the release disc
B2_VIDEO=offscreen make phase0   # the Phase 0 checks, with no b2 window
B2_VIDEO=offscreen make phase1   # the Phase 1 checks: image, tables, maps
B2_VIDEO=offscreen make phase2   # check S2: every arithmetic routine against the C, and its cycles
B2_VIDEO=offscreen make phase3   # check S3: fill, clear and the cursor against the C, both buffers
B2_VIDEO=offscreen make phase4   # check S4: the plotters on every golden pose, both buffers
B2_VIDEO=offscreen make phase5   # check S5u (every recorded clip-group call replayed) and S5
B2_VIDEO=offscreen make phase6   # check S5u for the walk, and S6: whole frames, sprites off
B2_VIDEO=offscreen make phase7   # check S5u for the sprites, and S7: whole frames with them
make stop                     # stop our b2
```

The C side (`host/beebtool.c`, a unity build of the engine):

```
make goldens                  # the pose and input streams, in build/golden
make coverage                 # the coverage gate (host/uncovered.txt lists what's left, and why)
make search                   # the pose search: poses that reach code the goldens miss
make record                   # unit-replay records (REPLAY.md), each replayed on the host
make cost                     # the frame-cost model: estimated cycles per frame
make gonogo                   # predicted frame times from the measured costs (after make phase4)
```

b2 runs from its snap with its own config folder
(`~/snap/b2/common/portalc-b2`), the stock "B/Acorn 1770" Model B, unlimited
speed and no audio; `host/b2up.sh` starts it and records its pid in
`build/b2.pid`, and refuses if some other program holds b2's HTTP port.
`host/b2.py` drives it over that port (127.0.0.1:48075).

## Layout

| Path | What |
|---|---|
| `portal.6502`, `boot.6502` | the release program (`PORTAL`) and its `!BOOT` |
| `harness.6502`, `tboot.6502` | the test harness (`HARNESS`): the release code so far and one suite of checks, `-D SUITE=0` (S5 onwards; `harness0`, `harness1` and the map builds), `3` (S3 and S4: `harness_s34_0`, `_1`) or `2` (S2: `harness_s2`), with `-D TESTBUF=0` or `1` and `-D MAP=0`, `7` or `9` |
| `src/top.6502` | the sections: screens, BSS, and the loader carrying the payload |
| `src/layout.6502` | the memory map and the mailbox |
| `src/stub.6502` | the loader's entry: takes the machine over, copies the payload to &300 |
| `src/tables.6502` | the fixed tables at &300-&EFF (Phase 1), the NMI handler at &D00 |
| `src/hw.6502` | the vsync IRQ with the buffer swap, the clock, palette |
| `src/mode5.6502` | the buffers: the cursor macros, each buffer's clear and EOR fill |
| `src/plot.6502` | the plotters: flat, shallow (the carry tree) and steep, over the pool |
| `src/clip.6502` | pieces and chains, and the clip group: `emit`, `resolve_block`, `clip_line`, `clip_edge_line`, `cut` |
| `src/lines.6502` | `transform`, the column tests, the side clips, `make_line` and `edge_line` |
| `src/walk.6502` | the capture walk: visits, `child_portal`, `process_edge`, the root, `walk` and `capture` |
| `src/sprites.6502` | sprite instances, clip segments, the draw order and the sprite pass |
| `src/spare.6502` | six of the pool's pages, in the spare |
| `src/scratch.6502` | the capture's own state, in the junk rows |
| `src/arith.6502` | the fixed-point arithmetic: quarter-square multiplies, reciprocal divisions, projections, rotation |
| `src/main.6502` | `start` and the main loop |
| `test/mailbox.6502` | the host's commands (test builds) |
| `test/s2.6502` | check S2: drives each arithmetic routine over a counter or xorshift32, CRCs the results |
| `test/s3.6502` | check S3: fill, clear, and the cursor walked over every pixel pair of a buffer |
| `test/s4.6502` | check S4: plots the host's pools and CRCs each image |
| `test/s5.6502` | checks S5 and S5u: the replay shim, and every vertex transformed at every pose |
| `test/s6.6502` | check S6: whole frames at the host's poses, the capture and image CRCed |
| `test/crc.6502` | the CRC-16 tables the checks fold their results with |
| `test/bss.6502` | the harness's own state |
| `host/b2up.sh`, `host/b2.py` | b2 start-up and automation |
| `host/beebtool.c` | the C side: exports, goldens, renders, the search, the recorder, the cost model |
| `host/trace.py`, `host/trace_hooks.c` | the traced copy of render.c and its hooks (`REPLAY.md`) |
| `host/coverage.sh`, `host/uncovered.py`, `host/uncovered.txt` | the coverage gate |
| `host/gonogo.py` | the frame-rate go/no-go: predicted frame times from measured costs |
| `host/replay.py` | unit replay on the Beeb: records decoded, poked, called, compared (`REPLAY.md`) |
| `REPLAY.md` | the unit-replay record format |

## Memory map

| Range | Contents |
|---|---|
| &00-&FB | zero page, allocated by Baron (`ZA_POOL`) |
| &200-&205 | BRKV, IRQ1V; &27E-&29B never written; &2F0-&2FF the mailbox (test builds) |
| &300-&EFF | fixed tables (Phase 1); the NMI handler at &D00 |
| &F00-&50FF | code and data |
| &5100-&56FF | per-frame state that crosses the plot (in harness builds, inside the buffer not under test) |
| &5700 | junk row above buffer A; the capture's scratch until the plot |
| &5800-&67FF | buffer A |
| &6800 | junk row below A; the capture's scratch |
| &6900-&6EFF | spare: never plotted, filled or shown; six of the pool's pages |
| &6F00 | junk row above B; the capture's scratch |
| &7000-&7FFF | buffer B |
| &8000 | B's junk row below: the paged ROM, where stores go nowhere |

The disc's `PORTAL` loads at &1200 and its stub copies the payload down to &300.
Once the IRQ is installed no OS call is made, so all of &300 up is ours.
