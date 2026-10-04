# Beeb port journal

Newest last. The plan's in `PLAN.md`; the C side keeps its diary in
`../JOURNAL.md`.

## 2026-09-30 — Phase 0: it boots, it swaps, it (probably) doesn't tear

Nothing clever yet. Just a program that loads, takes the machine hostage
and flips between two screens.

The file loads at &1200 and a stub shuffles it down to &300. Baron's
nested sections do the address juggling:

```
SECTION Loader, org = &1200, ..., filename = PROG_NAME
.payload
    SECTION Code, org = &300        ; assembled for &300, carried at &1200
        ...
    ENDSECTION
.entry                               ; the stub: take over, copy, JMP start
```

The copy runs upwards with the destination &F00 below the source, so it
never eats a byte before reading it. The stub lives past the payload,
well out of reach.

While the OS can still flush its buffers, the stub does the hostage bit:
`*FX15`, `SEI`, both VIAs muted, spritescale's CRTC and ULA settings, and
every colour black. Whatever the load left in buffer A stays invisible
until `start` is ready.

```
&0300  tables (Phase 1)        NMI handler at &D00, for now
&0F00  code
&4E00  per-frame state
&5D00  junk  ─┐
&5E00  buffer A
&6E00  junk  ─┤  the middle one's shared
&6F00  buffer B
&7F00  junk  ─┘
```

The IRQ is spritescale's with the swap moved in. The main loop posts an
address; the IRQ writes it in the vertical blank, just before the 6845
latches it anyway. So no tearing, by design:

```
    LDA swapreq : BEQ out
    LDA #12 : STA &FE00 : LDA nextr12 : STA &FE01
    LDA #13 : STA &FE00 : LDA nextr13 : STA &FE01
    LDA vsyncs : STA swapstamp
    LDA #0 : STA swapreq            ; old front buffer: yours
```

Test builds also keep a microsecond clock: the user VIA's T2, counting
down from &FFFF forever. Each vsync the IRQ tots up what it's counted,
using only A, since that's all the OS bothered to save.

The release build shows bars and a checkerboard on alternate frames, with
a green band crawling down. A tear would snap the band.

### Talking to b2

Test builds keep a mailbox in page 2. The host pokes a command, bumps
`seq`, and waits for `ack` to catch up:

```
&2F0  'P' 'C' '1'   magic, written last
&2F3  cmd           &2F4  four args
&2F8  seq           &2F9  ack
&2FA  err           &2FB  microseconds (4 bytes)
&2FF  stray NMIs
```

`host/b2.py` does the HTTP. `B2_VIDEO=offscreen make phase0`, 56 seconds:

| Check | Result |
|---|---|
| boot | both harnesses up in about a second, ROM checked as a real OS 1.20 |
| S0 | the full &5A00-byte image at &300 matches the file |
| irq | 50 vsyncs in 998,411µs: 19,968.2µs each (the book says 19,968) |
| soak | 5 emulated minutes, no stray NMIs, image intact, clock out by 10µs |
| release | swapping away, image intact |

Things b2 taught me:

- Long options want an `=`. Give it a space and it pops up a dialog and
  dies.
- It's perfectly happy with `SDL_VIDEODRIVER=offscreen`. No window.
- Every HTTP request costs about 20ms, so bulk checks will have to batch.

Still owed: actually looking at it. b2 won't take screenshots over HTTP,
so it's `make run` and a pair of eyes, or jsbeeb-mcp.

### Baron's allocator, under observation

I'm keeping an eye on the zero-page allocator in case it gets timid.
Verdict so far: not timid.

Anything the IRQ shares gets a byte to itself. Temporaries pile onto each
other the moment the previous tenant's dead:

```
draw.kind  = &0A     draw.frame = &0B
draw.rows  = &0A     draw.lines = &0B    ; moved in after the last reads
```

The cheekiest one: in the harness, the IRQ's temporary `hi` sits on
`swapstamp`'s byte. Nothing in the harness reads `swapstamp`, so the
store's dead and the byte's fair game. Quite right. It also means the
host has no business peeking pool variables: anything the host reads
goes at a fixed address.

(Phase 1 update: "quite right" was generous. Baron does the same when
main *does* read it. See below.)

13 bytes used in the release build, 25 in the harness.

### House rules

- Every `ZA_AUTO` says what it holds, and whether it's `in:`, `out:` or
  `local:`.
- Nothing lives in the code image. S0 compares the running code with the
  file, and a counter in there would fail it. I had two. They moved out.

## 2026-10-01 — Phase 1: tables, the C's homework, and a spike through Baron

### The boring half

The tables sit at &300-&EFF, the maps come out of the C as Baron source,
and every harness boots on b2 and agrees with the C byte for byte:

| Check | Result |
|---|---|
| S0 | &5A00 bytes at &300 match the file, all four harnesses |
| S1 | &300-&EFF match `arith.c`, except `nrm[0]`, which is an RTI |
| map | maps 0, 7 and 9 match the C, array by array |
| poison | the C with `nrm[0] = &40` renders every golden identically |

The RTI is the NMI handler. The ROM jumps to &D00, `nrm` lives at &D00,
and entry 0 is never read. Free handler.

### Coverage, or: what the goldens never do

The coverage gate wants every line of the ported C run by some golden.
The sweep and the seven-sprite walk left 97 lines of `render.c` untouched.
So a stress map, map 9:

```
    +            +------------+           +
    | R1  \      |    R3      |     / R2  |    R1  a shelf, above the lintel's line
    |            b------------c           |    R3  a slot 8 units tall
    |         /                 \         |    R2  a crawlspace, below the floor's
    |     /           Q             \     |    Q   tall and deep
    |  /                               \  |
    a                                     d
    +-------------------------------------+
    |              corridor               |    17 squat cells, ending in a wedge
```

From the corridor's near end a line of sight crosses more than 16
portals, so the depth cap finally does something. R1's floor hides behind
the corridor's lintel where it's close, so the floor line closes columns
at one end. R2's ceiling does the same under the floor edge at the other.
The wedge is for the camera: walk into a 65° corner and the slide gives
up, which is a path the camera code has and never took.

Plus a pose search. A clang `-fsanitize-coverage=trace-pc-guard` build of
the tool flags any frame that runs a basic block for the first time. 15
million random poses, biased to rims, vertices and near-clip distances,
found exactly one: the eye on a boundary facing just so, which sends
`root_portal` to its exact retry. It's a golden now.

`render.c` went from 97 unrun lines to 22. The 31 left across all three
files are in `host/uncovered.txt`, each with a reason, and
`host/coverage.sh` fails on any unrun line that isn't on the list. Most
are dead:

```
    j = inst_col(in, c);
    if (j >= pt_sprite_width(a))    // can't happen: c1 is rounded from the
        continue;                   // same left + w * scale that inst_col walks
```

That one, `seg_piece_at`'s loop, `crossing`'s `n = 0` and a few others
are C to simplify, not 6502 to write. Parked for a decision.

### The spike

Before writing any clip code, I wanted `walk`'s recursion on the real
Beeb with Baron allocating. So `test/spike.6502` is walk's skeleton:

- the save stack in page 1: nine arrays of 16, indexed by depth;
- `descend` inlined;
- a fake leaf chain seven calls deep, like the real
  `process_edge` → … → `mulg`, each link holding a few bytes across its call.

Everything feeds a checksum, and `host/spike.py` computes the same walk.
A bad save, a bad restore or a bad allocation changes the sum.

```
    LDX spike_depth
    CPX #PT_MAX_DEPTH : BCS cap
    LDA cell : STA STK_CELL,X       ; nine of these
    ...
    JSR take_child
    INC spike_depth
    JSR walk
    DEC spike_depth
    LDX spike_depth
    LDA STK_CELL,X : STA cell       ; and nine of these
```

`make spike`: 256 seeds, 59,781 portals, depth 16 every time, every
checksum right. The hardware stack bottoms out at S = &CF: 48 bytes, plus
3 if the IRQ lands there. Nothing touches the canary.

So the save stack stays as planned. The alternative, PHA/PLA round the
call, would cost 11 bytes of hardware stack a level instead of 2, about
200 bytes at depth 16. By count it would save about 20 cycles a descent,
which is noise.

Zero page in the harness: &00-&4A, 75 bytes, and every one accounted
for. That's 44 held by the chain, 9 for walk, 2 for depth and the pending
count, 2 for the leaf, 4 for the spike's clock and 14 for the IRQ and its
microsecond clock. No holes.

### Baron, under observation, part two

The spike wouldn't assemble at first:

```
error: ZA_AUTO variable freshly written and held live across recursion
```

Which variable? It didn't say. Bisecting found three problems. One was
mine, two were Baron's.

**Baron's: the smear.** A routine's RTS saw the live-after of every call
to it, results and bystanders alike. So a variable held across one `JSR
helper` looked live inside `helper`, and so before every `JSR helper`
anywhere:

```
.a { ... STA va : JSR helper : LDA va ... }
.b { JSR helper : ... STA vb : JSR helper : LDA vb ... }
     ^ va looked live here, so va and vb never shared a byte
```

