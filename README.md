# portalc

A C17 implementation of a 2.5D portal engine for the BBC Micro, following the
design in [docs/portal-spec.md](docs/portal-spec.md). It renders into an exact
MODE 5 back buffer (128x128 pixels, doubled rows with 2x2 dithering, EOR
vertical fill) and presents it in a window through
[richc](https://github.com/waitingforvsync/richc).

The engine restricts itself to 6502-friendly techniques throughout: no
multiplies or divides, only quarter-square, sine and reciprocal tables with
shifts and adds, and value widths no larger than the 6502 build would use.

## Building

```
git clone --recurse-submodules https://github.com/waitingforvsync/portalc.git
cd portalc
cmake -B build
cmake --build build
```

The tests run as a post-build step. Run `build/src/portalc` for the windowed
demo, or `build/src/portalc_dump` to write frames from a set of fixed camera
poses as PPM files.

## Controls

- `W`/`S` or cursor up/down - move forward/back
- `A`/`D` - strafe left/right
- cursor left/right - turn
- `Esc` - quit

## Layout

- `docs/portal-spec.md` - the engine design document
- `src/` - the engine library (`arith`, `map`, `render`, `mode5`, `palette`,
  `camera`) and the windowed app
- `src/richc/` - the richc library, as a git submodule
- `src/test/` - unit tests
