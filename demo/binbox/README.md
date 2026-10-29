# binbox

A logic-gate sandbox written in Ace, in an SDL3 window. Place square blocks on
a 256×128 grid, flip switches, and watch signals move through gates, one tick at
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
| S   | source     | A 0/1 switch, driving the write pins it touches. Click it in Simulate mode to flip it. |
| E   | wire       | Carries a signal over distance. Touching wires form one net. |
| R   | read       | A read pin: reads the wires and write pins it touches, and feeds every gate and LED it touches. |
| W   | write      | A write pin: drives the value of the gates, sources and clocks it touches onto the wires and read pins it touches. |
| 1   | AND        | High when every input is high. Needs at least 2 inputs. |
| 2   | OR         | High when any input is high. Needs at least 1 input. |
| 3   | NOT        | High when its input is low. Needs exactly 1 input. |
| 4   | NAND       | Low when every input is high. Needs at least 2 inputs. |
| 5   | NOR        | High when no input is high. Needs at least 1 input. |
| 6   | XOR        | High when an odd number of inputs are high. Needs at least 2 inputs. |
| 7   | XNOR       | High when an even number of inputs are high. Needs at least 2 inputs. |
| L   | LED        | Shows what the read pins it touches read. |
| B   | bridge     | Crosses two wires: north joins south, east joins west, the pairs kept apart. |
| C   | clock      | A source that flips itself: low for a span of ticks, then high for as many, counted from the start. With the clock selected (Edit, pencil), the side panel's Clock slider sets the span of clocks placed from then on: 1 (at start), 2, 4, 8, 16, 32, 64 or 128 ticks a half cycle. Clicking a placed clock does nothing; replace it to change its span. |
| T   | toggle     | A gate with memory: its output flips on each rising edge of its input, then holds. Its pins are read as for OR; with none it never flips. An error on a pin makes its output an error, and its stored value returns when the error clears. |

## Rules

- **Blocks have no direction.** A block's role comes from its type and what
  it touches.
- **Things connect by touching** (north, east, south, west, never diagonally).
  Touching cells of one kind are one block, however many cells it covers.
- **Wire, read pins and write pins are one net.** Touching ones short
  together, so a write pin drives every read pin and wire in its net. A read
  pin carries the net into the gates and LEDs it touches; a write pin carries
  the value of the gates, sources and clocks it touches onto the net. A run of
  pins that touches no gate, LED, source or clock is only wire, which has its
  own block, so it makes its net an error.
- **Gates, sources, clocks and LEDs connect only through pins.** Anything else
  touching them does nothing, so they pack against wires and each other
  freely. Touching LEDs are one LED, lit by a read pin beside any of its cells
  (an error when those pins disagree). Touching sources share one switch, and
  touching clocks of one span are one clock.
