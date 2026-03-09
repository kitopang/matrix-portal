# Sand Tetris — Project Context

## Hardware
- **Board**: Adafruit Matrix Portal S3 (ESP32-S3)
- **Display**: 64x64 RGB LED matrix
- **Sensor**: LIS3DH accelerometer (I2C addr 0x19)
- **Libraries**: Adafruit_Protomatter, Adafruit_LIS3DH, Wire

## Matrix Portal S3 Pin Config
```cpp
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;
// 5 addr pins for 64-row matrix
```

## Display Layout (64x64) — FINAL
```
x=0..41  (42px, ~2/3)  Left  = Tetris play field
x=42     (1px)          Divider
x=43..63 (21px, ~1/3)  Right = HUD panel
```

## Play Field — FINAL
- **Cell size**: 3×3 pixels per Tetris block
- **Grid**: 13 columns × 20 rows
- **PLAY_X0=1, PLAY_Y0=2** (1px left border, 2px top border)
- **Right border**: x=40 (PLAY_X0 + 13×3 = 40)
- **Floor**: y=62 (PLAY_Y0 + 20×3 = 62)
- **Column mapping**: col c → x = 1 + c×3
- **Row mapping**: row r → y = 2 + r×3

## HUD Panel (x=43..63, 21px wide)
Uses **TomThumb** (3×5) font — include `<Fonts/TomThumb.h>`, call `matrix.setFont(&TomThumb)`.
TomThumb cursor Y is the text baseline. Text occupies ~5px above cursor Y.

Vertical layout:
- y=6  → "SCR" label
- y=13 → score value
- y=22 → "LVL" label
- y=29 → level value
- y=38 → "LNS" label
- y=45 → lines value
- y=53 → "NXT" label
- y=55 → next piece preview (2px cells, 4×4 grid = 8×8px, centered in 21px)

## Controls (Accelerometer)
| Gesture | Axis | Action |
|---------|------|--------|
| Tilt left | +X > 2.5 m/s² | Move piece left |
| Tilt right | -X < -2.5 m/s² | Move piece right |
| Swift downward tilt | ΔY spike > 6 m/s² per frame | Hard drop |
| Button GPIO 0 (boot, active LOW) | — | Rotate CW |

- Movement repeat delay: ~150ms (hold-to-repeat)
- Drop cooldown: ~500ms to prevent double-triggers
- Read accel with `accel.getEvent(&event)` → `event.acceleration.x/y/z`

## Tetromino Pieces
7 standard pieces, each stored as 4 rotation states × 4 rows of 4-bit masks.
Bit order: bit3=col0, bit2=col1, bit1=col2, bit0=col3.

Spawn: curX=3, curY=0 (piece fills visible rows 1–3 at most).
Game over: if canPlace() fails on spawn.

## Sand Mechanic
- When a piece locks, its cells convert to individual sand pixels on the board
- Each pixel falls independently each frame if space below is empty
- Pixels can slide diagonally left/right if blocked below
- Line clear: when any full row (all 10 cols filled in sand), clear and cascade above

## Piece Colors
```
I=Cyan, O=Yellow, T=Magenta, S=Green, Z=Red, J=Blue, L=Orange
Ghost = dim gray, Locked sand = slightly darker version of piece color
```

## Gravity / Level Timing
- Level 1: 800ms per row drop
- Each level: −75ms (min 100ms)
- Level up every 10 lines

## Scoring
- 1 line = 100 × level
- 2 lines = 300 × level
- 3 lines = 500 × level
- 4 lines = 800 × level

## Key Implementation Notes
- Use `esp_random()` for seeding RNG on S3
- Frame rate cap at ~30 FPS using `micros()` delta
- Ghost piece: find lowest valid Y, draw in dim color before active piece
- Sand settling: run multiple gravity passes per frame after piece locks
- Avoid naming conflicts with Adafruit libs: don't use `FONT` as a variable name
- Matrix inherits GFX, so `matrix.setTextColor()`, `matrix.setCursor()`, `matrix.print()` all work
- `matrix.color565(r,g,b)` for 16-bit color
- Call `matrix.show()` once per frame at the end

## Files
- `sand_tetris.ino` — main sketch to create
- `pixeldust.ino` — reference/sample (shows hardware init pattern)
