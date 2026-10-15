# Coding standards

How code in this repo is written: the 6502 in `beeb/`, in Baron, and the C
in `src/`, which models it. Read this before writing any.

## The 6502 (`beeb/`, Baron)

### Layout

- No spaces round statement colons: `LDA #15:LDX #0:JSR &FFF4`.
- No spaces inside an operand: `STA ptr+1`, `LDA tab+2,X`.
- `SECTION` attributes do have spaces round `=`:
  `SECTION Boot, filename = "!BOOT", org = &400, load = &FFFF0400`.
- Labels lowercase. Memory-map constants of our own are upper case
  (`SCREEN_A`, `T_ORG`).
- Names in whole words, not abbreviations: `major`, `logmajor`, `xsign`,
  not `maj`, `lmaj`, `sx`.
- Every routine braced, `{ }`, and anything else where it makes sense.
- Every whole-line comment is `\\`: the file's header, the notes between
  routines, a routine's header and its parts'. Only trailing comments use
  `;`.
- Comments go to 100 columns at most, wrapped to fit. Code that can't be
  split, like a long macro call, can run past.
- A file's header comment says what's in the file, in two lines at most.
- A routine's or macro's header says what it's for, in two lines at most.
  Then a blank `\\` line, `Inputs:` with an indented list, one to a line,
  another blank `\\` line, and `Outputs:` likewise. What it preserves or
  corrupts goes under `Outputs:` where it matters, most of all for a macro.
  How it works goes in its trailing comments, if anywhere. Then a blank
  line, then its label.
- Declare each `ZA_AUTO` next to where it's used, in the smallest scope it
  can have: after the open brace for one the whole routine uses, after a
  part's `\\` comment for one only that part uses. Every part gets its own,
  named for what it holds there. Baron shares the bytes wherever the lives
  don't overlap.
- Be pragmatic about inputs, though: one that's a one-shot argument, which
  nothing wants after the call, can be worked on in place, and is listed as
  corrupted under `Outputs:` (a shallow run counts its row and fraction in
  `run_y`).
- A routine's inputs and outputs can be declared in its own scope, and the
  caller reaches them as `routine.var` (`row_move.row`). Baron's zero-page
  guide encourages it: the caller needn't know where the value lives, and
  a result can be handed straight to the next routine.
- A macro never reaches into its caller's scope. A `ZA_AUTO` only the
  macro uses is declared in it, by the same rule (`SHALLOW`'s `fraction`
  at its top, `grad2` after the part that uses it), and each expansion
  gets its own, sharing bytes where their lives don't overlap. One the
  caller sets or reads too is the caller's, passed in as an argument
  (`SHALLOW down, grad, row, count, start, end`), and so on down to the
  macros it calls. File-scope state (`cursor`, `run_y`) is used by name.
- `ZA_AUTO`s, then a blank line, then the code. A `ZA_AUTO` can have a
  trailing comment: in, out or local, and what it is.
- A routine's parts are separated by a blank line, a `\\` comment saying
  what comes next, and another blank line.
- Spaces, never tabs.
- Trailing `;` comments where they help, all on one column, far enough out
  to clear the code.
- Comments terse throughout.

```
\\ Plot one sprite.
\\
\\ Inputs:
\\     spr, scale: the sprite and its scale
\\     sprx, spry: where
\\
\\ Outputs:
\\     none

.plot_sprite
{
    ZA_AUTO1 strips                     ; local: byte columns left

    \\ set up

    ZA_AUTO1 width                      ; local: the plotted width

    JSR plot_setup
    LDA plot_setup.scale+1:BNE engine   ; 1x and up
    ...
}
```

### Zero page

- `ZA_AUTO` for every zero-page variable: none placed by hand. The pool is
  `ZA_POOL` in `src/memory.6502`.
- `ZA_ENTRY` only on roots, routines nothing calls (a `*RUN` entry point).
  A routine reached by a `JSR` or `JMP` gets no marker.
- `ZA_INTERRUPT` on every interrupt or `BRK` handler.
- State that several routines share (what the interrupt shares with the
  main loop, the multiply's pointers, the camera's position) is a
  `ZA_AUTO` at file scope, outside any routine, in the file that owns it:
  `src/irq.6502`, `src/tables.6502`, `src/camera.6502`.
  File scope only makes the name global: Baron still places it by
  liveness, so until something reads it, it shares bytes with whatever's
  alive. Check its bytes once its readers exist.
