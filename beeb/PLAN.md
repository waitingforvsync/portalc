# portalc on the BBC Model B: port plan

## Executive summary

1. The port lives in `portalc/beeb/` and targets a 32K Model B, written in Baron. The file loads at &1200, the loader stub copies it down to &300, it runs on b2 ("B/Acorn 1770"), and for every pose its frame must be byte-identical to `pt_render_frame`'s.
2. The port takes the machine over completely. It owns IRQ1V and enables only vsync (CA1). It makes no OS calls after the copy and uses zero page &00-&FB. Page-2 scratch avoids &27E-&29B, which survives BREAK. Esc exits through a power-on reset.
3. Runtime map: tables at &300-&EFF (`nrm[0]` doubles as an RTI at &D00), code at &F00-&4DFF, BSS at &4E00-&5CFF, and two 4K buffers at &5E00 and &6F00 that share a middle junk page. Headroom is about 3.2K at the size estimate and close to zero if code runs 25% over.
4. The screen set-up and IRQ are spritescale's, with the swap moved into the IRQ: it writes R12/R13 during blanking, so there is no tearing, and it counts vsyncs. Each frame, the camera's per-vsync movement and turn deltas are added N times (N = vsyncs since the last swap) and applied in **one** camera tick. The C gets the same entry point, so the camera stays exact.
5. Each stage is checked through a page-2 mailbox over b2's HTTP API. Results are diffed field by field against a C oracle (a unity build of the engine), then whole frames are compared with `sweep.bin` and the new goldens.
6. A coverage gate requires the goldens to execute every C line that is ported. The extra goldens are: searched poses, a deep-corridor stress map, the 7-sprite map's 4,688 frames, and camera input over all 64 key combinations. Any line still missed is listed with a reason.
7. The clip group and the walk internals get their own before/after state replay tests. Every buffer-writing check runs on both buffers. Harness builds carry the full release code, with BSS moved into the buffer not under test, and the integration test logs every frame.
8. The recursion keeps its 9 bytes per level on a depth-indexed software stack in page 1. A Phase 1 spike checks stack and zero-page behaviour before any clip code is written.
9. There are ten phases, taking about 35-51 working days to a verified, playable port. Memory checkpoints fall after Phases 2, 5 and 7, and a frame-rate go/no-go after Phase 4.
10. All decisions (D1-D11) are settled: Rich accepted every default, D3 became a C fix, and D4 is his camera design. Nothing is committed until Rich has reviewed it.

---

## 1. Decisions

### 1.1 Settled by Rich

- The target is a Model B with 32K.
- Baron nested sections: a `Loader` section at &1200 containing a `Code` section at `org=&300`. The loader's entry point copies `Code` down to &300 and jumps to it.
- The port lives at `portalc/beeb/`, and b2 is the emulator.
- Byte-exact means the same 4K buffer for the same `(x>>8, y>>8, angle, cell)` as `pt_render_frame`.
- A vsync IRQ counts frames between buffer swaps. The camera does **not** run N ticks: the per-vsync deltas (turn, and the facing step for the keys held) are added N times, and the sum is fed to a single camera tick (collision, slide, portal crossing). The per-vsync speeds will be retuned smaller for 50 Hz, so an N-frame sum stays small; whether it stays inside the collision's small-move assumption (camera.c's `|M| < 16` units) is checked once the speeds are set, not designed around now.
- The screen configuration and IRQ come from `~/dev/baron/examples/spritescale` (CRTC table, ULA &C4, palette `physcols`, VIA set-up, IRQ1V hook with CA1 ack), with the buffer swap moved from `swapbuffers` into the IRQ.
- Once the IRQ routine is installed the OS is off: no OS calls at all, so every byte from &300 upwards is ours (pages 0-2 are used as described in §2.1).

### 1.2 Decisions

Rich accepted every default below (D3 is resolved by a C fix, D4 is his design with the stated detail).

| # | Needed before | Question | My default |
|---|---|---|---|
| D1 | Phase 1 | **C host tooling.** (a) The oracle is a unity build that `#include`s every engine `.c` file and links only richc, so it can reach the static helpers and tables without a second copy of them. (b) `add_test_sprites` (static in `test_sprite.c`) moves to a small shared helper. (c) Unit replay intercepts static render functions through a copy of `render.c` generated at build time, with only the chosen definitions renamed, so there is no engine edit. The alternative is a compiled-out `PT_TRACE` hook in `render.c`. (d) The scratch frame-cost model (`cost/costrun.c`) moves into `beeb/host` so it can be recalibrated. | Yes to (a), (b) and (d). Generated copy for (c). |
| D2 | Phase 0 | The buffers share their middle junk page: &6E00 is the junk below A and the junk above B. This saves 256 bytes and relies on junk never being read, filled or displayed (spec §8.3). | Share it. |
| D3 | — | *Resolved:* the sprite quirk was a bug (a sprite whose centre left the view cone vanished instead of sliding off the screen edge). The C now forms the depth's reciprocal in `inst_new`, so there is nothing to copy; `transform` still stores `rx` only inside the frustum, and nothing else reads it outside. | Fixed in C (22 sweep frames changed). |
| D4 | Phase 8 | *Settled by Rich:* deltas added N times, one tick. Open detail: when turning while moving, is each of the N steps' facing step taken at the angle after that step's turn (the same path as N real ticks, minus collision per step), or all at the final angle? And does N keep main.c's clamp of 12? | Per step, turn then step, like N ticks would; clamp at 12. The C gets `pt_camera_step(c, m, input, n)`, identical to `pt_camera_tick` at n = 1, so the sweep and tests don't change. |
| D5 | Phase 0 | How to run b2 without a visible window: a visible or minimised window (works today), `SDL_VIDEODRIVER=offscreen` (untested), or Xvfb (needs `sudo apt install xvfb`). | Visible window. I try offscreen once. |
| D6 | Phase 1 | Are the generated `beeb/data/*.6502` files committed? | No. They are generated in the build and `beeb/data/` goes in `.gitignore`. |
| D7 | Any time | The Baron guide's relocation example (`docs/guide.md:262`) copies with `,X` where it needs `,Y`. Fix it in the baron repo? | Your call. It is out of scope here. |
| D8 | Phase 9 | How the tick count N is sampled. Measured between completed swaps, as you specified, N lags by two swap intervals (§4.2). Measured as vsyncs between main-loop tops, it lags by one interval. Both add up exactly to elapsed time. | Between swaps, as specified. |
| D9 | Phase 6 | What the release build does when a capacity limit overflows (pool, segments, toggles, pending, closed). The C panics at these points, so there is no reference. | Abort the frame: restore S, clear `cell_visit`, skip the swap so the previous frame stays on screen, and count the abort. Test builds TRAP instead. |
| D10 | End of Phase 1 | Ported lines that no golden input reaches, even after the pose search and the stress map. Accept them as verified by code review only, or build more stress inputs? | *Settled by Rich:* dead code is not ported. The rest is listed with reasons in `host/uncovered.txt`. |
| D11 | Phase 2 | Should the JOURNAL's Python 6502 interpreter be used alongside b2 to get exact cycle counts per routine? | No. Batch timing comes from T2 on b2; exact per-routine cycles, when wanted, from jsbeeb-mcp (§5.10). |
| D12 | Phase 2 | *Settled by Rich:* the arithmetic primitives (`mulu8` through `mul_recip16`, and the accumulator shifts) are macros, expanded inline on the caller's variables. The composites stay routines. This costs about 410 bytes over routines (2,241 against 1,831). | Macros, named after their C functions. |