In the spike, walk calls `spike_mix` at the top and again while `steps`
is live, so `steps` looked like a value carried into the recursion.
Fixed in Baron (0.4.2.0): a return only carries bytes the routine may
write. Anything else is already fenced off at the call. The spike still
needs it: `cmd_spike` calls `now` either side of `JSR walk`, and the old
Baron smears walk's inputs through `now` and refuses. 442 tests pass, and the
examples and all five of our builds assemble byte-identical. The error
names names now, too:

```
...held live across recursion: 'spike_sum', 'spike_dhigh', 'spike_lows'
```

**Mine: indexed stores.** `STA t0,X` proves nothing about which byte it
wrote, so `t0` never died and looked live all the way round the mailbox
loop. Baron has the word for it, and I'd forgotten:

```
    ZA_DISCARD t0                   ; the old value's dead: the loop makes a new one
    LDX #3
.keep
    LDA now.t,X : STA t0,X
    DEX : BPL keep
```

In `cmd_time` too, which gave back four bytes nobody knew were pinned.

**Running values through A.** `LDA sum : EOR x : STA sum` was a fresh
store as far as Baron could see, and inside a recursion a fresh store read
back afterwards is the per-level bug it exists to catch. `INC` was fine.
The spike's checksum, max and minimum moved out of the pool: they're the
test's, not the walk's. (Baron's since learned better: see below.)

That last one mattered for the real port: `pool_last`, `seg_last` and
friends are frame-global counters bumped inside the walk, and
`LDX pool_last : INX : STX pool_last` got the same refusal. Baron judged
freshness one instruction at a time, and a plain store is always fresh.

The fix needs nothing but loads and stores. If the recursive routine
reads a variable before it writes it, the value came in from the caller,
and it's one running value, however it's updated:

```
.walk
    ...
    LDX pool_last : INX : BEQ full  ; read first: pool_last is live into walk
    STX pool_last                   ; so this updates the running value
```

Only a variable the routine writes before any read is per-level, and
refused. (My first go tracked registers through every opcode. Rich called
it overcomplicated, and it was.) The spike's checksum, max and low water
could go back into zero page now, but they're the test's, so they stay
put.

**Baron's: IRQ outputs.** `swapstamp` again. Baron pinned what a handler
reads (`vsyncs`, `swapreq`) and fenced its footprint from main, but a byte
the handler only writes, while main reads it, got neither. So the
handler's own temp could land on it:

```
.main  LDA stamp : ...              ; main reads
.irq   STA hi : ...                 ; every interrupt: hi = stamp's byte
       BEQ out : STA stamp          ; ...but only sometimes rewrites stamp
```

Both got &70. On an interrupt that skips the store, main reads a T2 byte.

No annotation says "shared with a handler", and `ZA_INTERRUPT` promises
to keep the handler's variables apart from the rest of the program, so
it's a bug, not a missing marker. Fixed in Baron too: anything the handler
touches that's live outside it is pinned, like its inputs. Nothing
changes for us today, since nobody reads `swapstamp` yet. Phase 4's frame
counting will.

## 2026-10-01 — Phase 1, the rest: a traced render.c, replay records, costs

Two rulings from Rich first:

- **Counters stay automatic.** No fixed addresses where Baron can
  allocate. Anything the walk bumps (`pool_last`, `seg_last`, `num_pend`
  and friends) is updated in place.
- **Dead code isn't ported.** The dead lines in `host/uncovered.txt` stay
  in the C and never reach the Beeb.

### Wrapping render.c without touching it

Unit replay wants to catch `clip_line` and friends in the act: their
arguments and the state before and after. `host/trace.py` does it at build
time, on a copy. Each target is renamed and a wrapper takes its name:

```c
static void clip_line(pt_clip *cx, const pt_line *L, uint8_t st)
{
    tr_before_clip_line(cx, L, st);
    clip_line__impl(cx, L, st);
    tr_after_clip_line(cx, L, st);
}
```

The same script sprinkles the cost model's counters in, at function
entries or at anchor lines:

```python
('emit_merge', [('render.c', 'emit', r'^\s*uint8_t \*pp = \(st == ST_ABOVE\)', 1, 'before', None)]),
```

Every anchor has to match exactly as many times as it says, so when
render.c moves under it the build fails instead of quietly miscounting.
The traced build renders every golden byte-identical to the plain one.

### Records

Snapshotting 3K of frame state on every one of two million calls would
take a while. So recording runs the goldens twice. The first pass picks
calls: the first 64 of each function, any that runs a basic block nothing
had run before, and every 1024th. The second pass renders the same poses,
deterministically the same calls, and snapshots just those.

3,113 records, 2.6 MB, in 3 seconds. The format's in `REPLAY.md`.

Then `replaycheck` puts each one into a fresh renderer, runs the function
again and compares. All 3,113 reproduce themselves, so each record holds
everything its function reads. Make the decoder forget `cell_visit` and
the lot fails.

### What a frame costs, roughly

The scratch cost model from the planning moved in, on the same counters.
Estimated 6502 cycles of variable work per frame (capture, plot, sprites;
not the clear, fill or swap):

| map | frames | mean | capture | plot | sprites | 90% | max |
|---|---|---|---|---|---|---|---|
| 0 | 7,545 | 32,066 | 22,617 | 7,120 | 2,329 | 77,661 | 169,794 |
| 7 | 2,344 | 43,729 | 27,498 | 8,642 | 7,589 | 100,009 | 176,226 |
| 9 | 10,520 | 47,435 | 36,786 | 9,283 | 1,366 | 101,818 | 187,092 |

The weights are the sketches' guesses. Phases 2-4 measure the real
cycles, and the weights get swapped for those.

### Phase 1: done

Tables, maps, goldens, the pose search, the coverage gate, the traced
build, the record format, the spike, all on b2 where they touch the Beeb.
Two Baron bugs found and fixed on the way. Next: arithmetic.

## 2026-10-01 — Phase 2: the arithmetic, byte for byte

`src/arith.6502` has all of arith.c that anything uses: thirteen routines
and the primitives under them. Each one has been run on b2 against the C,
over every input where that's feasible (`gradq8_mag`: every dy against
every dx, 4.3 million), and over a million random ones plus edge sweeps
where it isn't. All identical.

### What isn't there

Dead code isn't ported (D10), and arith.c has a little:

- `pt_mulu8_neg` and `pt_mulu16_frac_neg`. Nothing calls them.
- `div_q8`'s byte-denominator path, its `e < 8` path and its 0xFFFF
  clamp. Its only caller, `corner_deflect` (camera.c:142), divides by 256
  or more.
- The clamps in `grad_mag`, `project_col` and `project_rowd`, and
  `gradq8_mag`'s right-shift one. Exhaustive runs at the worst case never
  reach them: `project_col` at |y| = x, every x, never gets q past 64.

The rare ones stay: `mulg_u16`'s saturation, `div_cols`'s clamp at 127,
both of `project_col_q8`'s, and `project_rowd`'s guard for e = 0. S2 feeds
them directly.

### S2

`CMD_S2` runs one routine over a batch. Each case's 32 input bits come
from a counter (exhaustive runs) or xorshift32 (random ones). A driver per
routine bends them into valid inputs, calls it, and folds the outputs into
a CRC-16. `beebtool s2` does the same to the C, and the host compares:

```
ok     S2 random div_cols, 1000000 cases                 355.8 cycles a call
```

Two CRCs that differ say nothing useful, so on a mismatch the host
bisects. The Beeb reruns shorter batches until the first bad case falls
out, and both sides print it:

```
FAIL   S2 all project_rowd, 6553 cases   first bad case at counter 33: C [31, 0, 0, 0], Beeb [159, 6, 0, 1]
```

The cycles come from running the batch again with the call skipped and
subtracting.

### Macros

Rich's call: nobody is ever going to want `JSR mulu8`. So the primitives
are macros, named after their C functions, and expanded in place on the
caller's own variables:

```
    LDA sine,X : BEQ cone
    SETMUL                          ; one table value...
    MULU16_FRAC_ROUND ux, cx        ; ...times |x|
    MULU16_FRAC_ROUND uy, cy        ; ...and |y|
```

That killed the relays too. `STA recip16.n` and `STA mul_recip16.num`
copied a caller's variable into the callee's just so it could be called.
A ZA_AUTO in a macro is declared wherever the macro is expanded, so
`RECIPW` gets a fresh temporary each time, and Baron overlays them.

It costs bytes: 1,831 as routines, 2,321 as macros (2,241 after the
two-byte change below). Most of the difference is `ACC_SHR`, a loop,
expanded seven times. If that ever
matters, `.acc_shr : ACC_SHR : RTS` puts the call back for 12 cycles.

### What it costs

Cycles a call, from 10% runs, so both columns saw the same inputs.
Inline primitives have no JSR to pay for; the composites' figures
include theirs. "Routines" is the same code before the macros and the
fast paths.