- `ZA_UNREACHABLE` straight after every branch that's always taken (`CLC:BCC
  start`, any branch on a known flag), after a `JSR` that never returns, and
  after a `BRK`'s message. It prunes the path that never happens, so Baron
  holds nothing live for it; without it the allocator packs worse.
- `ZA_WIPE` straight after a loop that wipes zero page wholesale.
- Recursion keeps nothing in zero page across the recursive call but a
  running count (`walk_levels`, `DEC` before, `INC` after). What a level
  wants back goes on the stack round the call, `LDA v:PHA` before and
  `PLA:STA v` after, or is made again before it's read; Baron refuses
  anything else held across, so it checks what's saved is enough.
- `ZA_INDEXEDBY` on an indexed access whose index set is known.
- `ZA_DISCARD` before a loop that rebuilds a multi-byte variable through
  indexed stores: an indexed store proves nothing.
- Check the allocation by hand in the `-v` listing's `[auto]` lines, every
  build that changes them: no two variables share a byte while both are
  live. Baron's allocator has had bugs before.
- Baron takes an OS call as touching nothing in the pool, but the pool runs
  over the OS's zero page. Until the loader takes the machine over, nothing
  may be live across an OS call or an interrupt in the OS's bytes.

### Hardware

- Hardware registers as literal addresses, named in a trailing comment:
  `STA &FE4D ; system VIA: IFR`. No names like `SYS_IFR`.
- NMOS 6502 only: no 65C12 opcodes, Master or not.

### Speed

Weigh every line by what it costs on a 2 MHz 6502.

**Optimise from the first line.** Every routine is written as fast as it
can be from the start, not tidied up later.

- Pass values in registers, not zero page, wherever they fit. A count in X,
  counted down by `DEX:BNE`, needs no store to set it up and costs 2 cycles
  a time round; a `pages` variable needs the store and a 5-cycle `DEC`.
- No copy made only to be read: read the value where it already is (the
  steep plotter adds `line_grad` itself, not a local holding its low
  byte). And no copy made only to be changed, when nothing wants the
  original after: work on it in place (the plotters count their row and
  fraction in `run_y`, listed as corrupted). Copy only what something
  still reads after it's changed.
- Code that runs once is thrown away afterwards (the loader's memory
  becomes T), so it can be a `MACRO`, inlined where it's used, and
  self-modifying: `abs,Y` with the operand's high byte stepped by `INC`,
  rather than a zero-page pointer.

The rest:

- Bytes where bytes will do, tables instead of multiplies, unsigned
  magnitudes with the sign picking the path, and work hoisted out of loops.
- Never negate a product: fuse the sums so terms go in by add or subtract
  (opposite signs are one subtraction; both negative, negate once).
- Follow the flags through the code, C most of all:
  - hoist `CLC`/`SEC` out of a loop whose body never touches C and whose
    exit leaves it known;
  - a branch on a known flag is a `JMP`, a byte shorter (with a
    `ZA_UNREACHABLE` after it);
  - the instructions that keep C: loads, stores, transfers, `INX`/`DEX`/
    `INY`/`DEY`, `INC`/`DEC`, `AND`/`ORA`/`EOR`, `BIT`;
  - a value can live in C: never let a `CMP` sit between an `ADC` and the
    `ROR` that wants its carry;
  - a known C folds a constant: `EOR #&FF:ADC #k+1` with C clear is k - x;
  - a shift's carry is known when the bit it shifts out is: three `ROL dv+1`
    into a byte that started at 0 leave C clear, so the add after needs no
    `CLC`.
- Never shift a multi-byte value in memory while A is free: hold one byte
  in A, `ASL A:ROL dv+1` rather than `ASL dv:ROL dv+1`. Hold the byte the
  next instruction wants, the low byte where an add follows. In a loop it's
  3 cycles a bit better, `LSR q+1:ROR A` against `ROR q`, stored once at the
  end.
- An index that's one short (as it came out of the sum that made it)
  stays as it is: fold the one into the address, `LDA table+1,X`, never an
  `INX` first.
- A hot loop's branches (taken each pair, each run) are followed by
  `SAME_PAGE target` (macros.6502), which fails the build if the branch
  crosses a page when taken, a cycle each time. Fix one by moving code, not
  padding: sprite.6502's routines are ordered so its pair loops fit. Code
  with these asserts goes first in its section, before data whose size
  changes (`MainCode` starts with the sprite plotters, the map after), so
  the map doesn't move it. A forward branch round a long block (a
  handler's column change, more than a page in all) can't always be kept
  on a page, and costs a cycle a call at most: it goes unguarded.