**Process.** Each phase ends with a JOURNAL.md entry. The spec is updated on the schedule in §7. Nothing is committed until you have reviewed it.

---

## 2. Memory map

### 2.1 Runtime, release build (after the copy-down)

| Range | Bytes | Contents |
|---|---|---|
| &00-&FB | 252 | Zero page: `ZA_POOL &00..&FB`, the same in every build. Estimated use is about 100 bytes. |
| &FC-&FF | 4 | Reserved. The ROM IRQ entry saves A in &FC. The ROM's BRK path writes the BRK address+1 into &FD/&FE before doing anything else, so a crash dump always shows it. &FF is the OS Escape flag. |
| &100-&18F | 144 | Walk save stack: 9 arrays × 16 levels (`cell`, `x`, `xr`, `tcur`, `bcur`, `tmax`, `bmin`, `steps`, `pend`), indexed by depth. Confirmed by the Phase 1 spike (JOURNAL.md). |
| &190-&1FF | 112 | Hardware stack. The spike measured 48 bytes at depth 16 with a seven-call leaf chain (2 per level with `descend` inlined into `walk`, plus the chain), and the IRQ adds 3. Test builds keep a canary in &190-&1BF. |
| &200-&203 | 4 | Kept. BRKV (&202) points at our `trap` in every build. |
| &204-&205 | 2 | IRQ1V, pointing at `irq`. |
| &206-&27D | 120 | Scratch block 1: camera tick `cur_y` lo/hi (64) and `cands` (32). |
| &27E-&29B | 30 | **Never written.** OS 1.20 keeps this range across a soft reset (the soft-reset high-water mark is &27E). &287 is the BREAK intercept: a &4C there gets jumped to. &28C is the language ROM number. |
| &29C-&2EF | 84 | Scratch block 2: `posts` (32) and the rest of the tick scratch. Page 2 holds 204 bytes of scratch in all, against about 170 needed. |
| &2F0-&2FF | 16 | Mailbox in test builds. It overlaps the OSFILE block (&2EE-&2FF), so `start` zeroes it. Free in the release build. |
| &300-&AFF | 2048 | Quarter-square tables sq1lo, sq1hi, sq2lo, sq2hi: 512 bytes each, page-aligned, read via `(zp),Y`. |
| &B00-&BFF | 256 | `sin_` at offsets 0-127, then `recipf` at offsets 128-255. |
| &C00-&CFF | 256 | `k8` |
| &D00-&DFF | 256 | `nrm`. Entry 0 is never read, so it holds **&40 = RTI**: the ROM's NMI vector jumps straight to &0D00, which makes it a free NMI handler. |
| &E00-&EFF | 256 | `sbyte` |
| &F00-&4DFF | 16,128 | Code, map, art, shades, masks, IRQ, keyboard tables. `Code guard = BSS_BASE`. |
| &4E00-&55FF | 2048 | Piece pool as 8 parallel pages: `p_x0`, `p_x1`, `p_y0`, `p_glo`, `p_ghi`, `p_ce`, `p_co`, `p_next`. |
| &5600-&5CFF | 1792 | Visits (24 × 24), `cell_visit` (24), instances (8 × about 60), segment pool (256), toggles (96), pending (32), closed/chain/clip scratch (about 100). About 1,560 bytes in all. |
| &5D00-&5DFF | 256 | Junk row above A |
| &5E00-&6DFF | 4096 | **Buffer A.** R12 = &0B, R13 = &C0. |
| &6E00-&6EFF | 256 | Junk row shared by A and B (D2) |
| &6F00-&7EFF | 4096 | **Buffer B.** R12 = &0D, R13 = &E0. |
| &7F00-&7FFF | 256 | Junk row below B. The displayed MA stays below &1000, so the hardware wrap latch plays no part. |

`BSS_BASE = &4E00` is a single constant in `layout.6502` and moves by whole pages after Phase 7. Bss is a top-level section with no filename, so it never goes on disc.

### 2.2 Harness layouts

The harness is not bound by the release headroom. The release BSS region is given over to test code, and BSS moves into the buffer that is not under test.

| Build | Code guard | BSS | Buffer under test | I/O area |
|---|---|---|---|---|
| `harness -D TESTBUF=0` | &5D00 | &6F00-&7EFF (inside B) | A, plus junk &5D00 and &6E00 | From the end of Code to &5CFF, plus buffer A for stages that don't render |
| `harness -D TESTBUF=1` | &5D00 | &5E00-&6DFF (inside A) | B, plus junk &6E00 and &7F00 | From the end of Code to &5CFF, plus buffer B likewise |
| `portal -D SCRIPTED` / `-D PROFILE` | `BSS_BASE` | release | both | Log ring in the headroom below `BSS_BASE`, with a minimum size ASSERTed |

- Harness builds always contain **the whole release Code section implemented so far**. On top of that they add the mailbox, shims, TRAPs, a CRC-16 table and T2 timing, budgeted at about 2.5K and checked with an ASSERT.
- `ASSERT` checks that BSS fits in 4,096 bytes.

### 2.3 Load time

- **Payload.** &1200 to &1200+N−1 holds the image assembled for &300. N is at most &4B00 in the release build and at most &5A00 in harness builds.
- **Stub.** The stub follows the payload and is also the exec address (`exec=&FFFF0000+entry`). `guard=&7C00` on Loader keeps the MODE 7 screen intact.
- **The stub never writes screen memory before the copy.** A harness image can reach about &6C00, so the buffers are cleared by `start`, after the copy. The clear bodies are the main loop's own, so this costs no extra code.
- **The copy is safe.** Destination is &F00 below source, so an ascending page copy never overwrites unread bytes. The whole-page overrun (up to 255 bytes past the payload) lands in BSS or junk, which `start` zeroes or ignores. The stub is never overwritten.