| | routines | now | | routines | now |
|---|---|---|---|---|---|
| mulu8 | 55 | 42 | grad_mag | 200 | 127 |
| recip8 | 35 | 15 | gradq8_mag | 305 | 266 |
| recip16 | 81 | 70 | project_col | 279 | 291 |
| mul_recip8 | 70 | 55 | project_col_q8 | 362 | 328 |
| mul_recip16 | 126 | 113 | project_q8 | 225 | 192 |
| mulu16_frac | 101 | 89 | project_rowd | 263 | 116 |
| mulu16_frac_round | 107 | 95 | lerp16 | 172 | 156 |
| mulg_u16 | 66 | 54 | rot (grid) | 814 | 569 |
| div_t | 373 | 310 | rot (random) | 803 | 557 |
| div_cols | 411 | 356 | dir8 | 183 | 183 |
| div_q8 | 363 | 310 | ray_basis | 144 | 145 |

The full run's exhaustive figures differ, since 10% of a counter is the
bottom of the domain: `recip16` is 94 over every n (its shift loop runs
once per bit of the high byte, which 10% never got past 25), `grad_mag`
166, `gradq8_mag` 264,
`rot` on the grid 561, `dir8` 204, `ray_basis` 153 and `mulu8` 44.

The fast paths:

- **`project_rowd`.** From depth 256 on, the answer is the top nine bits
  of h8 * (256 + f): one multiply, an add, a byte shifted right.
- **`rot`.** It sets each table value once and multiplies both
  magnitudes by it, and the signed sums are inline.
- **`grad_mag`.** dy * (256 + f) fits 15 bits, so the shift is a 16-bit
  one.
- **`project_col`.** From depth 32, |y| * (256 + f) >> 8 fits 16 bits.

`project_col` looks worse than before, and that's S2's random depths.
Three quarters of them are past 16384, where the fast path's shift loop
goes round 9 or 10 times at 14 cycles a go. The maps' depths, 256-4095,
go round 2-6 times.

Against the sketches: `mul_recip16` is 113 here against 105-114 there.
`project_rowd` is 110 with its RTS, against 76-95, and that's S2's depths
again: at 65535 its last loop goes round six times, at the maps' depths
twice at most.

### What S2 caught

Three bugs in the routines, all mine, and one in the rig:

- **`project_col` read a carry across a JSR.** The callee happened to
  leave C one way, until it didn't. It now sets C itself.
- **`dir8` kept the angle in a byte its helper reused.** Its helper now
  has its own temporaries.
- **`project_rowd`'s fast path.** It put a `CPX #9` between the ADC and
  the ROR that wanted its carry. X is at least 9 on that path, so C was
  always set, and every answer past depth 512 came back with a stray
  bit in it: 159 for 31. The compare now goes before the add.
- **The rig's timing.** A `CPX #4` inside the 32-bit subtraction loop
  ate the borrow, and for a while routines cost ±855,000 cycles a call.
  The loop is unrolled now.

### Two bytes, and the carry

The multiplies used to fill `acc`, three bytes, because the C's products
are 17 bits wide. Rich asked whether they needed to be. Nowhere, it
turns out:

- `gradq8_mag`, both near paths and `project_col_q8` never get there.
  Their own asserts keep the product under 2^15.
- `div_t` gets there 4.1 million times over its domain, and every one of
  them saturates. It shifts by 8 at most afterwards, so a 17th bit is
  t > 255.
- `div_cols`, `div_q8` and `project_q8` need the bit for exactly one
  instruction: the first right shift brings it in.

So the 17th bit stays where the ADC left it, in C:

```
    MUL_RECIP16 num, q          ; q: 16 bits, C: the 17th
    ACC_SHR17 q                 ; the first ROR takes C, the rest are 16-bit
```

The accumulator is now the caller's own variable, passed in, so `acc` is
gone. The shifts keep the low byte in A, since `ROR A` beats `ROR acc` by
3 cycles a bit. A host check over every routine's whole domain backs
each claim. It also caught a wrong comment: `project_col_q8`'s
pre-shifted numerator isn't "under 2^14" but up to 16,511, because d's
normaliser truncates. Harmless, since the product's still under 2^15.

The C does the same now (`prod17` in arith.c, and the top-level
JOURNAL.md). Against the old C: 19 billion cases, every golden frame and
every camera stream, all identical.

Full S2 runs on the same inputs, before and after:

| | before | after |
|---|---|---|
| mul_recip8 | 55 | 48 |
| mul_recip16 | 113 | 102 |
| div_t | 310 | 252 |
| div_cols | 356 | 306 |
| div_q8 | 310 | 297 |
| gradq8_mag | 264 | 225 |
| project_col_q8 | 328 | 308 |
| project_q8 | 192 | 172 |

Everything else is within a cycle of where it was. `project_col` and
`project_rowd` only changed in their near paths, which S2's random
depths hardly visit. `grad_mag` kept its own loop, which beats the
general shift over the short shifts its column counts give. And 2,321
bytes became 2,241.

### Budget checkpoint

arith isn't in the release yet, since nothing there calls it. When it
goes in, it takes the release's code and data from 1,510 bytes to 3,751,
of the 16,128 between &F00 and &4DFF. Zero page: 8 bytes for good, the
multiply's pointers, whose pages are set once at start-up. Everything
else is overlaid by Baron. (In the harness the routines' outputs get
bytes of their own, because S2's timing runs read them without writing
them.)

### Phase 2: done

S2 passes in full on b2, 104 runs, every routine byte-exact with the C.
It found a fast path's bug on the path's first run, which is what it's
for.
Next: mode 5.

## 2026-10-01 — Phase 3: mode 5

Clears, fills and the cursor, all in `src/mode5.6502`, and all checked
against mode5.c on both buffers.

### The fill

One body per buffer, unrolled. For each byte column it's an `LDA`, then
63 times `EOR : STA`, down the even scanlines and then the odd:

```
    LDA base + s, X             ; the column's first: nothing above it
    EOR base + r * 256 + k, X
    STA base + r * 256 + k, X   ; ...and so on, 63 times a pass
```