- Never a base before the table, `table-1,X`, unless `FITPAGE_FROM` puts
  the base on the table's page (`exp2_skip-8,Y`): with the table on a page
  boundary, the base is on the page before, and every access pays the
  crossing. A table numbered from 1 (the vertex cache's entries) keeps its
  element 0, unused, and is read as it is. One numbered from 0 (vc_slot, by
  vertex) is looped over upwards, or down to 0.
- Hot-path code like multiplies goes inline, as a `MACRO`. Its rare uses
  share one routine that expands it, as `pattern_xy` does `PATTERN_XY`:
  inline there, it costs bytes and saves almost nothing.
- A macro that takes a bound (`LOG2_16 bits`, `EXP2_16 bits`) gets the
  tightest one the value can reach, worked out at the call: the bound picks
  the expansion, and a loose one costs compares and bytes. `make_line`'s
  ends on the screen are `EXP2_16 EDGE_LOG`, 13; 16 only where the value
  can reach &FFFF. Say why in a trailing comment where it isn't plain.
- A file's macros go in a file of their own beside it, `vcache_macros.6502`
  for `vcache.6502`, which `portal.6502` includes first, before any code,
  so no macro waits on the order of the includes. The checks' are in
  `test/`, included first by each check's program.

### Memory

- Struct of arrays: every field of a record is a table of its own
  (`vs_k,Y`, `vs_xr,Y`), and a field that's an array is tables end to end.
- A table that's read indexed keeps to one page, as a page crossing costs
  a cycle: `FITPAGE` before it (`src/macros.6502`), or an `ASSERT` where
  its place is fixed.
- A table of any size is made by the loader, `make_tables` (tables.6502),
  in the `Tables` section, not assembled into the code: `shr3`, `trowhi`.
  A handful of bytes stays by its code as `EQUB`s (plot.6502's
  `mode5_pixel`, which the sprite plotters read too; the sprite check,
  which doesn't assemble plot.6502, has its own copy). Before adding a
  table, see whether arithmetic on one there does it: `SCREEN_AT` gets a
  pair's cursor from `shr3`, for 6 cycles more than two 65-byte tables,
  used once a strip and once a skipped stretch.
- Tables are sized by the map's caps (`MAX_VERTS`, `src/constants.6502`),
  never by a literal or the demo map's counts: bigger maps are coming.
- Workspace comes from the virtual sections in `src/memory.6502`, claimed
  where it's used, from the pool its lifetime fits: `Persist`, `Frame`,
  `Capture`, `Between`, the junk rows, or the loader's `Scratch`.
- Sort new state by lifetime against the frame's phases (camera tick,
  capture, fill, sprites, unplot), and give tables that never coexist the
  same addresses: separate virtual sections over one range, since Baron
  has no unions.
- A cache indexed by another table's index (cell to visit, vertex to
  entry) is emptied through its entries: each holds its key, and clears
  that one index, so only what was used is touched, never the whole table.
  The index table lives where it keeps its zeroes between uses.
- T's junk rows (`JunkTop` and `JunkBot`) are workspace too. Only the
  plotters (in the capture) and the unplotter write them, and nothing reads
  them, so they hold state that's dead from the first plot to the end of
  the unplot: the camera tick's temporaries, the sprite pass's own.

### Checking

- What the C has, the 6502 matches byte for byte, and `make test` checks
  it in b2: `make test_tables`, the loader's tables against
  `portalc_tables`; `make test_arith`, the macros against
  `portalc_arithref`; `make test_vcache`, the vertex cache against
  `portalc_vcacheref`; `make test_capture`, the capture, made-up instances
  too, against `portalc_captureref`; `make test_plot`, the plotters and the
  unplot against `portalc_plotref`; `make test_clip`, the clips against
  `portalc_clipref`; `make test_walk`, whole captures with their sprites'
  instances, each place's first frame unplotted, against `portalc_walkref`,
  and `make test_walk3`, over the stress scene with the depth capped at 3,
  against `portalc_walk3ref`; `make test_fill`, the fill against
  `portalc_fillref`; `make test_sprite`, the sprite plotters against
  `portalc_spriteref`.
