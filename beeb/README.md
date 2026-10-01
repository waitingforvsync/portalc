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
B2_VIDEO=offscreen make spike    # the recursion spike against its model
B2_VIDEO=offscreen make phase2   # check S2: every arithmetic routine against the C, and its cycles
make stop                     # stop our b2
```

The C side (`host/beebtool.c`, a unity build of the engine):

```
make goldens                  # the pose and input streams, in build/golden
make coverage                 # the coverage gate (host/uncovered.txt lists what's left, and why)
make search                   # the pose search: poses that reach code the goldens miss
make record                   # unit-replay records (REPLAY.md), each replayed on the host
make cost                     # the frame-cost model: estimated cycles per frame
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
| `harness.6502`, `tboot.6502` | the test harness (`HARNESS`), built with `-D TESTBUF=0` or `1`, and `-D MAP=0`, `7` or `9` |
| `src/top.6502` | the sections: screens, BSS, and the loader carrying the payload |
| `src/layout.6502` | the memory map and the mailbox |
| `src/stub.6502` | the loader's entry: takes the machine over, copies the payload to &300 |
| `src/tables.6502` | the fixed tables at &300-&EFF (Phase 1), the NMI handler at &D00 |
| `src/hw.6502` | the vsync IRQ with the buffer swap, the clock, palette, clears |
| `src/arith.6502` | the fixed-point arithmetic: quarter-square multiplies, reciprocal divisions, projections, rotation |
| `src/main.6502` | `start` and the main loop |
| `test/mailbox.6502` | the host's commands (test builds) |
| `test/spike.6502` | the recursion spike: walk's skeleton, checked against `host/spike.py` |
| `test/s2.6502` | check S2: drives each arithmetic routine over a counter or xorshift32, CRCs the results |
| `test/bss.6502` | the harness's own state |
| `host/b2up.sh`, `host/b2.py` | b2 start-up and automation |
| `host/beebtool.c` | the C side: exports, goldens, renders, the search, the recorder, the cost model |
| `host/trace.py`, `host/trace_hooks.c` | the traced copy of render.c and its hooks (`REPLAY.md`) |
| `host/coverage.sh`, `host/uncovered.py`, `host/uncovered.txt` | the coverage gate |
| `REPLAY.md` | the unit-replay record format |

## Memory map

| Range | Contents |
|---|---|
| &00-&FB | zero page, allocated by Baron (`ZA_POOL`) |
| &200-&205 | BRKV, IRQ1V; &27E-&29B never written; &2F0-&2FF the mailbox (test builds) |
| &300-&EFF | fixed tables (Phase 1); the NMI handler at &D00 |
| &F00-&4DFF | code and data |
| &4E00-&5CFF | per-frame state (in harness builds, inside the buffer not under test) |
| &5D00 | junk row above buffer A |
| &5E00-&6DFF | buffer A |
| &6E00 | junk row between A and B |
| &6F00-&7EFF | buffer B |
| &7F00 | junk row below B |

The disc's `PORTAL` loads at &1200 and its stub copies the payload down to &300.
Once the IRQ is installed no OS call is made, so all of &300 up is ours.