X is the column's block, so no `abs,X` ever crosses a page: 9 cycles a
byte, 37,035 a buffer measured, against the plan's "about 37k". The loop's
`CLC` sits outside it (Rich's catch). The body never touches C, and the
loop only goes round without a carry, so clearing it 32 times was 64
cycles a fill for nothing. 775 bytes a buffer.

### The clear

Moved over from hw.6502 unchanged: `STA page,Y` for 16 pages, 256 times
round. 21.9k cycles.

### The cursor

The C plotters' `pt_mode5_cur`, as the journal sketched it: a block
address in zero page and Y the row's even scanline.

- `AT c, page` places it: A is the canvas row, X the pixel column.
- `ROW` EORs a pattern pair in, under a mask that's a variable or a
  constant (`ROW blk, #&FF, pe, po` drops the `AND`), and leaves Y on the
  odd scanline.
- `DOWN` and `UP` move a row from there.
- `NEXTBYTE` moves a byte right. Plain `NEXT` is Baron's: it ends a `FOR`.
- `RIGHT` moves a pixel right.

`ROW`, `DOWN` and `UP` leave C alone, for the plotters' fractions.

mode5.h's comment says `AT` is two tables by row. It's cheaper without:

```
    TAY
    CMP #&80 : ROR A            ; row ASR 2, the sign kept: the character
    CMP #&80 : ROR A            ; row, -1 in the junk row above
    CLC : ADC page : STA c + 1
    TYA : AND #3 : ASL A : TAY  ; (row MOD 4) * 2
    TXA : ASL A : AND #&F8 : STA c
```

That's 35 cycles and no bytes; the tables would save two cycles for 144.
`RIGHT` only reaches the next block after its `LSR` has set C, so it
adds 8 with `ADC #7`.

The page is a parameter, like the multiplies' accumulator: whoever plots
says which buffer.

### S3

An image is a buffer with its junk row either side, 4,608 bytes. The host
pokes a random one, the Beeb runs an operation on it, and the host peeks
it back and compares every byte with `beebtool s3`'s.

The cursor gets four walks: a plot at every pixel of every row, then down
every column, up every column, and along every row, rows -4 to 67 each
time. Each walk reaches every pixel pair of the image exactly once, with
a pattern of its own, so a misplaced EOR can't be cancelled out by a
second one.

All pass, 8 images an operation on each buffer. To check it can fail, I
broke each macro in turn:

| broken | caught by |
|---|---|
| `AT`: row MOD 2 for MOD 4 | plot, up, right |
| `DOWN`: no step to the block below | down |
| `UP`: the wrong scanline above | up |
| `RIGHT`: the carry not counted | right |
| `AT`: a logical shift, losing the sign | plot, down, right |

Down misses the first one because it starts at row -4, where MOD 2 and
MOD 4 agree. Each break is caught by the walks that use it. The last
shows the junk rows really are compared: plot and right differ only in
the junk row above, about 256 bytes, while down starts its walk there and
goes wrong all the way down.

### B moves up a page

Rich's: &8000 is the paged ROM, and a store there goes nowhere. B's junk
row below only ever takes a line's overrun, which nobody reads, so the
ROM can be that junk row. B moves up to &7000, A to &5F00 and the shared
junk row to &6F00, and a page comes free (more BSS for now).

The plotters still read the junk row before they write it (`EOR (c),Y :
STA (c),Y`), so overruns into &8000 read ROM, EOR it, and store into
nothing. The C's junk row gets the real EORs, and nobody ever looks.

S3 can't poke that row any more, so on B it compares the buffer and the
junk row above, then checks &8000-&80FF still reads what it read before.
The cursor walks write rows 64-67 straight into it, and it doesn't
change, so b2's Model B really has a ROM there. Both start addresses are
still below &1000 (&0BE0 and &0E00), so the hardware wrap stays out of
it. A screenshot in jsbeeb shows each buffer whole, bottom row included.
S0, the IRQ, the soak and S3 all pass again; the harness image now runs
to &5DFF.

Rich's second idea: the RAM junk rows, &5E00 and &6F00, are 512 bytes of
scratch whenever the plotters aren't running. Capture can keep visit
fields there, and the sprite pass its toggles, though not the pool or
anything else that has to survive the plot. That's D13, for Phases 6-7.

### Budget checkpoint

mode5 is 1,664 bytes, nearly all of it the two fills. The release's code
and data come to 3,064 bytes, or 5,305 with arith, of 16,128. BSS has
4,096 bytes now, up a page.

### Phase 3: done

Next: the plotters, and the frame-rate go/no-go.

## 2026-10-01 — Phase 4: the plotters, and the go/no-go

### plot

`src/plot.6502` has `plot`, which runs over the pool from `pool_last`
down to 1. EOR commutes, so the order's free. It skips uncoloured pieces
and pieces sitting on the bottom edge that don't rise, and hands each of
the rest to one of three plotters:

- **Flat.** Y becomes the row's offset from its page, eight a byte. A
  whole byte is `ROW`, then `TYA : ADC #7 : TAY`, then `CPY last : BCC
  full`. The compare's carry is the clear carry the next `ADC` wants:
  42 cycles a byte.
- **Shallow.** The journal's carry tree for the whole bytes, and a pixel
  at a time for the ends.
- **Steep.** A pixel pair, then the gradient's whole rows, a row at a
  time, then the fraction's carry.

Rising lines needed no code of their own. The C's borrow is `f < gm`,
which is exactly the carry out of `(255 - f) + gm`, and the fraction
stays complemented afterwards. So a rising line keeps 255 − f, starting
at 127 rather than 128, and runs the falling code, moving up instead of
down. One macro does both directions, where the sketch had two, with
`SBC`.

Nothing in a loop clears C. The carry is known at every step: the tree's
leaves add `gm - 1` when it's set, flat lines get theirs from a compare,
and the pixel loops from the mask's `LSR`.

1,910 bytes, most of them the two trees (about 1.15K, as the sketch
said).

### S4

The oracle renders every golden pose, keeps its pool, plots that alone
into a zeroed buffer, and CRCs the result. The host packs the pools into
the harness's pad, which is free once S0 has checked it. The Beeb unpacks
each one into the pool pages, zeroes the image, times `plot` and CRCs the
image. When a pool doesn't match, the host splits it into single pieces
and names the first one that fails on its own.

That's all 20,409 golden poses, 3,000 random single pieces and 40 full
pools of 255 pieces, on both buffers. All identical, first time. So I
broke it five ways:

| broken | sweep frames that differ | named |
|---|---|---|
| a tree leaf a pixel short | 417 | a shallow piece |
| the wrong mask after a byte, pixel by pixel | 626 | a shallow piece |
| a flat end mask | 328 | a flat piece |
| steep's fraction carry dropped | 273 | a steep piece |
| rising lines starting at 128, not 127 | 453 | a rising piece |

Each break was caught, and the piece named was the kind broken. The last
is a rounding of one in 256, and it still shows up in 453 of 1,172 frames.

Measured, the plot takes 10.2k cycles a frame on the sweep, 8.0k on the
rims, 10.4k on map 7 and 11.6k on the stress map; the worst pose is
40.8k. The cost model had guessed 7.1k to 9.3k.

### Steep, a row at a time

Rich asked whether steep lines recompute the address for each column.
They don't: `AT` once, then only deltas. But they step a row at a time,
14 cycles a row. Adding the whole delta at once costs 38 cycles a column
however steep, or 46 rising:

```
    TYA : ADC g2 : AND #&FE     ; the target even scanline, from this block's top
```

Its low bits are the new Y and the rest are pages to add. It wins from
two or three rows a column. The goldens have 2.7 steep columns a frame,
and 88% of them move one row or two. Picking per line would save 6
cycles a frame, so it's not done.

### scrnbase, and the buffers move again

Rich's: `scrnbase`, a page, is the buffer being drawn, and clear, plot
and fill all work on it. The main loop sets `swapreq`; the IRQ shows the
buffer and turns `scrnbase` to the other one. With A at &5800 and B at
&7000, both multiples of &800, the CRTC's start address has a low byte of
0 for both, so R13 never changes:

```
    LDA #12 : STA &FE00
    LDA scrnbase : LSR A : LSR A : LSR A : STA &FE01
    LDA scrnbase : EOR #(&58 EOR &70) : STA scrnbase
```

Clear and fill keep a body per buffer, with the addresses as operands,
and pick one with a compare on `scrnbase`. One body through a pointer
would cost 2 cycles a byte, about 8k a fill.

The gap is good RAM. &6800 is A's junk row below and &6F00 B's above, and
&6900-&6EFF between them is never touched by anything but us: 1.5K. A's
junk row above (&5700) shrinks BSS below the screens to 2.25K, but the
gap more than pays for that. The visits and the rest of capture's scratch
fit the three junk rows, and six of the pool's eight pages the spare. So
`BSS_BASE` can rise to about &5100, which gives code 768 bytes more
(D13). Phase 7 settles it.

S0, the IRQ timing, the soak, S3 and S4 all pass again, and jsbeeb shows
both buffers whole.

### The go/no-go

`host/gonogo.py` (`make gonogo`) puts a frame together:

- capture from the cost model's counters, with S2's measured arithmetic,
  guesses for the rest, and 15 cycles a call;
- plot as S4 measured it, pose by pose;
- sprites guessed;
- clear and fill, 58.9k between them;
- the camera at 5k.

| map | median | 90% | max | fps at the median | at 90% |
|---|---|---|---|---|---|
| 0 | 91k | 164k | 292k | 22.0 | 12.2 |
| 7 | 109k | 191k | 288k | 18.3 | 10.5 |
| 9 | 117k | 207k | 315k | 17.1 | 9.7 |

The plan's guess for map 0 was 90k at the median and 151k at 90%: close.
Frames are shown at vsyncs, though, and then it matters how the main loop
waits. Over each map's frames in order:

| map | post, then wait | capture during the wait |
|---|---|---|
| 0 | 16.0 fps | 18.2 fps |
| 7 | 14.2 | 16.3 |
| 9 | 13.5 | 15.2 |

The camera and capture never touch a buffer, so they can run while the
last swap is pending. Waiting just before the render, not straight after
posting, buys about 13%. That's D14's open question, for Phase 9.

Capture is the biggest piece, and still mostly guesses. Phases 5 and 6
will say. The numbers go to Rich before Phase 5.

### Phase 4: done

Next: vertices, lines and the clip.

## 2026-10-01 — Phase 5: vertices, lines and the clip

### What's there

`src/clip.6502` (1,887 bytes) has everything that cuts and clips:

- rows: `row_step`, `piece_y`, `line_y`;
- the pool: `pool_alloc`, `pool_split`, `piece_at`, `cut`;
- chain building, and the closed list;
- the clip group: `emit`, `resolve_block`, `clip_line`, `clip_edge_line`.

`src/lines.6502` (1,450) has `transform`, the column tests, the side
clips, `make_line` and `edge_line`.

The shapes:

- **One clip context, in zero page.** Only one clip runs at a time, so
  render.c's `pt_clip` is globals: `cx_*`, the chain being built `cb_*`
  and the line `l_*`. There's one builder block. Phase 6's `process_edge`
  will save the floor builder's four bytes before the ceiling clip reuses
  it.
- **No visit pointer.** `cx_act` replaces the C's visit and slot: an index
  into `vis_act`, a slot's 24 visits at a time.
- **Pairs, indexed by X.** An edge's two vertices (`ev_*`) and two
  endpoints (`ep_*`) are two-entry arrays, so the near and far endpoint,
  or v0 and v1, are one index apart.
- **`piece_at` keeps the column in A** by walking with X and Y in turn:

```
.loop
    CMP p_x1,X : BCC donex      ; c < x1: this one
    LDY p_next,X
    CMP p_x1,Y : BCC doney
    LDX p_next,Y
    JMP loop
```

### Unit replay

`host/replay.py` decodes a record and pokes the pool and the visits'
active pieces straight into BSS. Everything zero page goes through a
staging block, which the shim (`test/s5.6502`) unpacks before the call
and packs back after. The host pokes nothing in zero page itself, so
Baron sees every write. Then the after half is re-encoded the C's way and
compared byte for byte.

The recorder gained five targets: `make_line`, `edge_line`, `transform`,
`col_gt` and `col_ge`. That's 4,824 records in all, every one
reproducing itself on the host. REPLAY.md had CAM at 8 bytes; it's 9.

On the Beeb, all 3,617 of Phase 5's records come out identical. So does
every map vertex at all 20,409 golden poses (S5), and 50,000 synthetic
`make_line` calls.

The synthetic ones exist because the goldens hardly ever clip a line at
the near depth with the clip column rounding left of the near endpoint:
the random endpoints reach it 61 times in 50,000. They also showed the
other near-clip line host/uncovered.txt called rare is dead. The start
row is at most 16,512 + 32,639, so its "carry out: clamp" never
carries. Dead code isn't ported (D10), so it's gone from the Beeb, and
from the list's "rare".

The shim times each call alone, so the replay gives capture's first real
costs:

| | median | mean |
|---|---|---|
| `transform` | 768 | 829 |
| `resolve_block` | 1,060 | 1,235 |
| `clip_line` | 536 | 709 |
| `clip_edge_line` | 652 | 973 |
| `edge_line` | 804 | 820 |
| `make_line` | 570 | 605 |
| `emit` | 230 | 273 |
| `cut` | 340 | 209 |

They're per call, not per frame, and the records are picked for novelty,
not frequency. Where they can be held against the cost model, they
agree. `transform` is about 720 for a vertex outside the frustum and
about 1,000 inside, where the model has 664 and about 1,050. (Its
rotation, reciprocal and column are S2's, counted apart.)

### Breaking it on purpose

Eight breaks:

| broken | caught by |
|---|---|
| `emit`'s closed-range test, off by one | `emit`, `resolve_block`, `clip_line`, `clip_edge_line` |
| `piece_y`'s rounding bias, a unit low | the same four |
| `chain_extents` rounding up a unit high | the same four |
| `transform`'s column outside the frustum, 63 | `transform` |
| `clip_left`'s column, 1 | `edge_line` |
| `col_ge` one column short | `col_ge` |
| `crossing`'s "at least 1" made 2 | nothing: it's dead |
| `make_line`'s clip-column fraction, a unit off | the synthetic set, 137 of 50,000 |

The `crossing` one was in host/uncovered.txt as dead already. I'd ported
it anyway, so it's gone. The fraction one gets past all 787 recorded
lines, and that's what the synthetic set is for.

### The save stack

Rich's: the walk should save on the hardware stack, `LDA : PHA` before
the recursive `JSR` and `PLA : STA` after, not into arrays in page 1. The
spike's case for the arrays was the hardware stack's depth. But page 1
holds both either way, about 195 bytes at depth 16, whichever it is.
Pushes are about 20 cycles a descent cheaper, and simpler. Baron takes
them: the `STA` after the call kills the old value, so nothing in zero
page is live across it. So Phase 6's walk pushes (D15).

### Budget checkpoint

Called from the release, everything so far would be about 10.5K of code
and data: Phase 0's 1.5K, the fills, arith, plot, clip and lines. Phase 5
came to about 5.9 bytes a line of C. At that rate the rest comes to:

| phase | estimate |
|---|---|
| 6, walk and capture | 2.0K |
| 7, sprites | 2.0K |
| 8, camera | 1.6K |
| 9, the main loop and keys | 0.5K |

That's about 16.6K, against 16,128 with `BSS_BASE` at &4E00: about 500
over. With D13's placement it's about 16,900, which leaves about 300 to
spare. Either way it's right on the line. If it comes to it, §2.4's
fallbacks come cheapest first. Sizing the instance array to the map saves
about 360. One fill body with patched operands saves about 400 (a body
is 775, less the patching). Or there's the 16K of sideways RAM.

### Phase 5: done

Next: the walk, with its saves on the hardware stack.

## 2026-10-01 — Phase 6: the walk

### What's there

`src/walk.6502` (2,199 bytes) is the rest of render.c's capture:

- the visits;
- `child_portal`, `next_open` and `close_columns`;
- `process_edge` (841 of the bytes);
- the root search and its exact retry;
- `walk`;
- `capture`, the frame's half of `pt_render_frame`.

Sprites wait for Phase 7, so nothing calls `inst_new`, `seg_record` or
`inst_issue` yet.

The shapes:

- **The portal is zero page.** `po_*` is the one being walked, and `ch_*`
  the child `process_edge` finds. A descent is a copy, some pushes and a
  `JSR`:

```
    LDA po_cell : PHA           ; ...the portal's seven bytes, then
    LDA steps : PHA
    JSR take                    ; ch_* into po_*
    INC depth
    JSR walk
    DEC depth
    PLA : STA steps             ; ...and back
```

  That's 8 bytes a level, until Phase 7's pending count makes it D15's 9.
  The tail call is `JSR take : JMP portal`.
- **A visit is 19 tables of 24, in the spare**: cell, k, xr, then v0's
  and v1's eight bytes each. With `cell_visit`, that's 480 bytes, there
  until D13 decides where everything lives. Release BSS didn't have room.
- **`process_edge` keeps its colours and heights in locals**, and
  `edge_clip` does the dull part of each line: the active piece's index,
  fresh cursors, `clip_edge_line`. The C's three `pt_clip`s come down to
  which fields get set when: the clip only ever writes the cursors.
- **The root's bounds** come from `pool_alloc`'s own indices, not an
  assumed 1 and 2, so the abort check (below) can start the pool
  anywhere.
- **`root_exact` needs only the sign** of a = rx k1 + ry k2. But it needs
  it exactly, so it gets a 16 by 16 signed multiply (194 bytes, for a
  path only boundary poses take). On a convex ring the sign changes once,
  so it stops at the first transition. The camera's multiplies in Phase 8
  may want to share it.

Folded in, or gone:

- An edge starting past the right ray has an sx0 of 128, which is its xa,
  so it has no columns. `process_edge` says so at once, rather than via
  render.c's arm for it.
- Two arms I ported in Phase 5 are dead now there's a `process_edge` to
  prove it (D10): `cut`'s skip past a lagging cursor (render.c:752-754)
  and `edge_line`'s start past the right ray (1095-1096).
- `next_open` never needs its `l == c` test. Once the first loop is done
  nothing covers c, so nothing starts there either. The mutations said so
  first; see below.

### The overflow (D9)

`OVERFLOW n` sits wherever the C panics: the pool (2), the closed list (3)
and the visits (5). In the release it's `JMP capture_abort`: S back to
where `capture` saved it, `cell_visit` emptied, `INC aborts`, and C set.
In a test build it stops with the code, unless the check sets
`ovf_abort`.

The abort check starts the pool high (`ovf_pool`) so about half of 16
frames run out of pieces. It passes: 5 dropped, exactly the 5 whose pools
were too big, 5 counted, `cell_visit` empty, and the next 16 frames
exactly as the C draws them.

### Baron's turn

The walk wouldn't assemble. Baron's recursion check refused `JSR capture`,
naming `pool_last`, `num_visits`, the rotation's outputs and five more.
Two sound shapes were getting refused, each with a 20-line repro:

- A routine outside the cycle seeds a count, then calls into the
  recursion, and its caller reads the count after. That's `capture`
  seeding `pool_last`.
- A helper's result is used within one level, and read again by `main`
  after the whole recursion returns.

The check ran at every call whose callees recurse, and its live set
counted reads by whoever runs after the outermost return. Rich OK'd a
narrower rule. A value is per-level only if a level reads it back after
its own recursive call, so a seed before the recursion and a result read
after it are one value each.

So the check now:

- runs only at a call that can come back round to itself;
- judges on liveness with the return edges left out.

That also caught a per-level value it used to miss: one stored on a
guarded path. Baron's tests: 445, green under asan. Its examples and the
release disc were byte-identical either way. Rich committed it.

### The harness ran out of room

The release code plus every check came to 1,833 bytes past &5700:

| | Bytes |
|---|---|
| Release code so far | 12.7K |
| Test code | 7.5K, against the 2.5K the plan budgeted |
| of which S2's drivers | 3.3K |

So it's a suite a harness now (Rich):

- `-D SUITE=2` is S2 alone, `harness_s2`.
- `SUITE=0` is the rest: `harness0`, `harness1` and the map builds.
- The Phase 1 spike is gone, since the real walk has done its job.
- The CRC tables moved to `test/crc.6502`, as every suite folds with
  them.

S4's full pool (1,786 bytes) no longer fits the 1,750 of I/O area left.
A pool that big is poked straight into the pool's pages and plotted in
place.

### Checking it

S5u replays the walk's recorded calls with the visits poked in, and all
701 match. They had to go to the right build first, though. A record's
map is its CAM's first byte, and the files mix all three. Phase 5's
targets never read the map, so it never mattered. `child_portal`'s
record 108 is a map 9 record, and on map 0 it walked a ring that never
ends.

S6 renders whole frames, sprites off, at every golden pose, and CRCs the
capture (pieces, visits, `cell_visit`) and the image:

| | Poses | |
|---|---|---|
| Map 0, buffer A | 7,545 | identical |
| Map 0, buffer B | 7,545 | identical |
| Map 7 | 2,344 | identical |
| The stress map | 10,520 | identical |

That's 27,954 frames. The first run said 1,160 of the sweep's 1,172
differed, while pose 0 alone was fine: `s6_out`'s index is a byte, and 64
records of 16 is 1,024. Batches of 16 now.

`close_columns` splits a piece at l or rr only for pinches no pose finds
(host/uncovered.txt: rare). So `beebtool synthclose` writes 4,000
synthetic calls on random chains, 89% of them splitting, and all 4,000
match.

Breaking it on purpose:

| Broken | Caught by |
|---|---|
| The depth cap a level early | S6 on the stress map (78 of 1,500) |
| The top chain's `bmin` always 0 | `process_edge`'s replays, and every S6 set |
| `visit_advance` keeping the floor bound's active piece | S6: 418 of the sweep's 1,172 |
| The root search missing a vertex exactly on the left ray | S6 on the rim poses (8 of 1,500) |
| `cell_visit` left full | S6: the next frame hangs, or overflows the pool |
| `close_columns` not splitting at rr | the synthetic set (2,938 of 4,000), nothing golden |
| A first visit starting at the twin | nothing: equivalent |
| `next_open`'s end test taking l == c | nothing: equivalent |

The twin one undoes itself. Its v1 is P, which is never strictly right of
xl, so the seek's first step lands where the C starts, one transform
later. The `next_open` one is the redundant test above, now gone.

S3 and S4 pass again on the new harness.

### What it costs

Capture, measured against the cost model's figure for the same poses
(cycles):

| | Model median | Measured | Model 90% | Measured | Model max | Measured |
|---|---|---|---|---|---|---|
| sweep | 26.9k | 39.2k | 78.6k | 114.5k | 177k | 264k |
| map7 | 29.9k | 42.2k | 83.5k | 114.9k | 185k | 264k |
| rim | 18.9k | 27.0k | 72.9k | 105.3k | 181k | 261k |
| stress | 41.4k | 57.9k | 117.5k | 156.1k | 207k | 283k |

It runs about 40% over the model everywhere. Where the model guessed (60
for `process_edge`, 160 for `child_portal`), the 6502 says 3,852 and
1,520 a call, though those include the clipping and transforms they
call.

A whole frame (clear, capture, plot, fill, no sprites or camera) is a
median of 108k on the sweep, 128k on the stress map, and at most 368k. At
the median that's 3 vsyncs, 16.7 frames a second, before sprites and the
camera.

Zero page reaches &B5 in the harness (it was &95). The stack never gets
below &153.

### Budget

The walk is 2.2K against Phase 5's estimate of 2.0K. The projection
becomes about 16.8K, against 16,128 (or about 16,900 with D13). Still
right on the line, and Phase 7 is next.

### Optimisation: later

Rich: a pass of its own, after the sprites. Pricing every recorded call
both ways shows where the model's 40% went. Where it used S2's measured
arithmetic it's right: `transform` 0.91 of the model, `child_portal`
1.08. Where it guessed it's short: the clip group up to 1.68
(`clip_edge_line`), `edge_line` 1.40 and `process_edge` 1.43. It missed
the glue: `row_step`'s setup under every `piece_y` and `line_y`, the
chain helpers, arguments copied into zero page, each line's context, and
the vertex copies per edge.

On the sweep, roughly:

| | Share of capture |
|---|---|
| clipping lines | 36% |
| transforms | 26% |
| making lines | 23% |
| cuts | 9% |
| `child_portal`'s own | 6% |

The candidates so far:

- the `transform` trims, about 150 cycles a vertex;
- reading the visits' tables in place, not copying 22 bytes an edge;
- the vertex cache, if it comes back;
- whatever a sampling profiler finds in the clipping (system VIA T1
  every 2,000 or so cycles, the interrupted PC to the host, mapped to
  labels).

### Phase 6: done

Next: sprites.

## 2026-10-01 — Phase 7: sprites

### What's there

`src/sprites.6502` (2,653 bytes) is the rest of render.c, the sprites
(spec §9):

- **`inst_new`** projects a cell's sprite on the cell's first visit: its
  scale, its corner and column cursor in 24 bits, and the columns it
  covers. Behind, too near or wholly off the screen, it's instance 0, the
  C's null one. Most calls find no sprite at all: a median of 24 cycles,
  a mean of 343.
- **`seg_record`** runs as the walk enters a portal in a cell with a
  sprite. It records the bound pieces over the columns the portal and the
  sprite share, while the chains are still whole. A segment is 5 bytes
  plus one a piece, from a 256-byte pool.
- **`inst_issue`** runs as a walk call pops. The instances its portals
  left pending go to the tail of the draw order, innermost first (spec
  §9.4).
- **`sprite_pass`** runs between the plot and the fill. Per byte column,
  it reads the plotted toggles into a list, paints each instance's opaque
  runs over them as more toggles, clipped by its segments, and plots the
  list back.

The walk's side is three calls and a byte:

```
.walk
    LDA num_pend : STA pend     ; this call's portals leave theirs above it
.portal
    JSR seg_record
    ...
    LDA pend : PHA              ; D15's ninth byte
    JSR take
    INC depth
    JSR walk
    ...
.issue
    LDA pend
    JMP inst_issue
```

And `visit_new` calls `inst_new`. Four new overflows: the instances (7),
the segments (8), the pending stack (9) and a byte column's toggles (10).

### D13, placed

`BSS_BASE` goes up to &5100:

| Where | What |
|---|---|
| The junk rows | the visits' 20 tables, `cell_visit`, the pending stack, the active pieces, the closed list |
| The spare, &6900-&6EFF | six of the pool's eight pages |
| BSS, &5100 | the pool's other two, the segments, the instances, their row edges, the toggles: 1,312 bytes |

The junk rows are capture's only until the plot overruns into them. So
capture zeroes `cell_visit` at its start, where the C clears it at the
frame's end. Test builds CRC the capture before they zero the junk rows
for the plot, so the whole-frame checks still compare the overruns.

### The harness, again

With the sprites in, suite 0 came out 959 bytes past &5700. So:

- S3 and S4 have a suite of their own, `SUITE=3` (`harness_s34_0` and
  `_1`).
- The CRC tables are built at start-up by `crc_init`, not assembled:
  512 bytes back.

S6's record now times the capture, the render (plot, sprites, fill) and
the clear separately, and takes the state's CRC before the plot. Later
the profiler needed room as well, and the frames got `SUITE=4` (next
entry).

### The tenth pose

The first frames with sprites stopped with error 6: `root_exact`'s
fallback, the one that can't happen. One pose at a time, all 16 of the
batch matched. In a batch, the tenth pose failed, and every one after it,
whatever they were. Pose 9 twelve times over failed from the tenth copy
on.

The host writes a batch's poses into `io_area`, which was the pad after
the code. The sprites had brought the code to &56C7, 57 bytes short of
&5700, and 16 poses are 96 bytes. From the tenth on they were in junk0,
where capture keeps its visits. Each frame wrote its visits over the
poses still to come.

`io_area` is in test BSS now.

### Read before run

Four bugs came out of reading the code back before it ever ran:

- `step1`'s upward step stored the gradient's high byte over the row's,
  then subtracted it from itself.
- A pixel column ending at the art's row 32 went through `h << 3`, which
  is 256. In a byte.
- `bstep` indexed the segment with a Y it had just lost.
- `seg_record` indexed its new segment from the pool's next free byte,
  less 5. For the pool's last segment that wraps to 0, and it would have
  written in front of `segs`.

Baron had its say too: two `LDY abs,Y`, a mode the 6502 never had, and
some branches out of range.

### Checking it

The replays (S5u) now cover the sprite functions, and the walk's
replays carry the full sprite state:

| | Records | |
|---|---|---|
| `inst_new` | 179 | identical |
| `seg_record` | 185 | identical |
| `inst_issue` | 142 | identical |

All 17 targets pass. S7 renders whole frames with sprites on, and the
sweep's 1,172 matched on Phase 7's own code. The full run came after the
optimisation pass (next entry), on its code:

| | Poses | |
|---|---|---|
| Map 0, buffer A | 7,545 | identical |
| Map 0, buffer B | 7,545 | identical |
| Map 7 | 2,344 | identical |
| The stress map | 10,520 | identical |

That's S6's 27,954 frames again, sprites and all. The abort check passes
with sprites on: 5 of 16 frames dropped, 5 counted, and the next 16
frames as the C draws them. It no longer checks that `cell_visit` is empty,
since capture empties it at the start now.

The stress frames take the stack to &146, the ninth byte at work (S6's
reached &153).

Not broken on purpose yet. The mutation round is written but has never
run: it aims at suite 0's builds, which don't render frames any more.
It's owed.

### What it costs

On the sweep, with Phase 7's code (cycles):

| | Sprites off | Sprites on |
|---|---|---|
| Median | 109k | 109k |
| 90% | 196k | 284k |
| Max | 364k | 648k |

Most frames barely notice the boulder. Get close to it and the worst
frame nearly doubles. That's where the optimisation pass went first.

### The demo

Rich wanted to see it run once the sprites were code complete: the
camera fixed at the map's start, turning an angle unit a vsync. So the
release learned to render. Everything from arith to the sprites came out
of the harness-only block, and the main loop became the demo. The angle
at each capture is the vsync count, on from the start's, so it turns at
50 units a second however long the frames take.

`make demo` runs it in b2 at real speed, in our own config folder. (The
checks' b2 runs flat out, which would spin it like a top.) `MAP=7` gives
it seven sprites.

### Budget

The sprites came to 2,653 bytes against Phase 5's estimate of 2.0K. With
the renderer in, the release runs to &4AB1: 1,614 bytes under &5100.
Phases 8 and 9 were estimated at 1.6K and 0.5K, so the projection becomes
about 17.4K against D13's 16,896: about 500 over. It's off the line now,
on the wrong side. §2.4's fallbacks, cheapest first, are still the way
back.

Every harness carries the release, so the camera's 1.6K comes out of
each of them as well, and most haven't got it. The tightest,
`harness0_map7`, has 383 bytes left today.

### Phase 7: done

Next: the camera. But first, a detour.

## 2026-10-01 — Optimisation: the first pass

### The profiler

The SUITE 4 harnesses carry a sampler. The system VIA's T1 fires every
997 µs (1,994 cycles), and while `prof_on` says a frame is running, the
IRQ takes the interrupted PC off the stack and into a 1,760-byte buffer.

`host/profile.py` runs the S6 poses with sprites on (the sweep, a quarter
of the rim, map 7, a quarter of the stress set: 7,739 frames). It maps
each PC to its instruction in the listing and reports by routine, by
label, by overlapping 64-byte window and by instruction.
`profile.py bench` runs the same poses unsampled, for a before and after.

### Where it went

| Before | Share |
|---|---|
| fill | 24.5% |
| clear | 14.4% |
| sprite pass | 12.6% (24.7% on map 7) |
| plot | 6.8% |
| `rot` | 4.9% |
| `process_edge`'s own | 4.4% |
| `transform` | 2.7% |
| the next thirty | 0.5 to 2.4% each |

Fill and clear are at the floor: 9 cycles a byte (`EOR abs,X`, `STA
abs,X`) and 5.3. That's 40% of every frame I can't touch.

Everything else is spread thin. No silver bullet, just a lot of lead ones.

The sprite pass is the odd one out: cheap most of the time and dreadful
up close. A sprite filling the screen made a 648k-cycle frame on the
sweep and 763k on map 7.

### What changed

The ones that mattered:

- **Pixels that paint alike paint together.** A close sprite is scaled
  up, so a byte's four pixels come from one or two source columns. Each
  pixel's paint only ever touches its own bits, so pixels with the same
  source column and clip rows can share one paint under a wider mask.
  And clip rows past the art's own first and last rows can't change a run
  (rows only increase), so they're clamped to them first. That makes most
  of a close-up's pixels agree:

```
    LDX gm : BEQ new
    CMP gcol : BNE other        ; the waiting pixels' column and rows: this one joins
    LDX top : CPX gtop : BNE other
    LDX bot : CPX gbot : BNE other
    LDA c : AND #3 : TAX
    LDA pixmasks,X : ORA gm : STA gm
```

- **`rot` skips the high multiply** when a coordinate is under 256. In
  the recorded transforms that's 68% of x and 88% of y. A transform's
  median went from 758 cycles to 660.
- **The sprite pass's toggles are a linked list**, so inserting and
  removing cost the same anywhere. Shuffling the array was 6% of a
  close-up. The toggles above every sprite in a byte column fold into the
  list's head, which only the prefix ever reads.
- **The sprite pass reads a byte column's blocks** (4 rows, 8 bytes) with
  one OR each, since most are empty, and counts them down.
- **`row_step` has a shallow path**: a gradient under 1 is one multiply.
- **`col_gt` and `col_ge` read a visit's end vertex in place**, rather
  than having it copied into `cv_*` first. `vs_cv1` is gone.
- **`process_edge` copies the edge's vertices as far as they're needed**:
  flags and columns for the span, the start for a child, the end only for
  lines.

And smaller ones: `pool_alloc` inlined; the pool's pages written
directly; `project_col` skipping its high multiply too; `transform`'s
seams only tested on a zero sum; `edge_line` handing `cx_act` to the
clip; the sprite bounds stepped in place; the source column's rounding
folded into its start; the palette pointed one back so a shade is two
loads; and instances that miss a byte column skipped before anything's
set up.

### The numbers

Mean frame 152.6k cycles before, 138.9k after: 9% off. Capture is 11.5%
faster, the render (plot, sprites, fill) 9.7%, the clear not at all.

| | Mean | 90% before | 90% after | Max before | Max after |
|---|---|---|---|---|---|
| sweep | -10.0% | 284k | 236k | 648k | 409k |
| rim | -7.9% | 233k | 201k | 518k | 367k |
| map 7 | -11.1% | 329k | 284k | 763k | 505k |
| stress | -7.1% | 244k | 225k | 405k | 350k |

The worst frames gained most: a third off the close-ups.

### The demo's loop

The demo used to draw a frame whole, post it, and spin until the vsync
swapped it in: on average half a vsync gone every frame, to rounding.
Capture needs neither buffer, so now it doesn't wait for one:

```
.loop
    JSR capture                 ; the next frame, while the last one's swap is pending
.wait
    LDA swapreq : BNE wait      ; only the clear needs the back buffer
    JSR clear
    ...
    LDA #1 : STA swapreq
    JMP loop
```

Measured on b2, the demo's turning camera went from 3.36 vsyncs a frame
to 2.85: 14.9 frames a second to 17.5. Phase 9's loop should start out
this way.

### What it cost

354 bytes, most of them the sprite pass's. The release now runs to
&4C0D, 1,266 bytes under &5100. BSS has 128 bytes left.

That makes the projection for Phases 8 and 9 worse, not better. If bytes
get dearer than cycles, the first to give back is the head fold: 35 bytes
for 0.2%.

### Not done

- **The vertex cache.** The C transforms 13.1 vertices a frame on map 0,
  10.0 of them distinct (map 7: 14.1 and 11.8; stress: 21.3 and 16.7).
  The repeats are nearly all one vertex: Q, the far end of a portal,
  which the child's walk transforms again a moment after its parent did.
  A general cache costs more to fill than it saves. Reusing Q alone, from
  the parent's visit while it's still on that edge, would save 1.4k to
  2.8k cycles a frame for about 50 bytes and 72 of BSS.
- **`rot`'s per-frame half.** The angle's table values and signs are the
  same for every vertex in a frame. Hoisting them saves 30 cycles a call,
  0.35%, and touches the S2 and S5 shims.
- **Reading less of the toggle image.** The C reads every toggle down to
  the deepest sprite's bottom, and its capacity counts them, so the Beeb
  has to as well. Reading only down to the lowest visible row would be a
  spec change, on both sides.

## 2026-10-01 — The demo: a better view, another room, and a tear that wasn't

### Out of the boulder's way

Map 0's start looks straight down the passage, with the boulder bang in
the middle of it. So on maps 0 and 7 the demo's camera now stands 136 to
the right, at (1756, 1400) in cell 6: the closest it can stand and still
see the passage past the boulder. All 256 angles from there match the C (S7, sprites
on).

### Map 1: the second room

Map 1 is map 0 with a different start: (812, 1536) in cell 8, between
the pillar and the corridor back to the first room, facing the pillar
and the stairs at angle 150. As it turns, it looks back down the
corridor at the boulder from the other side.

- `beebtool`'s `start_pose` knows map 1, and `beebtool export` writes
  `data/map1.6502`: map 0's geometry, the new start.
- `make build/portal_map1.ssd` builds the disc. Any `portal_mapN.ssd`
  is the release on map N, and `make MAP=1 demo` runs it.

All 256 angles match the C. On b2 it does 13.9 frames a second with
nothing dropped. That's slower than map 0's 17.5, as there's more to
walk from there: capture's median is 93k cycles, against 22k at map 0's
demo pose.

### Even scanlines only? No

Rich asked: could the sprite pass read only the even scanlines of a byte
column, looking for anything not black? Half the reads.

Before the fill, a byte isn't a colour, it's a toggle, and the shades
are two-scanline dithers. So a toggle can be zero on its even line and
not on its odd. Shades 9 and 12 are both &50 on the even scanline, but
&A0 and &A5 on the odd: a line between them is a toggle of (0, &05). Half
of the shade pairs map 0 uses have a pixel like that.

Miss one and the colour under the sprite is wrong down to the column's
end, an erase can miss a world toggle, and the toggle count drifts from
the C's. And if it worked, it would save about 1% of a frame.

### The tear that wasn't

Rich saw what looked like tearing in b2, as if there were one buffer.
There are two, and the program keeps out of the one on show:

- 300 samples of map 1 running, each reading the shown buffer twice,
  with `scrnbase` and the swap stamp read either side. In the 184 with
  no swap in between, the shown buffer never changed by a byte. (The
  other 116 straddled a swap.)
- The swap is in the vsync IRQ, at row 26, after the 16 displayed rows.
  The 6845 takes the new R12 at the next frame's start, 13 rows later,
  and the back buffer isn't cleared until the IRQ has been.
- Nothing holds the IRQ off. The release has no `SEI` after start-up.

So the tear is probably b2's. It shows an emulated 50Hz picture at the
host's refresh rate, so a host frame can catch the top of one Beeb frame
over the bottom of the last. Phase 0's "(probably) doesn't tear" stands,
still with its "probably": a b2 screenshot of a tear would settle it.
Its config has `screenshot_last_vsync` set, which by its name takes the
last whole Beeb frame.

There is some judder, and it's honest. The camera turns a unit a vsync,
but the frames don't all take the same number of vsyncs, so on map 1 the
turn from one frame to the next goes 3, 4, 3, 3, 4.

### Not done

- **A demo without the boulder.** There's no such map, but skipping
  `sprite_pass` gives one: S6 already draws frames that way, exactly as
  the C does with sprites off. Capture still projects the boulder and
  records its segments, a few hundred cycles. A `-D NOSPRITES` in
  `main.6502` and the Makefile, if wanted.

## 2026-10-04 — The clip rework, on the Beeb

The C's capture changed under the port (the root JOURNAL, 2026-10-03 and
04): exact rows, twins, the run-by-run clip, the level skip, the portal
in the visit. Now the 6502's has too, routine for routine.

### What's there

- **The visit holds the portal.** `vs_x`, `vs_xr`, `vs_tcur`, `vs_bcur`,
  `vs_tmax` and `vs_bmin` join its tables, and the walk recurses on the
  visit alone. A level pushes 3 bytes (the visit, its edges left, the
  pending count), not 9.
- **Pieces keep their exact row.** `p_yf`, the fraction plus a half, is
  the pool's ninth page. A split never moves a row, and the plotter starts
  its fraction there, not at 128.
- **The clip** is render.c's: `own_piece`, `chain_add`, `merge` (twins
  and all), `flip`, `hide`, `meet`, `clip_line`, `merge_above`,
  `close_columns` and `run_end`, on one line cursor, `line_at`: the row in
  hand, one step on, or a product.
- **The walk** is `visit_of`, `enter` and `process_edge`, with the level
  skip.

Gone: `cut`, `emit`, `resolve_block`, `clip_edge_line`, `chain_extents`,
`chain_append`, the closed list (`closed_add`, `next_open`, `cl_*`),
`child_portal`, `visit_new`, `root_portal`, and the `po_*`, `ch_*` and
`cb_*` globals. So are two overflows, the closed list's (3) and the
visits' (5).

What render.c hands from call to call lives in zero page, where the
calls can share it: the range being placed and its row (`rg_*`), and the
line's and the bound's rows where a run starts (`rn_*`). `hide` and
`meet` move those on; `flip` and `merge` only read them. The bounds'
cursors are `cx_p + st`, so a side's status indexes its own.

### Memory

Everything the capture throws away before the plot came to 1,128 bytes,
against the junk rows' 768:

- the visits, now 29 tables. Rich's call: sized by the map's cells, 28,
  as a cell has a visit at most. So they can't overflow, and the check's
  gone.
- `p_next`, which only the capture reads now (the sprite pass reads
  segments, not chains);
- `cell_visit` and the pending stack.

Rich's other idea: things used one after the other can share. The sprite
pass's own state (its instances' column cursors, row edges and art, and
the toggles, 519 bytes) only comes to life after the plot. The capture's
is dead by then. So:

| Where | What |
|---|---|
| A's junk rows | 18 of the visits' tables |
| B's junk row above | `p_next` |
| the spare | six pool pages, as before |
| BSS, &5100 | `p_co`, `p_yf`, the segments and what the capture makes of the instances (873 bytes); then the overlap |
| the overlap | the sprite pass's state, or until the plot the capture's spill: the other 11 visit tables, `cell_visit`, pending, `in_segt`, `in_prev` |

Baron can't say union, so it's two sections over the same bytes: `Bss`'s
tables and `Spill`'s. `BSS_BASE` stays at &5100, with 144 bytes to spare.

Every field is a table of its own. The four active slots are four tables
end to end, so a line's slot table plus its visit is one index, `cx_act`,
worked out once a line.

The code grew 525 bytes: 250 of them the bigger map, 255 the clip. It
ends at &4E1A, 741 bytes short of &5100.

### Bugs on the way

Two of mine:

- `line_at` took column x0 as the line's anchor before looking at x1. An
  empty line (both ends too near, x0 = x1) is cut off at every column,
  x0 included. 80 of the 256 demo angles.
- `piece_at` doesn't keep A when it stops on an odd step (`TYA : TAX`),
  and `clip_line` thought it did. It walked off a chain's end and spun.
  The cure I wanted, `LDX p_next,X`, isn't an instruction, so the caller
  reloads A.