### 2.4 Budget (release build)

| Region | Capacity | At the estimate | Code +25% |
|---|---|---|---|
| Fixed tables &300-&EFF | 3,072 | 3,072 | 3,072 |
| Code and data &F00-&4DFF | 16,128 | about 12,950 | about 15,900 |
| BSS &4E00-&5CFF | 3,840 | about 3,600 | same |
| Page 2 scratch | 204 | about 170 | same |
| **Headroom** | | **about 3.2K** | **about 0.2K** |

If it doesn't fit, these are the options, cheapest in speed first:
1. Keep one-shot set-up in the stub. This is already done for VIA, CRTC, ULA, the black palette and the vectors.
2. Size the instance array to the map: saves about 360 bytes.
3. One fill body with self-modified operand high bytes: saves about 400 bytes, costs about 0.5-1.3k cycles a frame.
4. Table-driven shallow plotter instead of the branch tree: saves about 650 bytes.
5. A 192-piece pool: saves 512 bytes. **This needs your OK**, because the Beeb would then fail earlier than the C.

---

## 3. Source layout, data and build

### 3.1 Tree

```
beeb/
  Makefile            data (runs the cmake build first), portal, harness0/1, check-*, run, sizes, coverage
  README.md           build, run, harness use, memory map (Phase 0)
  boot.6502           !BOOT: machine code at &400 (spritescale style), OSCLI "/PORTAL"
  tboot.6502          same, runs HARNESS
  portal.6502         release top level; -D SCRIPTED / -D PROFILE / -D HALT_AT_START variants
  harness.6502        full Code + test/ ; -D TESTBUF=0|1, -D MAP=0|7|S
  src/  layout stub tables hw arith mode5 plot pool clip lines walk sprites camera bss main (.6502)
  test/ mailbox.6502 (loop, dispatch, TRAP, canary, T2, CRC), shims.6502, replay.6502
  data/ (generated, ignored) map map7 stress art shades sine consts (.6502)
  host/
    CMakeLists.txt    added from the root CMakeLists: beebexport (links libportal),
                      beeboracle (unity build + generated traced render.c, links richc only),
                      a --coverage variant of the oracle
    beebexport.c      data/*.6502 and golden streams
    oracle.c          C reference for every stage, unit-replay wrappers, pose search
    costmodel.c       calibrated frame-cost model (D1)
    b2up.sh  b2.py  check.py
  build/              (ignored) .ssd .lst .json, raw files, golden/, fail-*/
```

- All runtime code sits in one `Code` section, spliced in with `INCLUDE`. The reason is that Baron's allocator doesn't follow fall-through across a section boundary, and `ZA_CANJUMP` targets must be in the same section.
- `ZA_ENTRY` goes on `start`, and on the mailbox loop in the harness. `ZA_INTERRUPT` goes on `irq`.

### 3.2 Top-level shape (portal.6502)

```
INCLUDE "src/layout.6502"                 ; BSS_ORG, CODE_GUARD chosen by IF on HARNESS/TESTBUF
ZA_POOL &00..&FB
SECTION Screens, org=&5D00                ; labels only, never saved
.junk0 SKIP &100 : .bufa SKIP &1000 : .junk1 SKIP &100 : .bufb SKIP &1000 : .junk2 SKIP &100
ENDSECTION
SECTION Bss, org=BSS_ORG, guard=BSS_ORG_END
  INCLUDE "src/bss.6502"
ENDSECTION
SECTION Loader, org=&1200, load=&FFFF1200, exec=&FFFF0000+entry, filename="PORTAL", guard=&7C00
.payload
  SECTION Code, org=&300, guard=CODE_GUARD
    INCLUDE "src/tables.6502"             ; ASSERTs: &300, &B00, &C00, &D00 (nrm[0]=&40), &E00
    INCLUDE "src/hw.6502" ... INCLUDE "src/main.6502"
  ENDSECTION
.entry
  INCLUDE "src/stub.6502"
ENDSECTION
```

### 3.3 Stub and `start`

**Stub** (the OS is alive until step 2):
1. `LDA #15 : LDX #0 : JSR &FFF4` (flush all buffers, which silences sound).
2. `SEI`.
3. Write &7F to &FE4E, &FE6E, &FE4D and &FE6D (both IERs and both IFRs).
4. DDRA=&7F (&FE43), DDRB=&0F (&FE42), latch &03 to &FE40 (auto-scan off), PCR=&04 (&FE4C).
5. All 16 palette entries black to &FE21, ULA control &C4 to &FE20, then CRTC R0-R13: 63, 32, 45, &24, 38, 0, 16, 26, 0, 7, 32, 8, then &0B, &C0. Whatever is in buffer A while the image is still there shows only as black.
6. IRQ1V = `irq` and BRKV = `trap`.
7. Copy and jump:
   ```
   LDY #0 : LDX #HI(entry-payload+&FF)
   .copy LDA payload,Y : STA &300,Y : INY : BNE copy
         INC copy+2 : INC copy+5 : DEX : BNE copy
         LDX #&FF : TXS : JMP start
   ```

**`start`:**
1. Zero &2F0-&2FF (the mailbox and OSFILE block), both page-2 scratch blocks, the walk save stack and BSS.
2. Clear both buffers and all three junk rows. In test builds, fill the stack canary.
3. Write the real palette (`physcols = {0,4,1,2}`, 16 bytes).
4. User VIA T2: ACR bit 5 = 0, load &FFFF, IER bit 5 off.
5. Initial state: front buffer A, back buffer B, `swapreq=0`, camera at the exported start pose in cell 2.
6. `LDA #&7F : STA &FE4D`, then `LDA #&82 : STA &FE4E`, then `CLI`.
7. The release build goes to `mainloop`. The harness writes the magic last, then enters the mailbox loop. A `HALT_AT_START` build spins here.

### 3.4 What the C exports (`portalc_beebexport beeb/data beeb/build/golden`)

The tool runs `pt_arith_init`, `pt_scene_init` and `end_map`, so twins, `angle` and `len8` are final. Every file carries the header "generated, do not edit".

- **`map.6502`:**
  - counts (45 verts, 22 cells, 91 edges, 2 sprites);
  - vertices `vx_lo`/`vx_hi`/`vy_lo`/`vy_hi`;
  - cells `c_first`/`c_num`/`c_floorz`/`c_ceilz`/`c_floor`/`c_ceil`/`c_sprite`;
  - edges `e_v0`/`e_adj`/`e_twin`/`e_colour`/`e_angle`/`e_len8`;
  - sprites `s_x`/`s_y`/`s_size` (lo and hi) and `s_art`.
