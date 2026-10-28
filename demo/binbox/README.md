# binbox

Logic circuits and programs to watch run, written in Ace, in an SDL3 window. It
opens on a home screen with two tiles:

- **Diagram**: a logic-gate sandbox. Place square blocks on a 64×32 grid, flip
  switches, and watch signals move through gates, one tick at a time. The Home
  button, first in the toolbar, goes back.
- **Program**: a placeholder for now (coming soon).

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
| S   | source     | `S`   | A 0/1 switch. Click it in Simulate mode to flip it. |
| W   | wire       | `W`   | Carries a signal over distance. Touching wires form one net. |
| I   | input      | `I`   | A gate's input: reads what it touches and feeds every gate it touches. |
| O   | output     | `O`   | A gate's output: drives its gate's value onto what it touches. |
| A   | AND        | `&`   | High when every input is high. Needs at least 2 inputs. |
| R   | OR         | `\|`  | High when any input is high. Needs at least 1 input. |
| N   | NOT        | `!`   | High when its input is low. Needs exactly 1 input. |
| Z   | NAND       | `D`   | Low when every input is high. Needs at least 2 inputs. |
| V   | NOR        | `V`   | High when no input is high. Needs at least 1 input. |
| X   | XOR        | `^`   | High when an odd number of inputs are high. Needs at least 2 inputs. |
| C   | XNOR       | `=`   | High when an even number of inputs are high. Needs at least 2 inputs. |
| L   | LED        | `L`   | Shows the level of what it touches. |
| B   | bridge     | `+`   | Crosses two wires: north joins south, east joins west, the pairs kept apart. |

## Rules

- **Blocks have no direction.** A block's role comes from its type and what
  it touches.
- **Things connect by touching** (north, east, south, west, never diagonally).
  A source or output next to an input or LED drives it directly.
  Wire is only needed to cover distance.
- **A bridge joins wire to wire only.** A run of bridges passes straight
  through, so a wire can cross several buses at once. A source, pin, or gate
  touching a bridge does nothing. It lights when either direction is high.
- **A gate connects only through its pins.** A wire or source touching a gate
  directly does nothing; it must reach an input.
- **Touching cells of one gate type are one gate**, so a gate can be any
  shape, and every input around it counts. Different gate types never
  merge.
- **A gate with no inputs is idle** (low). One with too few (AND, NAND,
  XOR, XNOR) or too many (NOT) is an error.
- **A wire is low, high, or error.** With no driver it is low. Drivers that
  agree give their level. Drivers that disagree (for example a high source and
  a low output on one net) are contention, an error, shown magenta.
- **Errors spread.** A gate reading an error outputs an error.
- **Timing.** Every gate takes one tick to respond; wires respond instantly.
  That alone gives the textbook behaviours: glitches, a NOT wired back into
  itself oscillating, and cross-coupled gates holding a bit.

## Controls

| Input | Action |
|-------|--------|
| F1 / Edit | Edit mode: build the board; blocks show without levels |
| F2 / Simulate | Simulate mode: simulate it (leaving stops auto-ticking; levels and the tick count carry over) |
| Hover | Shows the block's name at the bottom center, in either mode |
| Wheel | Zoom the board about the pointer, in either mode: 0.5× (20-pixel cells) at start, up to 4× (1× is 40-pixel cells), and out to 0.25× (10-pixel cells) |
| − / + | Zoom out or in about the board's center, in either mode |
| Depth button | Toggle the side faces that make blocks look raised, in either mode; on at start |
| Middle drag | Pan the board, in either mode, until any cell, a corner one too, sits at the window's center; the cursor shows a closed hand |
| Green button / Ctrl+Cmd+F | Full screen; the window is also resizable, and the board refits to it |
| S W I O A R N Z V X C L B | Select a block (see Blocks), or click it in the side panel's Blocks widget (Edit with the pencil only) |
| Chevron at the side panel's top left | Folds the panel to a narrow strip, or unfolds it; the board takes the freed width |
| 1 / Pencil | Tools widget in the side panel: the pencil tool (Edit only); selecting a block returns to it |
| 2 / Select | Select tool (Edit only): drag a box over blocks; drag inside the selection to move it (blocks replace what they land on); Delete or Backspace removes it; Escape or a click elsewhere drops it |
| Cmd+C / Cmd+X / Cmd+V | Edit only: copy or cut the selection to an in-app clipboard; paste it as a floating selection at the pointer (else the board's center), kept on the board, to drag and drop like a move (Escape or a click elsewhere drops it where it is) |
| Cmd+Z / Cmd+Shift+Z | Edit only: undo or redo an edit (a pencil drag, a move, paste, cut, delete, New); the last 20 are kept, and Open clears them. Undoing a floating paste cancels it |
| Left click / drag | Edit with the pencil: place the selected block. Simulate: flip a source |
| Right click / drag | Edit: remove |
| Space / Auto | Simulate: toggle auto-ticking at the Speed slider's rate; a button in the side panel's Simulate widget |
| Speed slider | Drag for Auto's rate; in the side panel's Simulate widget. It snaps to 1 to 9 Hz by 1, then 10 to 90 by 10, 100 to 900 by 100, 1 to 9 kHz by 1 kHz, and 10 kHz (4 Hz at start). The footer shows the rate reached when the frames cannot keep up with it |
| Step | Simulate: step one second of ticks at the Speed slider's rate (1 tick at 1 Hz); a button in the side panel's Simulate widget |
| New / Open / Save / Save as | Bar buttons; the diagram's name shows at the bar's right |
| Cmd+S | Save (asks for a name the first time) |
| Cmd+O | Show or hide the list of diagrams |
| Escape | Close the list or name box, otherwise drop the selection |
| Cmd+Q | Quit |

The Save-as box takes `a`–`z`, `0`–`9`, `-` and `_` (Shift+`-`); Enter saves,
Escape cancels.

## Example: SR latch

`diagram/latch.bnbx` is a NOR latch: two NOR gates, cross-coupled, so
Q = NOR(R, Q′) and Q′ = NOR(S, Q). It sits at the board's center (columns
29–34, rows 14–17):

