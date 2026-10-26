# logibox

A logic-gate sandbox written in Ace: place square blocks on a 16×16 grid,
flip switches, and watch signals move through gates, one tick at a time, in
an SDL3 window.

## Running

Needs Homebrew SDL3 on Apple arm64. From this directory:

```sh
ace pkg run      # build and run
ace test .       # run the tests
```

The program finds its diagrams in `diagram/` next to its `build/` folder, so
it can be started from any directory.

## Blocks

| Key | Block      | Label | What it does |
|-----|------------|-------|--------------|
| S   | source     | `S`   | A 0/1 switch. Click it in Run mode to flip it. |
| W   | wire       | `W`   | Carries a signal over distance. Touching wires form one net. |
| I   | input pin  | `I`   | A gate's input: reads what it touches and feeds every gate it touches. |
| O   | output pin | `O`   | A gate's output: drives its gate's value onto what it touches. |
| A   | AND        | `&`   | High when every input is high. Needs at least 2 input pins. |
| R   | OR         | `\|`  | High when any input is high. Needs at least 1 input pin. |
| N   | NOT        | `!`   | High when its input is low. Needs exactly 1 input pin. |
| Z   | NAND       | `D`   | Low when every input is high. Needs at least 2 input pins. |
| V   | NOR        | `V`   | High when no input is high. Needs at least 1 input pin. |
| X   | XOR        | `^`   | High when an odd number of inputs are high. Needs at least 2 input pins. |
| C   | XNOR       | `=`   | High when an even number of inputs are high. Needs at least 2 input pins. |
| L   | light      | `L`   | Shows the level of what it touches. |

## Rules

- **Blocks have no direction.** A block's role comes from its type and what
  it touches.
- **Things connect by touching** (north, east, south, west, never diagonally).
  A source or output pin next to an input pin or light drives it directly.
  Wire is only needed to cover distance.
- **A gate connects only through its pins.** A wire or source touching a gate
  directly does nothing; it must reach an input pin.
- **Touching cells of one gate type are one gate**, so a gate can be any
  shape, and every input pin around it counts. Different gate types never
  merge.
- **A gate with no input pins is idle** (low). One with too few (AND, NAND,
  XOR, XNOR) or too many (NOT) is an error.
- **A wire is low, high, or error.** With no driver it is low. Drivers that
  agree give their level. Drivers that disagree (for example a high source and
  a low output pin on one net) are contention, an error, shown magenta.
- **Errors spread.** A gate reading an error outputs an error.
- **Timing.** Every gate takes one tick to respond; wires respond instantly.
  That alone gives the textbook behaviours: glitches, a NOT wired back into
  itself oscillating, and cross-coupled gates holding a bit.

## Controls

| Input | Action |
|-------|--------|
| 1 / Edit | Edit mode: build the board |
| 2 / Run | Run mode: simulate it (leaving stops auto-ticking; levels and the tick count carry over) |
| S W I O A R N Z V X C L | Select a block (see Blocks), or click it in the block row under the bar (Edit only) |
| Left click / drag | Edit: place the selected block. Run: flip a source |
| Right click / drag | Edit: remove |
| Space | Run: toggle auto-ticking (every 250 ms) |
| Step | Run: step one tick |
| New / Open / Save / Save as | Bar buttons; the diagram's name shows at the bar's right |
| Ctrl+S | Save (asks for a name the first time) |
| Ctrl+O | Show or hide the list of diagrams |
| Escape | Close the list or name box; otherwise quit |

The Save-as box takes `a`–`z`, `0`–`9`, `-` and `_` (Shift+`-`); Enter saves,
Escape cancels.

## Example: SR latch

`diagram/latch.lgbx` is a NOR latch: each NOR is an OR feeding a NOT, and
the two are cross-coupled, so Q = NOR(R, Q′) and Q′ = NOR(S, Q).

```
. . . . . . . . . . .
. S I | . . . . . . .     R (reset)
. . . | O I ! O W W L     Q
. . I | . . . . . W .
. . W . . . . . . W .
. . W W W W W W . W .     Q′ back to the upper OR
. . . . . . . W . W .
. S I | . . . W . W .     S (set)
. . . | O I ! O . W .     Q′
. . I | . . . . . W .
. . W W W W W W W W .     Q back to the lower OR
```

The file is saved with R on and S off, so it powers up held in reset: the
light is off. Switch to Run (2), turn on auto-ticking (Space), and turn R off: the
light stays off. Flip S on and off: it turns on and stays on. Flip R on and
off: it turns off and stays off.

**Feedback circuits need a defined start and long enough pulses.** A diagram
stores how a circuit is built, sources included; gate outputs are simulation
state, so every load starts with all gates low. Saved with both R and S off,
the latch's two halves rise and fall together and oscillate forever, since
identical gates never break the tie. The same happens when R and S are
released together, or when a pulse is shorter than the trip around the loop
(about 4 ticks here). Hold each pulse at least that long, and save feedback
circuits with an input held, the way real circuits hold reset at power-on.

## Diagram format (`.lgbx`)

An 8-byte header, then one byte per cell, row by row:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `LGBX` |
| 4 | 1 | version, `1` |
| 5 | 1 | width, `16` |
| 6 | 1 | height, `16` |
| 7 | 1 | reserved, `0` |
| 8 | w × h | cell codes |

| Code | Block | Code | Block |
|------|-------|------|-------|
| 0 | empty | 5 | output pin |
| 1 | source, off | 6 | AND |
| 2 | source, on | 7 | OR |
| 3 | wire | 8 | NOT |
| 4 | input pin | 9 | light |
| 10 | NAND | 12 | XOR |
| 11 | NOR | 13 | XNOR |

A cell's top bit is reserved for future flags and must be 0. Gate outputs are
not stored: a loaded diagram starts with every gate low. A reader rejects a
wrong magic, version, reserved byte, board size, file length, or unknown code.

## Code

| File | Contents |
|------|----------|
| `grid.ace` | The board: blocks and their placement. |
| `group.ace` | Groups of touching cells: wire nets and multi-cell gates. |
| `sim.ace` | Levels, drivers and contention, and the tick. |
| `diagram.ace` | `.lgbx` encode and decode. |
| `ui.ace` | Rectangles, layered drawing, and text helpers. |
| `main.ace` | The window, input, bar, Open list, and Save-as box. |
| `*.test.ace` | Tests for each module, run with `ace test .`. |