- **`map7.6502`:** the same map with `add_test_sprites` (D1). Selected with `-D MAP=7`.
- **`stress.6502`:** a deep corridor of at least 18 thin cells in one line of sight, with its own start pose. It is built with the existing map builder and must pass `pt_map_validate`. It exists to reach the depth cap (16), `close_columns` and `closed_add` with no engine change. Selected with `-D MAP=S`.
- **`art.6502`, `shades.6502`, `sine.6502`:** `sin_` uses `lround(sin)`, so it is exported, never recomputed.
- **`consts.6502`:**
  - radius, speeds, `PT_CAM_HEIGHT`, `PT_SPRITE_NEAR`;
  - caps: depth 16, pool 256, closed 8, pend 32, togs 32, segs 256;
  - VF bits;
  - `PT_CAM_START_X`/`Y`/`ANGLE` and start cell 2, so `pt_map_find_cell` is never ported.
- **Integer tables** are Baron expressions using `DIV`, never `/`. `nrm[0]` is overridden to &40.

### 3.5 Golden streams (`beeb/build/golden/`, never versioned)

A pose record is 6 bytes: x u16 LE, y u16 LE, angle, cell.

| File | Contents |
|---|---|
| `sweep_poses.bin` | The sweep's 672 centroid poses and 500 walk poses. They re-render to `sweep.bin`. |
| `map7_poses.bin` | `test_sprite`'s 64-angle sets and its random walk: 2,344 frames of the seven-sprite map. |
| `rim_poses.bin` | The demo map's eye on every vertex and edge midpoint, facing along, across and at the rays of the edge, and every 16th angle: 6,372 frames. |
| `stress_poses.bin` | The stress map (map 9): a grid of points in every corridor cell and the room at 32 angles, then its rims. 10,520 frames: the depth cap, closed columns, the pinch, the wedge. |
| `search0_poses.bin`, `search7_poses.bin`, `search9_poses.bin` | The pose search's finds: poses that run a basic block nothing above runs. One so far (map 0: the root portal's exact retry). |
| `lcg_inputs.bin` | The sweep's LCG for 40,000 ticks: its first 4,000 are the sweep's walk, all of them `probe_collision`'s pattern. Run from both the demo map's and the stress map's starts. |
| `combo_inputs.bin` | 40,000 ticks drawn from all 64 input combinations, including `PT_IN_RIGHT`, which no existing generator ever presses. |
| `replay/<fn>.bin` | Unit-replay records (§5.6, `REPLAY.md`). |

The plan's `test_poses.bin` (the C tests' concrete poses) was dropped: the coverage gate shows nothing left for them to reach.

### 3.6 Representation rules that keep the port exact

- **`pool_last` replaces `pool_num`.** `pool_last` is the last used index, 0-255, where the C's `pool_num` runs 1-256 and can't fit a byte.
  - Allocate with `LDX pool_last : INX : BEQ full : STX pool_last`.
  - `plot` walks X from `pool_last` down to 1. Order doesn't matter, because EOR commutes.
  - `seg_last` works the same way. The oracle reports `num = last + 1`.
- **Frame-wide counters are `ZA_AUTO`** (Rich, Phase 1: no fixed addresses where Baron can allocate). `pool_last`, `seg_last`, `num_visits`, `num_insts`, `num_pend` and `depth` are set by the frame driver, outside the walk, and inside it the walk reads each before writing it, so Baron treats each as one running value however it's updated (`LDX pool_last : INX : STX pool_last`, `LDA seg_last : CLC : ADC #5 : STA seg_last`). A variable the walk writes before reading it, and still needs after a recursive call, is a per-level value, and Baron refuses it: that's what the page-1 save stack is for. (Baron learned the running-value rule during Phase 1: JOURNAL.md.)
- **`transform` stores `rx` only inside the frustum**, as the C does; `inst_new` forms its own reciprocal of the sprite's depth (D3), so nothing reads `rx` outside it.
- **Signed values.**
  - `>> 8` of negative values floors, as C's arithmetic shift does (render.c:827-828, 833, 1869). On the 6502 that means taking the high byte of a sign-kept 24-bit value.
  - `inst_new`, `col_next` and `rows[]` need 24-bit intermediates.
  - The camera x and y are u24, and the renderer is given `x>>8` and `y>>8`.