```
S I V O W L     R (reset) into the upper NOR; Q to the LED
. . I . W .     Q′ into the upper NOR, by touch
. . O . W .     the lower NOR's output, Q′
S I V I W .     S (set) into the lower NOR; Q back down column 9
```

Touching does most of the wiring: the lower NOR's output sits right
against the upper NOR's input, so only Q needs wire.

The file is saved with R on and S off, so it powers up held in reset: the
LED is off. Switch to Simulate (2), turn on auto-ticking (Space), and turn R off: the
LED stays off. Flip S on and off: it turns on and stays on. Flip R on and
off: it turns off and stays off.

**Feedback circuits need a defined start and long enough pulses.** A diagram
stores how a circuit is built, sources included; gate outputs are simulation
state, so every load starts with all gates low. Saved with both R and S off,
the latch's two halves rise and fall together and oscillate forever, since
identical gates never break the tie. The same happens when R and S are
released together, or when a pulse is shorter than the trip around the loop
(2 ticks here, one per NOR). Hold each pulse at least that long, and save feedback
circuits with an input held, the way real circuits hold reset at power-on.

## Diagrams

- `diagram/latch.bnbx`: SR latch
- `diagram/half-adder.bnbx`: half adder
- `diagram/full-adder.bnbx`: full adder
- `diagram/alu.bnbx`: 4-bit add/subtract ALU. Four full-adder slices, least
  significant on the right; in each slice the left switch is a B bit and the
  right one an A bit. The switch at the bus's right end is SUB: it XORs every
  B bit and is the first carry in, so the LEDs show A − B. The carry LED at the
  bottom left is the carry out (no borrow, when subtracting).

## Diagram format (`.bnbx`)

An 8-byte header, then one byte per cell, row by row:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `BNBX` |
| 4 | 1 | version, `1` |
| 5 | 1 | width, `64` |
| 6 | 1 | height, `32` |
| 7 | 1 | reserved, `0` |
| 8 | w × h | cell codes |

| Code | Block | Code | Block |
|------|-------|------|-------|
| 0 | empty | 5 | output |
| 1 | source, off | 6 | AND |
| 2 | source, on | 7 | OR |
| 3 | wire | 8 | NOT |
| 4 | input | 9 | LED |
| 10 | NAND | 12 | XOR |
| 11 | NOR | 13 | XNOR |
| 14 | bridge | | |

A cell's top bit is reserved for future flags and must be 0. Gate outputs are
not stored: a loaded diagram starts with every gate low. A reader rejects a
wrong magic, version, reserved byte, board size, file length, or unknown code.

## Code

| File | Contents |
|------|----------|
| `grid.ace` | The board: blocks and their placement. |
| `group.ace` | Groups of touching cells: wire nets and multi-cell gates. |
| `sim.ace` | Levels, drivers and contention, and the tick. |
| `speed.ace` | The Speed slider's stops as tick rates, and their Hz/kHz/MHz text. |
| `diagram.ace` | `.bnbx` encode and decode. |
| `ui.ace` | Rectangles, layered drawing, and text helpers. |
| `bar.ace` | Where the toolbar and the side panel's Tools, Blocks, Simulate and Info widgets sit, and what is in them. |
| `view.ace` | The window size and the board's zoom and pan. |
| `history.ace` | Undo and Redo snapshots. |
| `select.ace` | The select tool's box, carried blocks, and clipboard. |
| `store.ace` | The diagram folder, names, and the Open list. |
| `app.ace` | `App`: the simulation, the selection, the file, and the commands that change them. |
| `input.ace` | SDL events to `App` commands. |
| `render.ace` | Draws the board, bars, and panels. |
| `theme.ace` | The bar and panel colors every screen shares. |
| `icon.ace` | The icons, generated from `icon/*.svg` by `python3 -I icon/bake.py`; do not edit by hand. |
| `home.ace` | The home screen: a tile for each screen to open, and the shared quit check. |
| `program.ace` | The Program screen, a placeholder. |
| `main.ace` | The SDL window and the event loop. |
| `*.test.ace` | Tests for each module, run with `ace test .`. |
