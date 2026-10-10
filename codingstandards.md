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
- A routine's header comment goes above it, in `\\` lines, brief, then a
  blank line, then its label.
- `ZA_AUTO`s straight after the open brace, then a blank line, then the
  code. A `ZA_AUTO` can have a trailing comment: in, out or local, and what
  it is.
- A routine's parts are separated by a blank line, a single `\ ` comment
  saying what comes next, and another blank line.
- Trailing `;` comments where they help, all on one column: column 40, in
  every file.
- Comments terse throughout.

```
\\ Plot one sprite: spr, scale, sprx and spry in.

.plot_sprite
{
    ZA_AUTO1 strips                     ; local: byte columns left

    \ set up

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
- `ZA_WIPE` straight after a loop that wipes zero page wholesale.
- `ZA_INDEXEDBY` on an indexed access whose index set is known.
- `ZA_DISCARD` before a loop that rebuilds a multi-byte variable through
  indexed stores: an indexed store proves nothing.
- Check the allocation by hand in the `-v` listing's `[auto]` lines: no two
  variables share a byte while both are live. Baron's allocator has had
  bugs before.

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
- Code that runs once is thrown away afterwards (the loader becomes
  Scratch), so it can be a `MACRO`, inlined where it's used, and
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
  - a branch on a known flag is a `JMP`, a byte shorter;
  - the instructions that keep C: loads, stores, transfers, `INX`/`DEX`/
    `INY`/`DEY`, `INC`/`DEC`, `AND`/`ORA`/`EOR`, `BIT`;
  - a value can live in C: never let a `CMP` sit between an `ADC` and the
    `ROR` that wants its carry;
  - a known C folds a constant: `EOR #&FF:ADC #k+1` with C clear is k - x.
- Keep the hot byte in A in shift loops: `LSR q+1:ROR A` beats `ROR q` by 3
  cycles a bit, stored once at the end.
- Hot-path code like multiplies goes inline, as a `MACRO`.

### Memory

- Struct of arrays: every field of a record is a table of its own
  (`vs_k,Y`, `vs_xr,Y`), and a field that's an array is tables end to end.
- Workspace comes from the virtual sections in `src/memory.6502`, claimed
  where it's used.
- Sort new per-frame state by lifetime: dead before the plot, born after it,
  or crossing it. Dead-before-render state can live where nothing reads it
  at render time; data used one after the other can share addresses
  (separate sections over the same range).

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
- Tables are built as the 6502 builds them (bit-a-delta packing,
  recurrences, division loops), not from floating point at run time, and
  read the 6502's way (`pt_log2` through `nrm` and `k8`).
- The 6502 points under Speed apply here too: bytes, tables, sign-picked
  paths, nothing negated.
- Match the code round it: its comment density, naming and idiom.
- Build with clang, always.