- **Reciprocal products are 16 bits and a carry**, in the C (arith.c's `prod17`) and on the Beeb: the bits in the caller's variable, the 17th in C. Each caller's asserts keep it clear, or it saturates, or the first right shift brings it in (JOURNAL.md, Phase 2). Nothing needs a 24-bit accumulator.
- **Asserted s16 fits** (for example `transform`'s a=x+y and b=x−y) are only exact while the asserts hold. So goldens come only from the Debug build (clang, asserts on), and test builds TRAP where the C asserts. The oracle self-tests `(-3)>>1 == -2`.
- **Baron:** `DIV`, never `/`. `>>` is logical, so use `FLOOR(x/2^n)`.
- **The walk.**
  - `descend` is inlined into `walk`: `depth ≥ 16` means `close_columns`, otherwise save, `INC depth : JSR walk : DEC depth`, restore.
  - Saving is to the depth-indexed page-1 arrays, so nothing zero-page is live across the `JSR`. That is the form Baron's allocator accepts.
  - The last edge's child is a `JMP` (tail call).

### 3.7 Build commands

```
cmake --build ~/dev/portalc/build --target portalc_sweep portalc_beebexport portalc_beeboracle
~/dev/portalc/build/beeb/host/portalc_beebexport ~/dev/portalc/beeb/data ~/dev/portalc/beeb/build/golden
# cd beeb; mkdir -p build
baron -v --symbols build/portal.json -p build --inf -log1 build/sizes.txt \
      -o build/portal.ssd --title PORTAL --opt 2 boot.6502 portal.6502 > build/portal.lst
baron -v -D TESTBUF=0 --symbols build/harness0.json -p build --inf \
      -o build/harness0.ssd --title HARNESS --opt 2 tboot.6502 harness.6502 > build/harness0.lst
baron --warn 2 --check portal.6502
```

- Each module ends with `PRINT #1, "<module>", P% - <module>_start`. `make sizes` reports the totals against §2.4.
- The first `entry - payload` bytes of `build/PORTAL` are the runtime image.
- The host takes every address from `build/*.json`. Nothing the host reads lives in the zero-page pool.

---

## 4. Runtime: IRQ, buffers, movement, keyboard

### 4.1 The vsync IRQ

This is spritescale's `irq` (ack CA1, count), with the swap moved in from its `swapbuffers`, and a whole-frame count in place of its half-rate `framectr`. Only CA1 is enabled, so the handler needn't check the source. It uses A only and has no BCD.

```
.irq  ZA_INTERRUPT
      LDA #&02 : STA &FE4D            ; ack CA1
      INC vsyncs                      ; 50.08 Hz, 19,968 us per frame
      LDA swapreq : BEQ out
      LDA #12 : STA &FE00 : LDA nextr12 : STA &FE01
      LDA #13 : STA &FE00 : LDA nextr13 : STA &FE01
      LDA vsyncs : STA swapstamp
      LDA #0 : STA swapreq            ; old front buffer is now free
.out  LDA &FC : RTI
```

- **Cost.** About 50 cycles, or 85 on a swap.
- **Why it can't tear.** With R4=38, R6=16 and R7=26, the IRQ lands in blanking, about 6.6 ms before the 6845 latches R12/R13 at the next frame start. Both register writes therefore take effect together.
- **All CRTC accesses happen in the IRQ.** The main loop never writes &FE00.
- **Shared state.** Every value the IRQ shares with the main loop is a single byte. `nextr12`/`nextr13` are written before `swapreq`.

### 4.2 Main loop

```
.mainloop
  JSR readkeys                          ; -> input
  LDA swapstamp : TAX : SEC : SBC lastswap : STX lastswap
  CMP #13 : BCC @+ : LDA #12 : .@       ; clamp as main.c
  STA nticks                            ; nticks x (turn, add the facing step), then
  JSR camera_step                       ;   one collision/slide/crossing tick
  JSR capture                           ; BSS only: overlaps the pending swap
.@ LDA swapreq : BNE @-
  JSR clear_back : JSR plot : JSR sprite_pass : JSR fill_back
  LDX backbuf : LDA r13_of,X : STA nextr13 : LDA r12_of,X : STA nextr12
  LDA #1 : STA swapreq
  TXA : EOR #1 : STA backbuf
  JMP mainloop
```

**How N is counted, corrected:**
- At the top of frame k+1, frame k's swap is almost always still pending. So N(k+1) = S(k−1) − S(k−2), which is **two swap intervals of lag**.
- N is 0 on frames 1 **and 2**. It is occasionally 0 later too, when a vsync lands between posting a swap and reading `swapstamp`.
- The sum of N still equals the elapsed vsyncs, less the clamp.
- The clamp's feedback is bounded: 12 ticks × at most about 11k cycles is at most about 132k cycles, so it can't run away.
- D8 offers N = vsyncs between loop tops, which lags one interval and also sums exactly.
- Keys are read once per frame and apply to all N accumulated steps.
- `camera_step` = N × (turn by the turn speed; add `dir8(speed, angle)` for each movement key into the s.8 move), then the existing collision, slide and portal crossing once, on the summed move.

**Expected frame cost** (uncalibrated model; call overhead, about +20% of the variable work, is not in it):

| | Median | 90th percentile | Max |
|---|---|---|---|
| Capture, plot and sprites | 27k | 87k | 170k |
| Fill and clear | 58k | 58k | 58k |
| Camera (N delta steps + one tick) | about 5k | about 6k | about 12k |
| **Frame** | **about 90k (about 22 fps)** | **about 151k (about 13 fps)** | **about 240k** |

These numbers are replaced at the fps go/no-go point at the end of Phase 4 (§5.9).

### 4.3 Keyboard and exit

- **Direct scan.** `LDA #key : STA &FE4F : BIT &FE4F : BPL no`, looping over `keynum`/`keybit`. It costs about 150 cycles a frame. OSBYTE and OSRDCH are never used.

  | Key | Number | Input bit |
  |---|---|---|
  | W | &21 | FWD |
  | Cursor up | &39 | FWD |
  | S | &51 | BACK |
  | Cursor down | &29 | BACK |
  | A | &41 | LEFT |
  | D | &32 | RIGHT |
  | Cursor left | &19 | TURNL |
  | Cursor right | &79 | TURNR |

- **Esc (&70).** `SEI : LDA #&7F : STA &FE4E : JMP (&FFFC)`.
  - The IER then reads &80, so the OS takes the **power-on** path: it clears RAM, rebuilds page 2 and enters BASIC.
  - A plain soft reset would keep &27E-&29B, and so would the BREAK key. That is why §2.1 never writes that range.
- **BRK.** BRKV points at `trap`. The ROM first issues service call 6, which may touch DFS workspace that is now our code, so reaching `trap` is best effort. &FD/&FE always hold the address.

### 4.4 Capacity overflow

- **Test builds:** `TRAP n` at every C `RC_PANIC` and at every `RC_ASSERT` whose failure would make the frame meaningless. That covers render.c:141, 230, 836, 857, 885, 899, 936, 1773, 1908 and the others.
- **Release build** (D9):
  - `capture` saves S with `TSX : STX capsp`.
  - On overflow: `LDX capsp : TXS`, clear `cell_visit` for the visits made so far, skip the rest of the frame and the swap, and `INC aborts`.
  - The same applies to overflows in the sprite pass. The half-drawn back buffer is cleared next frame.

---

## 5. Verification

### 5.1 b2 set-up and safety (`host/b2up.sh`, `host/b2.py`)

- **Starting b2.**
  - b2up.sh starts b2 only if nothing is listening on port 48075:
    ```
    SDL_AUDIODRIVER=dummy /snap/bin/b2 --config-folder ~/snap/b2/common/portalc-b2 \
        -c "B/Acorn 1770" --no-limit-speed -e http > beeb/build/b2.log 2>&1 &
    ```
  - It writes a pidfile.
- **Never take over Rich's session.** `b2.py` only talks to an instance recorded in the pidfile, unless given `--attach`. If 48075 is held by some other process, b2up.sh refuses and says so.
- **Right config, every run.**
  - Before every `/run`, `b2.py` issues `/reset/b2?config=B%2FAcorn%201770`, encoded because `+` isn't decoded as a space.
  - It then peeks &C000-&FBFF and compares it with b2's `assets/roms/OS12.ROM`, which has the same md5 as `OS120.rom`. Your default "B (Custom OS)" (`OS120new.rom`) fails this check.
- **Discs** go in the request body (`/run/b2?name=...ssd`, `Content-Type: application/x.acorn.disc-image.ssd`), so the snap's file sandbox and private /tmp don't matter.
- **URLs** always carry every positional part.
- **Tested configuration:** Model B, OS 1.20, Acorn 1770 DFS, no Tube, no Econet.
  - b2 has no 8271 Model B configuration, so 8271/DFS 1.20 load behaviour is untested.
  - If real 8271 hardware is available, run a one-off S0 at the maximum image size on it.

### 5.2 Mailbox (&2F0-&2FF, test builds)

| Address | Field |
|---|---|
| &2F0-&2F2 | Magic 'P' 'C' '1', written last by `start` |
| &2F3 | `cmd` |
| &2F4-&2F7 | `arg0`-`arg3` |
| &2F8 | `seq` (the host's first value is 1) |
| &2F9 | `ack` |
| &2FA | `err` (0 = OK; otherwise a TRAP code; &FF = BRK) |
| &2FB-&2FE | Elapsed µs, 32-bit |
| &2FF | NMI count (Phase 0) |

- `start` zeroes the whole block before writing the magic, so leftover OSFILE bytes can't look like a command.
- The host pokes the inputs, then `cmd..seq` in one request (so it is atomic), polls `ack` every 2 ms with a timeout, then peeks the outputs.
- The Beeb spins on `seq == ack`, so everything the host peeks is quiescent while it waits.

### 5.3 Harness builds

- One `harness.6502`, built with `TESTBUF=0` and `TESTBUF=1` (§2.2), and with `MAP=0|7|S`.
- It carries the full release Code implemented so far, with the mailbox as a second `ZA_ENTRY`. Zero-page allocation therefore matches the release build closely.
- `check.py zpdiff` reports every zero-page address that differs between `harness*.json` and `portal.json`.
- Every stage that writes a buffer runs on **both** buffers. Before each test, both of that buffer's junk rows are zeroed, and the junk rows (including &6E00) are part of the comparison.
- **Batch sizes** are floor(I/O area / record size), taken from the symbols.
- **Exhaustive and random arithmetic domains** are generated on the Beeb:
  - either by enumeration, or by a 16-bit xorshift that the oracle mirrors;
  - the Beeb returns a CRC-16 per 256 results;
  - a mismatching chunk is re-run with full output.

### 5.4 Oracle (`portalc_beebtool`)

- **Build.** A unity build of every engine `.c` file (`host/beebtool.c`), linked only against richc, in four variants: plain; `_cov` (clang source coverage, for the gate); `_search` (trace-pc-guard, for the pose search); and `_trace`, which includes the copies of render.c and arith.c that `host/trace.py` writes at build time, with the unit-replay wrappers and the cost counters in (D1c, D1d).
- **Output.** It writes expected results in a canonical field order. `check.py` reads the Beeb's fields by symbol name (`bss.p_x0`, ...), normalises layout details, and reports the first mismatch with its pose, index and decoded values.
- **Buffers.** Before re-running `plot()`, the oracle zeroes all of `fbmem`, junk rows included.
- **`nrm[0]` poison run.** The oracle sets `nrm_[0]=&40`, re-renders every golden and re-runs every camera stream, and checks that nothing changes. That makes S1's `nrm[0]` exception safe without editing arith.c.

### 5.5 Coverage gate (Phase 1, re-run every phase)

- `make coverage` (`host/coverage.sh`) runs every golden stream in §3.5 through the `_cov` build and lists every unexecuted line of arith.c, camera.c and render.c.
- **Pass condition:** every line is executed, or is listed in `host/uncovered.txt` with a reason. The script fails otherwise, and also when a listed line starts running.
- **Where it stands (end of Phase 1):** 31 lines, each one dead (an invariant rules it out), unused, or rare (reachable, but no pose found in the search; S2 or S5 synthetic cases cover them). Rich's ruling (D10): **dead code is not ported.**

### 5.6 Unit replay (clip group and walk internals)

- **Targets:** `clip_line`, `resolve_block`, `emit`, `clip_edge_line`, `cut`, `child_portal`, `next_open`, `close_columns`, `process_edge`, `seg_record`, `inst_new` and `inst_issue`.
- **Recording.** Across the golden poses, the oracle's wrappers record each call's arguments and its before-state and after-state:
  - `pool[1..pool_num)` and `pool_num`;
  - the `pt_clip` fields and cursors (tp, bp, bp_set);
  - the chain builders and the closed list;
  - the visit record;
  - the segments and instances.
- **Sampling.** Records are capped per function, keeping every call that reaches a new line.
- **Replay.** A Beeb shim pokes the before-state into the BSS layout, calls the routine, and the host diffs the after-state.
- The record format is in `REPLAY.md`. `make record` writes about 3,100 records (the first 64 calls of each function, every call that runs a new basic block, every 1024th call, at most 2,000 per function), then replays each on the host: every record must reproduce itself, which proves it holds everything the function reads.

### 5.7 Checks

| Check | What is compared | Pass condition |
|---|---|---|
| **S0 image** | &300 onwards against `PORTAL[0:N]`: on every harness boot once the magic appears (before any command), and once on the release build with `-D HALT_AT_START`. Operands labelled `smc_` are masked using the symbols JSON. Phase 0 runs it at the maximum N (&5A00). | Identical |
| **S1 tables** | Every table byte in the image against the oracle's dump. The one exception is `nrm[0]` (&40 against 0), covered by the poison run. | Identical |
| **S2 arith** | Each routine in dependency order, exhaustive where the domain is at most 2^17, otherwise edge values plus 1M Beeb-generated cases, compared by CRC. Includes `mulu8`, `recip8`/`recip16`, `grad_mag`, `dir8`, `rot` (all angles × a grid plus random), the saturation arms that can fire (`host/uncovered.txt` lists the ones that can't, which aren't ported), `pt_div_q8` and `pt_ray_basis`. A primitive's driver expands its macro (D12). | Identical |
| **S3 mode5** | Fill and clear of random buffers; single-pixel ROW/AT on every row including −4..−1 and 64..67; both buffers. | Identical, junk rows included |
| **S4 plot** | For every golden pose: poke the C's pool, `plot` into a zeroed buffer and junk rows, peek. Also random single-piece stress and a full pool (`pool_last`=255). Both buffers. | Identical |
| **S5 lines** | `transform` of every vertex at each pose, with outside vertices fed straight after inside ones (`sx` and `rx` compared only inside the frustum, where the C defines them). `make_line` and `edge_line` records. | Identical |
| **S5u replay** | §5.6, per routine. | Identical after-state |
| **S6 capture** | `pool_last`, the pieces in index order, visits, `cell_visit` cleared afterwards, the stack high-water mark. Then whole frames with sprites off, on both buffers, on the demo and stress maps. The stress map runs the depth cap at 16 and measures the real stack worst case. `-D ABORTTEST` forces an overflow through a pokeable starting pool index: the frame must be skipped and the next pose must render exactly. | Identical; canary intact |
| **S7 sprites** | Instances, segments and order links; the sprite pass on poked capture state; whole frames with sprites on against `sweep.bin` and all other goldens. Includes sprite centres just outside each ray, and `map7`'s 4,688 frames. | Identical |
| **S8 camera** | Per `camera_step(input, n)`: x and y (u24), angle and cell over the walk, collision and combo streams (84,000 steps, with n drawn from 1..12), in batches sized to the I/O area or as a CRC per 64 steps with a full dump on mismatch. Coverage must show `corner_deflect` and `mulu16x8_lo` executed. | Identical at every step |
| **S9 integration** | The release loop with real IRQs, swaps and N counting, with input taken from a ring (see below). Runs of 500 and 5,000 frames. `oracle replay` runs the logged (input, N) sequence through `pt_camera_step`. | Every frame's pose and checksum match; the final buffers and camera state match in full; ΣN = vsyncs elapsed less clamp losses. The N distribution is reported. |

**The S9 ring.** The ring lives in the headroom below `BSS_BASE`, with at least 16 records ASSERTed.
- **Record:** 12 bytes: input, N, x u24, y u24, angle, cell, and a 16-bit checksum of the back buffer taken before its swap is posted.
- **Flow:** the host writes inputs ahead of the Beeb and drains the logged records.
- **When input runs out,** the Beeb spins with IRQs live. The stall only shows up as a clamped N, which the replay reproduces.
- **Checksum cost:** the checksum costs time, so SCRIPTED runs are never used for fps figures.

### 5.8 Failure diagnosis

- Every poll has a timeout. On timeout, or when `err` is non-zero, the host saves the following to `build/fail-<time>/`:
  - the MODE 7 screen &7C00-&7FE7, which shows DFS errors such as "File not found";
  - pages 0-2 (stack return addresses, and the BRK address in &FD/&FE);
  - the mailbox and BSS.
- On a frame mismatch, the host writes expected, actual and XOR images with `portalc_fb2ppm`.

### 5.9 Cycle measurement

- **Timer.** Every harness build, and `-D PROFILE`, runs the vsync IRQ with a T2 accumulator. The IRQ adds `(last − T2) mod 65536` into a 32-bit µs total.
- **`now()`.** `SEI`, then read T2 high, low, high and retry if the high bytes differ, add the total, `CLI`. One T2 tick is 1 µs, which is 2 cycles.
- **Calibration.** b2 emulates the 1 MHz bus stretching, so VIA accesses cost what they would on hardware. An empty section is measured and subtracted. The IRQ share (about 0.15%) is reported.
- **Per-routine cost** (S2) is (batch time − empty-loop time) / count, compared with the JOURNAL sketches.
- **Per-stage cost.** S6 and S7 return a per-pose record (capture, clear, plot, sprites, fill). The `PROFILE` build logs per frame: ticks, capture, wait, clear, plot, sprites and fill. `check.py profile` gives the median, mean, 90th percentile and max, plus the fps and N distributions.
- **fps go/no-go at the end of Phase 4.**
  - Re-weight `host/costmodel.c` with the measured cycles for arith, clear, fill and plot, plus about 15 cycles of overhead per call.
  - Cost the 7-sprite map as well.
  - Bring the predicted frame times to Rich before Phase 5.

### 5.10 jsbeeb-mcp (debugging aid)

Rich installed jsbeeb-mcp (Matt Godbolt's headless jsbeeb as an MCP server, `claude mcp add jsbeeb -- npx -y jsbeeb-mcp`, local to this project). It doesn't replace b2: the byte-exact runs over thousands of poses stay scripted against b2's HTTP API, and b2 remains the reference. It adds what b2's HTTP API lacks, for interactive use by Claude:

- **Registers and cycle counts** (`read_registers`), and `run_for_cycles` stopping at a breakpoint: exact cycles for one routine, where T2 on b2 gives microseconds over batches. This settles D11 without the Python interpreter.
- **Breakpoints and machine snapshots** (`save_state`/`restore_state`): stop at a TRAP or a suspect routine, inspect memory, retry from a checkpoint.
- **Headless**, so no window: quick experiments while b2 runs the harness.
- **Screenshots** of real video output, for eyeballing the display set-up (Phase 0) and frames.
- **A second emulator**: agreement between b2 and jsbeeb on a stage's output is a cheap guard against an emulator quirk being mistaken for a port bug.

Use it from Phase 0 (display set-up, stub, IRQ), in Phase 2 (per-routine cycles against the JOURNAL sketches), and whenever a b2 check fails and the fault needs finding.

---

## 6. Phases

Effort is in focused working days. Phase 8 can start any time after Phase 2.

| Phase | Deliverable | Checks | Days |
|---|---|---|---|
| **0. Skeleton, loader, b2** | `beeb/` tree, Makefile, boot files, stub and copy, takeover, CRTC/ULA/palette, IRQ and swap loop alternating two test patterns, mailbox, TRAP/BRK trap, failure dumps, b2up.sh with pidfile and config/ROM check, `b2.py`, `beeb/README.md`. `INC &2FF : RTI` at &D00 for now. Code padded to the maximum &5A00. | S0 at the maximum image size; a 5-minute soak with no NMIs; IRQ period measured at 19,968 µs; tear-free by eye. Measure HTTP round-trip time. Try a minimised window and `SDL_VIDEODRIVER=offscreen` (D5), a fresh config folder's first run, and `SDL_AUDIODRIVER=dummy`. | 3-4 |
| **1. Export, tables, oracle, gate, spike** | `beebexport` (maps including stress, data, goldens), `tables.6502`, the oracle unity build and generated traced render.c, pose search, coverage gate, unit-replay record formats. **Recursion spike:** walk skeleton with the real save stack and the real 9 bytes, inlined `descend`, a dummy leaf through the deepest chain (`process_edge` → … → `mulg_u16`), IRQ running, real `ZA_POOL`. Measure bytes per level and zero-page use, and decide on the save-stack form. | S1; poison run; `mapdump` agrees with the exported map; poses re-render to `sweep.bin`; coverage report (D10) | 3-5 |
| **2. Arith** | `arith.6502` in dependency order: mulu8 → mulu16_frac(+round) → mulg_u16 → recip8/16 → mul_recip8/16 → grad_mag, div_t, div_cols, div_q8, gradq8_mag → project_col, _q8, project_q8, project_rowd → lerp16 → rot → dir8 → ray_basis | S2 and cycles per routine. Budget checkpoint. | 4-6 |
| **3. Mode 5** | Cursor macros, clearA/B, fillA/B | S3 on both buffers; fill about 37k and clear about 21k cycles | 1-2 |
| **4. Plotters** | Steep, the shallow trees, flat, end bytes, `plot` with `pool_last` | S4 on both buffers. **fps go/no-go (§5.9).** | 3-4 |
| **5. Vertices, lines, clip** | Pool and chain helpers, `transform`, `col_*`, side clips, `rel_h`/`make_line` (near-clip branch included), `edge_line`, the clip group | S5, S5u. Budget checkpoint. | 6-8 |
| **6. Walk and capture** | Visits, `child_portal`, `next_open`, `close_columns`, `process_edge`, roots, walk (save stack, tail `JMP`), frame driver, overflow handling (D9) | S5u (walk functions), S6 on the demo and stress maps. **First whole frames byte-exact with sprites off.** | 4-6 |
| **7. Sprites** | `inst_new` (24-bit, flooring `>>8`), `seg_record`, `inst_issue`, `sprite_pass` | S5u (sprite functions), S7 including `map7`. Budget checkpoint; `BSS_BASE` finalised. | 5-7 |
| **8. Camera** | C first: `pt_camera_step(c, m, input, n)` (the accumulation, then the existing tick body), with `pt_camera_tick` = n of 1 so nothing else changes. Then `camera.6502` (u24 position, page-2 scratch). | S8 over 84,000 steps with n in 1..12, plus cycles per step | 3-5 |
| **9. Integration** | Release main loop, keyboard, clamp, Esc power-on exit, the `SCRIPTED` ring, `PROFILE` | S9, the profile report, a manual play test (including Esc and BREAK) | 3-4 |
| **10. Optimisation (optional)** | Profile-guided changes: table placement, `pt_rot`/`edge_frame`, fallbacks from §2.4. The spec §8.3 line buffer is dropped, because it needs about 4.5K the map doesn't have. | Re-run S6, S7 and S9 after every change | open-ended |

The total through Phase 9 is roughly **35-51 days**.

---

## 7. Documentation schedule

| Phase | Update |
|---|---|
| Every phase | A JOURNAL.md entry |
| 0 | `beeb/README.md`: build, run, harness use, memory map |
| 3 | Spec §8.3: the shared junk page, per-buffer clear and fill bodies, and why the line buffer is ruled out on memory |
| 6 | Spec §7.5: the walk's depth-indexed save stack, inlined `descend`, `pool_last` |
| 7 | Spec §9.4: pending entries kept in BSS rather than on the hardware stack |
| 8 | Spec §10.4: measured camera cost per tick, which is well above "a few hundred" |
| 9 | Spec §12 (measured costs) and §13 (verification done), and §14 if anything is still open |

---

## 8. Risks, ranked

1. **Release memory is tight.** Headroom is about 3.2K at the estimate and about 0.2K at +25%.
   - *Mitigation:* guards and ASSERTs, `make sizes` at three checkpoints, the ordered fallbacks in §2.4 (the pool change needs your OK). Harness builds are no longer limited by this.
2. **Exactness depends on C integer details:** arithmetic `>>`, 24-bit intermediates, u8 wraparound, asserted s16 fits.
   - *Mitigation:* §3.6, Debug-only goldens, TRAPs at the C's asserts, per-stage diffs and unit replay to localise any divergence.
3. **The goldens miss code.**
   - *Mitigation:* the coverage gate, pose search, the stress map, `map7`, the combo input stream. What remains goes to D10.
4. **Camera: one tick per frame on a summed move.** Its cost (about 4.4k cycles typical, 11k worst) is paid once a frame. The risk is the collision's small-move assumption (`|M| < 16` units, camera.c:150) once the 50 Hz speeds are set and N is large.
   - *Mitigation:* per Rich, revisit once speeds are tuned: options are capping the summed move, lowering the N clamp, or widening the candidate search. S8 draws n from 1..12 so any break shows up.
5. **Stack and recursion.**
   - *Mitigation:* the save stack in page 1 plus inlined `descend` (about 60 bytes of hardware stack), the Phase 1 spike, the stress map exercising the real cap of 16, and the canary in every test.
6. **Zero-page allocator** problems (refusals on recursion, pool exhaustion, aliasing that shows only in the release build).
   - *Mitigation:* the spike before Phase 5, a harness that carries the full code, the `zpdiff` report, per-frame checks in S9.
7. **The frame-cost model is uncalibrated.**
   - *Mitigation:* recalibration and a go/no-go at the end of Phase 4.
8. **b2 automation unknowns** (headless modes, latency, first run of a fresh config folder, dummy audio) and **harness safety**.
   - *Mitigation:* the Phase 0 spike, a visible window as fallback, the pidfile and config/ROM check.
9. **Load time.** Only the 1770 DFS is tested. Whether that DFS uses &1100-&18FF during `*RUN` is resolved by S0 at the maximum image size. The NMI risk is covered by RTI at &D00 and the Phase 0 NMI soak.
10. **Overflow behaviour** has no C reference.
    - *Mitigation:* D9, the C's own caps, TRAPs in test builds, and the `ABORTTEST` check.
11. **Branch range and page-crossing costs** in hot loops.
    - *Mitigation:* `BNEP`/`FITPAGE`/`JNE` macros, fill operands laid out so `abs,X` never crosses a page, and `ASSERT HI(t)==HI(t+size-1)` on page-sensitive tables.

---

## 9. Critique points not taken

- **Lowered-cap build (`PT_MAX_DEPTH` overridable on both sides).** Not the default. It needs an edit to render.h and tests a cap the real build never uses. The stress map reaches the real cap of 16 and measures the real stack worst case. The lowered cap stays as a fallback if the map can't reach the cap.
- **Render a degraded root-only frame on overflow.** Replaced by skipping the frame (D9). That costs about 20 bytes, whereas a root-only frame would be new rendering code for a case where the C has no reference.
- **Use &4E00-&7FFF as Stage 2's I/O area.** Superseded. Harness builds now move BSS into one buffer, and exhaustive and random arithmetic domains are generated on the Beeb and returned as CRCs, so I/O space no longer limits batches.
- **An oracle assert that nothing reads `nrm[0]`.** Replaced by the poison run, which proves the same thing on every golden without editing arith.c.
- **Build the host tools from `src/CMakeLists.txt` beside `src/tools`.** They stay in `beeb/host`, added from the root CMakeLists, so the port is self-contained. The unity-build oracle deals with the duplicate-statics concern that motivated the point.
- **Exact cycle counts from the Python 6502 interpreter.** Not adopted by default, because you chose b2 as the emulator. It is offered as D11.