And two in the checks:

- The C replays a call into a fresh renderer, zero past the record's
  state. The Beeb's tables held the last record's, so a visit made by the
  call started from rubbish. The host pokes whole tables now, zero past
  the record.
- A visit never entered has no cursor, vertices or instance yet. The
  Beeb doesn't touch them until `enter` sets them; the C zeroes them when
  it makes the visit. S6's capture CRC counts them as 0 where `xr` is 0,
  on both sides. The replays can't: `inst_new` runs inside `enter`, after
  the vertices are in and before `xr` is.

### Checking it

On the emulator first: `emu6502.py` behind a stand-in for b2 that drives
the harness's mailbox, so `b2.py`'s own phases run unchanged.

- Every golden pose's image, sprites on: 22,572 frames, all the C's.
- S5u: 4,988 records of the 21 functions on maps 0, 7 and 9, plus 4,000
  synthetic `close_columns` records and, new, 4,000 synthetic `run_end`
  ones (a later open run closed after the fact, which no pose reaches).
  All reproduce.
- S4 on both buffers, the pools with their fractions, random ones too.
- S6 and S7 on every stream, both buffers, map 7 and the stress map, and
  the abort check.

Then on b2: phases 4, 5 and 6, and phase 7's sprite replays, its frames
and the abort check. All clean. The stress map's capture there: median
51,456 cycles, 90% 132,954, worst 249,284, and the stack never below
&1A0 (it was &146).

