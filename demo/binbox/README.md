# binbox

A logic-gate sandbox written in Ace, in an SDL3 window. Place square blocks on
a 64×32 grid, flip switches, and watch signals move through gates, one tick at
a time.

## Running

Needs Homebrew SDL3 on Apple arm64. From this directory:

```sh
ace pkg run      # build and run
ace test .       # run the tests
```

The program finds its diagrams in `diagram/` next to its `build/` folder, so
it can be started from any directory.

## Blocks

| Key | Block      | What it does |
|-----|------------|--------------|
| S   | source     | A 0/1 switch. Click it in Simulate mode to flip it. |
| W   | wire       | Carries a signal over distance. Touching wires form one net. |
| I   | read       | A gate's read pin: reads what it touches and feeds every gate it touches. |
| O   | write      | A gate's write pin: drives its gate's value onto what it touches. |
| A   | AND        | High when every input is high. Needs at least 2 inputs. |
| R   | OR         | High when any input is high. Needs at least 1 input. |
| N   | NOT        | High when its input is low. Needs exactly 1 input. |
| Z   | NAND       | Low when every input is high. Needs at least 2 inputs. |
| V   | NOR        | High when no input is high. Needs at least 1 input. |
| X   | XOR        | High when an odd number of inputs are high. Needs at least 2 inputs. |
| C   | XNOR       | High when an even number of inputs are high. Needs at least 2 inputs. |
| L   | LED        | Shows the level of what it touches. |
| B   | bridge     | Crosses two wires: north joins south, east joins west, the pairs kept apart. |
| K   | clock      | A source that flips itself: low for a span of ticks, then high for as many, counted from the start. With the clock selected (Edit, pencil), the side panel's Clock slider sets the span of clocks placed from then on: 1 (at start), 2, 4, 8, 16, 32, 64 or 128 ticks a half cycle. Clicking a placed clock does nothing; replace it to change its span. |
| T   | toggle     | A gate with memory: its output flips on each rising edge of its input, then holds. Its pins are read as for OR; with none it never flips. An error on a pin makes its output an error, and its stored value returns when the error clears. |

The pin keys `I` and `O` are temporary: the keys are being remapped.

## Rules

- **Blocks have no direction.** A block's role comes from its type and what
  it touches.
- **Things connect by touching** (north, east, south, west, never diagonally).
  A source or write pin next to a read pin or LED drives it directly.
  Wire is only needed to cover distance.
- **A bridge joins wire to wire only.** A run of bridges passes straight
  through, so a wire can cross several buses at once. A source, pin, or gate
  touching a bridge does nothing. It lights when either direction is high.