- A macro or routine is run over every input where that's feasible,
  summed where the answers won't fit. Each check is its own program,
  `test/CHECK_portal.6502` (`-D TEST=TRUE`), with only the engine files it
  needs: the loader runs it and stops, leaving its answers in HAZEL, &C000
  to &DFFF (`test/test.6502`), for `host/test_build.py` to compare. A new
  check is a new program and a name in the Makefile's `CHECKS`. Break it
  once on purpose, to see the check fail: each path, and a macro expanded
  more than one way (falling and rising) in each way, through an `IF` on
  its parameter. A break that writes outside its buffer can crash the check
  ("never finished"), which is a fail too, but one that shows a difference
  says where. A fast path that hands what it rejects to a slower, exact one
  can't be broken by making it stricter: break it laxly, letting through
  what it should catch. Some can't show even then, as what they let
  through is handled exactly further on (the engine's clip events at the
  strip's end, which never come, are kept out of `NEAREST_EVENT` only to
  save it work): measure one of those on the cycle-exact model instead,
  and keep it only if it pays. A macro expanded at several sites with the
  same arguments (`REBUILD_PAIR`, in two pair loops) is broken at each site
  alone, through a broken copy of it called there.
- Made-up cases mustn't repeat with the cases' own structure: a value
  stepped by 5 over 16 cases a line comes round to the same place each
  line, and whole combinations (a kind of clip with a kind of span) never
  occur. Take the systematic parts from the case's number (a kind of clip,
  the bounds' edges), and the rest from an LFSR stepped once a case, the
  same in the C and the 6502 (`LFSR`, test_macros.6502); then count in the
  C which paths the cases reach before trusting the check.
- Step the LFSR a byte (eight steps) for each made-up value, as the sprite
  check's `random_byte` does. One step leaves the low byte the last one
  shifted a bit, as the taps (&B400) miss it: values drawn one after
  another move together (a sprite's x with its y), and whole cases come
  round again.
- Count the 6502's paths too, on the cycle-exact model: every instruction
  run, every branch taken both ways. Code unrolled by a macro or a `FOR`
  (a handler per column count, a copy per slot) can leave one copy unreached,
  which the C's count can't see. A branch that never falls through is
  either an `(always)` or a test the 6502 can drop.
- A path the demo scene never takes is checked over the C's stress scene
  (`test/stressdata.6502`, which `make map` makes), and a cap it never
  meets with the cap lowered: a variant of the check, as
  `test/walk3_portal.6502` sets `depth_cap` and `stress_scene` and
  includes `walk_portal.6502`. The source picks its default with
  `IF DEFINED()`, binding another name (`MAX_DEPTH`), as Baron's guide says.
- After breaking on purpose, restore and rebuild before measuring or
  replaying anything: a build left from the last break measures the break.
- Sums are Fletcher's, mod 255 (`FLETCHER`, a carry out added back in).
  Mod 256, a byte that's the same in every frame of a row of 256 adds
  nothing, so an error in it can't show.

### Baron's gotchas

- Each file on the command line assembles on its own: `boot.6502` and
  `portal.6502` can't see each other's symbols. Share through `INCLUDE`, or
  through fixed addresses.
- A label inside a scope hides a global of the same name: `.rpost` as a loop
  label turned `STA rpost,X` into a store over the loop.
- A `ZA_AUTO` and a label can't share a name in one scope.
- An `INCLUDE` is relative to the file it's in, not to the command line's.

## The C (`src/`)

The C is a model of the BBC build, so it's written the 6502's way.

- Every value is held in the type the 6502 will hold it in, at its
  narrowest: no `int32_t` intermediates where `int16_t` fits.
- Every narrowing is written out. `pt_u8`, `pt_u16` and `pt_i16` (`arith.h`)
  check it fits; a plain cast only where the wrap is meant (an angle's
  `& 1023`, a low byte). The library builds with `-Wconversion`.
- No pointer that can be NULL standing for "none": not for an optional
  parameter, not for a field. Pass the value (an empty set rather than
  NULL), or a flag and a value.
- Caches are emptied as the 6502 empties them, through their entries'
  back-references, not with a `memset`.
- Tables are built as the 6502 builds them (bit-a-delta packing,
  recurrences, division loops), not from floating point at run time, and
  read the 6502's way (`pt_log2` through `nrm` and `k8`).
- The 6502 points under Speed apply here too: bytes, tables, sign-picked
  paths, nothing negated.
- The C can be more careful than the 6502: asserts and checks the 6502
  skips, as hardening. Where the 6502 takes a shortcut (`root_side` takes
  the last side untested), the C keeps the check, so a case that would
  break the shortcut is caught there and designed round.
- Match the code round it: its comment density, naming and idiom.
- Build with clang, always.