Poking a record's state into the Beeb a table at a time made the replays
crawl on b2, about 60 HTTP requests a record. `replay.py` gathers the
writes now and sends each run of tables in one go, 12 a record. A
`clip_line` there takes 654 cycles at the median and 976 on average, a
`process_edge` 3,336 and 4,006.

The coverage gate's list of render.c lines was the old render.c's. It
has the new ones now, a reason each. The pose search found four new
poses, two of them in room 3. They're in the goldens and the records.

### What it costs

Maps 0 and 1 at the demo poses, all 256 angles, cycles a frame
(`frame.py sweep`):

| | map 0 before | after | map 1 before | after |
|---|---|---|---|---|
| capture, mean | 51,485 | 47,910 | 81,449 | 74,431 |
| capture, median | 22,037 | 24,032 | 92,402 | 90,375 |
| capture, 90% | 135,259 | 118,846 | 142,720 | 115,845 |
| capture, max | 149,656 | 132,933 | 168,785 | 129,171 |
| frame, mean | 145,291 | 140,540 | 166,257 | 158,117 |
| frame, max | 335,543 | 314,549 | 258,447 | 232,022 |
| pieces, mean and max | 23.3, 68 | 16.6, 47 | 35.8, 73 | 26.6, 50 |
| the demo, frames a second | 17.8 | 17.6 | 13.9 | 14.2 |