- **A gate connects only through its pins.** A wire or source touching a gate
  directly does nothing; it must reach a read pin.
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
| F1 / Edit | Edit mode: build the board; blocks show without levels. The bar's Mode button toggles the two modes and names the one a press switches to |
| F2 / Simulate | Simulate mode: simulate it (leaving stops auto-ticking; levels and the tick count carry over) |
| Hover | Shows the block's name at the bottom center, in either mode |
| Wheel | Zoom the board about the pointer, in either mode, in steps of 0.25: 0.5× (20-pixel cells) at start, up to 2× (1× is 40-pixel cells), and out to 0.25× (10-pixel cells) |
| − / + | Zoom out or in about the board's center, in either mode; the field before them shows the zoom level (0.5 at start) |
| Depth button | Toggle the side faces that make blocks look raised, in either mode; on at start |
| Middle drag | Pan the board, in either mode, until any cell, a corner one too, sits at the window's center; the cursor shows a closed hand |
| Green button / Ctrl+Cmd+F | Full screen; the window is also resizable, and the board refits to it |
| S W I O A R N Z V X C L B K T | Select a block (see Blocks), or click it in the side panel's Blocks widget (Edit with the pencil only) |
| Chevron at the side panel's top left | Folds the panel to a narrow strip, or unfolds it; the board takes the freed width |
| 1 / Pencil | Tools widget in the side panel: the pencil tool (Edit only); selecting a block returns to it |
| 2 / Select | Select tool (Edit only): drag a box over blocks; drag inside the selection to move it (blocks replace what they land on); Delete or Backspace removes it; Escape or a click elsewhere drops it |
| Cmd+C / Cmd+X / Cmd+V | Edit only: copy or cut the selection to an in-app clipboard; paste it as a floating selection at the pointer (else the board's center), kept on the board, to drag and drop like a move (Escape or a click elsewhere drops it where it is) |
| Cmd+Z / Cmd+Shift+Z | Edit only: undo or redo an edit (a pencil drag, a move, paste, cut, delete, New); the last 20 are kept, and Open clears them. Undoing a floating paste cancels it |
| Left click / drag | Edit with the pencil: place the selected block. Simulate: flip a source |
| Right click / drag | Edit: remove |
| Space / Auto | Simulate: toggle auto-ticking at the Speed slider's rate; a button in the side panel's Simulate widget |
| Speed slider | Drag for Auto's rate; in the side panel's Simulate widget. It snaps to 1 to 9 Hz by 1, then 10 to 90 by 10, 100 to 900 by 100, 1 to 9 kHz by 1 kHz, 10 to 90 kHz by 10 kHz, and 100 kHz (4 Hz at start). The footer shows the rate reached when the frames cannot keep up with it |
| Reset | Simulate: returns to the state Simulate was entered with: every gate low, the tick count and clocks back to 0, sources as they were then; Auto keeps running or stopped; a button in the side panel's Simulate widget (no key) |
| Step | Simulate: step one second of ticks at the Speed slider's rate (1 tick at 1 Hz); a button in the side panel's Simulate widget |
| New / Open / Save / Save as | Bar buttons; the diagram's name shows at the bar's right |
| Cmd+S | Save (asks for a name the first time) |
| Cmd+O | Show or hide the list of diagrams |
| Escape | Close the list or name box, otherwise drop the selection |
| Cmd+Q | Quit |

The Save-as box takes `a`–`z`, `0`–`9`, `-` and `_` (Shift+`-`); Enter saves,
Escape cancels.

## Example: JK flip-flop

`diagram/jk-flip-flop.bnbx` is a JK flip-flop built around one toggle (T), with
J the source at the left and K the lower source. A toggle flips only when its
input rises, so the input is made high exactly when the flip-flop must change:
`(J and not Q) or (K and Q)`. That is the JK equation `Q+ = J·Q' + K'·Q`. J sets
Q low to high, K sets it high to low, and a pulse that would change nothing (J
while set, K while reset) does nothing. It has no clock: it reacts as soon as J
or K changes. It sits at the board's center (columns 26-37, rows 12-18):

```
. . N I W W W W W W W .     NOT of Q: the upper AND's second pin
. . O . . . . . . . W .
. . I . . . . . . . W .
S I & O I | O I T O W L     J (S) into the upper AND; OR; the toggle; Q to the LED
. . . . . I . . . . W .
. . . . . O . . . . W .
. . . S I & I W W W W .     K (the lower S) into the lower AND; Q into its other pin
```

In the drawing, `I` is a read pin and `O` a write pin.

Touching does most of the wiring: each gate's write pin sits against the next
gate's read pin, so only Q needs wire.

The file is saved with J and K off and Q low. Switch to Simulate (2), turn on
auto-ticking (Space), and flip J on and off: the LED turns on and stays on. Flip
K on and off: it turns off and stays off. Holding both on leaves it set: from
reset it sets, and from set Q dips low for a few ticks and flips back. It does
not toggle on every tick as a clocked JK would, because the toggle's input
stays high after the first flip. Pulses of any length work, since the toggle
reacts to the edge, not to the level. Q follows about 4 ticks after the input
changes.

**Feedback circuits with gates alone need a defined start and long enough
pulses.** Gate outputs are simulation state, so every load starts with all
gates low. Two cross-coupled NORs saved with both inputs off rise and fall
together and oscillate forever, since identical gates never break the tie. A
toggle breaks the tie by remembering, which is why this flip-flop needs no held
input at power-on.

## Diagrams

- `diagram/toggle.bnbx`: a toggle built from gates, what the toggle block does without
  it. A NOT (a tick late) and an AND make a one-tick pulse when the switch rises, and an
  XOR fed back on itself flips on that pulse. Flip the switch on and off: the LED turns on
  and stays on; again: it turns off. Needs no reset.
- `diagram/jk-flip-flop.bnbx`: JK flip-flop from one toggle and four gates (J sets, K resets, no clock)
- `diagram/half-adder.bnbx`: half adder
- `diagram/full-adder.bnbx`: full adder
- `diagram/ripple-counter.bnbx`: 8-bit ripple counter on a clock. Each bit is three
  gates: a NOT and a NOR make a one-tick pulse when the bit's clock falls, and an
  XOR fed back on itself toggles on that pulse (the first bit's clock is the
  clock block, the next bit's is this bit). The LEDs show the count, least
  significant on the left. Needs no reset.
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
| 0 | empty | 5 | write |
| 1 | source, off | 6 | AND |
| 2 | source, on | 7 | OR |
| 3 | wire | 8 | NOT |
| 4 | read | 9 | LED |
| 10 | NAND | 12 | XOR |
| 11 | NOR | 13 | XNOR |
| 14 | bridge | 15–22 | clock, span 0–7 (CLOCK + span) |

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
| `theme.ace` | The bar and panel colors. |
| `icon.ace` | The icons, generated from `icon/*.svg` by `python3 -I icon/bake.py`; do not edit by hand. |
| `main.ace` | The SDL window and the event loop. |
| `*.test.ace` | Tests for each module, run with `ace test .`. |
