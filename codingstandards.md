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
- Every routine braced, `{ }`, and anything else where it makes sense.
- Every whole-line comment is `\\`: the file's header, the notes between
  routines, a routine's header and its parts'. Only trailing comments use
  `;`.
- A routine's header comment goes above it, brief, then a blank line, then
  its label.
- Declare each `ZA_AUTO` next to where it's used: after the open brace for
  one the whole routine uses, after a part's `\\` comment for one only that
  part uses. Every part gets its own, named for what it holds there: never
  one variable reused for unrelated jobs. Baron shares the bytes wherever
  the lives don't overlap.
- `ZA_AUTO`s, then a blank line, then the code. A `ZA_AUTO` can have a
  trailing comment: in, out or local, and what it is.
- A routine's parts are separated by a blank line, a `\\` comment saying
  what comes next, and another blank line.
- Spaces, never tabs.
- Trailing `;` comments where they help, all on one column, far enough out
  to clear the code.
- Comments terse throughout.

```
\\ Plot one sprite: spr, scale, sprx and spry in.

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
  main loop, the multiply's pointers) is a `ZA_AUTO` at file scope, outside
  any routine, in the file that owns it: `src/irq.6502`, `src/tables.6502`.
  File scope only makes the name global: Baron still places it by
  liveness, so until something reads it, it shares bytes with whatever's
  alive. Check its bytes once its readers exist.
- `ZA_UNREACHABLE` straight after every branch that's always taken (`CLC:BCC
  start`, any branch on a known flag), after a `JSR` that never returns, and
  after a `BRK`'s message. It prunes the path that never happens, so Baron
  holds nothing live for it; without it the allocator packs worse.
- `ZA_WIPE` straight after a loop that wipes zero page wholesale.
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
- An index that's off by one (as it came out of the sum that made it)
  stays as it is: fold the one into the address, `LDA table-1,X` or
  `LDA table+1,X`, never an `INX`/`DEX` first.
- Hot-path code like multiplies goes inline, as a `MACRO`.

### Memory

- Struct of arrays: every field of a record is a table of its own
  (`vs_k,Y`, `vs_xr,Y`), and a field that's an array is tables end to end.
- A table that's read indexed keeps to one page, as a page crossing costs
  a cycle: `FITPAGE` before it (`src/macros.6502`), or an `ASSERT` where
  its place is fixed.
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

### Baron's gotchas

- Each file on the command line assembles on its own: `boot.6502` and
  `portal.6502` can't see each other's symbols. Share through `INCLUDE`, or
  through fixed addresses.
- A label inside a scope hides a global of the same name: `.rpost` as a loop
  label turned `STA rpost,X` into a store over the loop.
- A `ZA_AUTO` and a label can't share a name in one scope.

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
- Match the code round it: its comment density, naming and idiom.
- Build with clang, always.