The frames a second simulate the demo's loop over those costs; for
"before" they match b2's 17.5 and 13.9.

The stress map didn't change, so its 10,520 golden poses compare too:
capture's mean goes from 64,152 to 61,780, its 90% from 140,841 to
132,122 and its worst from 258,129 to 248,111, with the median flat. A
straight corridor has few steps for twins and the level skip to save.

The heavy frames win: those over 100k cycles of capture are 12% down on
map 0 and 14% on map 1, and the worst 11% and 23%. The light ones lose
about 2,500 cycles. Map 0's demo is mostly light, so it's a shade slower.

Angle 210 shows why: one wall, 9,161 cycles before, 13,038 after. A near
wall's lines are cut off at the screen's edge. The old clip split the
cut-off columns off as blocks of known side and ran the rest through the
extents test: one product a line. render.c runs them through everything
else's machinery. `flip` finds where the line comes back on screen
against the bound (a product at x0), `merge` splits there (the same
product again), and the visible part goes through `meet` twice. That's
about seven products a line, and the calls between. It's the price of
splitting a bound only where a run really ends: the old split at x0
whether it needed to or not.

One saving is the 6502's own. In a cut-off stretch on the bound's side,
the line's row is the furthest there is, so it's beyond whatever the
bound's rows are. `flip` sets the cursor as `line_at` would and skips the
bound's product.

### Not done

For the light frames, if wanted:

- **A row cache.** About seven products a frame repeat one already
  worked out, worth some 600 cycles. Exact, so the C needn't change.
- **The extents test at each run's start**, not just the clip's. A
  prototype says 1%. That's a change to render.c.