- **A bridge passes wire straight through.** A run of bridges joins the wires
  at its two ends, so a wire can cross several buses at once. A pin touching a
  bridge reaches the wire or pin past it, as if the bridges were wire; sources,
  gates, and LEDs never cross one, so they touch their pins directly. A run
  never turns: it joins only along one straight line. With nothing to
  reach, touching a bridge does nothing. It lights when either direction is high.
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
| F1 / Mode | Toggle between Edit and Simulate, as the bar's Mode button does (it names the mode a press switches to). Edit: build the board; blocks show without levels. Simulate: simulate it (leaving stops auto-ticking; levels and the tick count carry over) |
| Hover | Shows the block's name at the bottom center, in either mode |
| Wheel | Zoom the board about the pointer, in either mode, in steps of 0.125: 1× (40-pixel cells) at start, up to 2×, and out to 0.25× (10-pixel cells); at 0.5× and below blocks show their color alone, without icons |
| − / + | Zoom out or in about the board's center, in either mode; the field before them shows the zoom level (1 at start) |
| Depth button | Toggle the side faces that make blocks look raised, in either mode; on at start |
| Snap button | Make the block being placed (Edit, pencil) snap to the cell under the pointer; off, it follows the pointer freely and a click still places it in that cell. Off at start |
| Middle drag | Pan the board, in either mode, until any cell, a corner one too, sits at the window's center; the cursor shows a closed hand |
| Green button / Ctrl+Cmd+F | Full screen; the window is also resizable, and the board refits to it |
| S E R W 1–7 L B C T | Select a block (see Blocks), or click it in the side panel's Blocks widget (Edit with the pencil only) |
| Chevron at the side panel's top left | Folds the panel to a narrow strip, or unfolds it; the board takes the freed width |
| Pencil | Tools widget in the side panel: the pencil tool (Edit only); selecting a block returns to it |
| I / Dropper | Tools widget in the side panel, or the I key: the dropper tool (Edit only): a left click on a block makes it the pencil's block, with a clock's span, and switches to the pencil; an empty cell does nothing |
| V / Select | Tools widget in the side panel, or the V key: the select tool (Edit only): drag a box over blocks; drag inside the selection to move it: its blocks float, joining nothing, until Escape or a click elsewhere drops them, replacing what they land on; Cmd+drag inside it carries a copy instead and leaves the original (floating blocks are put down first); Delete or Backspace removes it; Escape or a click elsewhere drops it |
| Cmd+C / Cmd+X / Cmd+V | Edit only: copy or cut the selection to an in-app clipboard; paste it as a floating selection at the pointer (else the board's center), kept on the board, to drag and drop like a move (Escape or a click elsewhere drops it where it is) |
| Cmd+Z / Cmd+Shift+Z | Edit only: undo or redo an edit (a pencil drag, a move, paste, cut, delete, New); the last 20 are kept, and Open clears them. Undoing floating blocks cancels them: a move goes back, a paste away |
| Left click / drag | Edit with the pencil: place the selected block. Simulate: flip a source |
| Right click / drag | Edit: remove |
| Space / Play / Pause | Simulate: toggle auto-ticking at the Speed slider's rate; a button in the side panel's Simulate widget that reads Play while stopped and Pause while ticking |
| Speed slider | Drag for the auto-tick rate; in the side panel's Simulate widget. It snaps to 1 to 9 Hz by 1, then 10 to 90 by 10, 100 to 900 by 100, 1 to 9 kHz by 1 kHz, 10 to 90 kHz by 10 kHz, 100 to 900 kHz by 100 kHz, and 1 MHz (4 Hz at start). The footer shows the rate reached after the tick count while Auto runs; it falls short of the setting when the frames cannot keep up |
| Reset | Simulate: returns to the state Simulate was entered with: every gate low, the tick count and clocks back to 0, sources as they were then; auto-ticking keeps running or stopped; a button in the side panel's Simulate widget (no key) |
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
J the source at the lower left and K the source at the top. A toggle flips only
when its input rises, so the input is made high exactly when the flip-flop must
change: `(J and not Q) or (K and Q)`. That is the JK equation
`Q+ = J·Q' + K'·Q`. J sets Q low to high, K sets it high to low, and a pulse
that would change nothing (J while set, K while reset) does nothing. It has no
clock: it reacts as soon as J or K changes. It sits at the board's center
(columns 122-133, rows 60-66):

```
. . . S . . . . . . . .
. . . O I & I W W W I L   K (S) and Q (I) into the upper AND; Q to the LED
. . . . . O . O . W . .
S . . . . I . T . W . .   the upper AND feeds the OR; the toggle's output is Q
O I & O I | O I . W . .   J (S) and NOT Q (I) into the lower AND; OR into the toggle
. . I . . . . . . W . .
. . O N I W W W W W . .   NOT of Q, from the Q wire
```

In the drawing, `I` is a read pin and `O` a write pin.

Touching does most of the wiring: each source and gate's write pin sits against
the next gate's read pin, so only Q needs wire.

The file is saved with J and K off and Q low. Switch to Simulate (F1), turn on
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
- `diagram/ripple-counter.bnbx`: 8-bit ripple counter on a clock. Each bit is a
  toggle (T) and, between bits, a NOT: the toggle flips on every rising edge of
  its input, and its input is the NOT of the bit before (the first bit's input
  is the clock block, through a write and a read pin), so each bit flips when the one before falls. The LEDs
  show the count, least significant on the left. Needs no reset; the first
  few ticks settle to 0 as the NOTs come up.
- `diagram/alu.bnbx`: 4-bit add/subtract ALU. Four full-adder slices, least
  significant on the right; in each slice the left switch is a B bit and the
  right one an A bit. The switch at the bus's right end is SUB: it XORs every
  B bit and is the first carry in, so the LEDs show A − B. The carry LED at the
  bottom left is the carry out (no borrow, when subtracting).

## Diagram format (`.bnbx`)

A 10-byte header, then one byte per cell, row by row:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `BNBX` |
| 4 | 1 | version, `1` |
| 5 | 1 | reserved, `0` |
| 6 | 2 | width, `256`, little-endian |
| 8 | 2 | height, `128`, little-endian |
| 10 | w × h | cell codes |

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
| `group.ace` | Groups of touching cells: nets and multi-cell blocks. |
| `sim.ace` | Levels, drivers and contention, and the tick. |
| `speed.ace` | The Speed slider's stops as tick rates, and their Hz/kHz/MHz text. |
| `diagram.ace` | `.bnbx` encode and decode. |
| `ui.ace` | Rectangles, layered drawing, and text helpers (text size 16 px). |
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
| `font.ace` | The text font (Lilex SemiBold, monospaced, printable ASCII), generated from `font/Lilex-SemiBold.ttf` by `python3 -I font/bake.py`; do not edit by hand. License in `font/LICENSE.txt`. |
| `main.ace` | The SDL window and the event loop. |
| `*.test.ace` | Tests for each module, run with `ace test .`. |
