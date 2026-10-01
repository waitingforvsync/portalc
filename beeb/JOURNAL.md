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
not frequency. But `transform`, for one, is about 830 where the cost
model has about 650 (its guess of 90 on top of the measured rotation).

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
