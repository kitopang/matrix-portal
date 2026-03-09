/*
 * Sand Tetris — Adafruit Matrix Portal S3, 64×64 RGB matrix
 *
 * Layout:
 *   x=0..41  (42px, ~2/3) — Tetris play field: 13 cols × 20 rows, 3px cells
 *   x=42     (1px)        — vertical divider
 *   x=43..63 (21px, ~1/3) — HUD: score, level, lines, next piece
 *
 * Controls:
 *   Tilt LEFT          → move piece left   (ax > TILT_THRESH)
 *   Tilt RIGHT         → move piece right  (ax < -TILT_THRESH)
 *   Swift downward tilt→ hard drop         (|dAY| per frame > DROP_THRESH)
 *   Boot button GPIO 0 → rotate CW
 *
 * Sand mechanic: once a piece locks, every cell becomes an individual pixel
 * that obeys per-frame gravity — falling straight down or sliding diagonally.
 * Line clears happen whenever a full row of sand accumulates.
 */

#include <Wire.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/TomThumb.h>

// ── Hardware ─────────────────────────────────────────────────────────────────
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin = 2, latchPin = 47, oePin = 14;

Adafruit_Protomatter matrix(
  64, 4, 1, rgbPins, 5, addrPins, clockPin, latchPin, oePin, true);

Adafruit_LIS3DH accel = Adafruit_LIS3DH();

// ── Display / game layout ────────────────────────────────────────────────────
#define MATRIX_W    64
#define MATRIX_H    64
#define CELL        3       // pixels per Tetris cell
#define FIELD_COLS  13      // 13 × 3 = 39px + 2px borders = 41px
#define FIELD_ROWS  20      // 20 × 3 = 60px + 4px borders = 64px
#define PLAY_X0     1       // pixel x of column-0 left edge (1px left border)
#define PLAY_Y0     2       // pixel y of row-0 top edge    (2px top border)
#define DIVIDER_X   42      // vertical divider x coordinate
#define HUD_X       43      // HUD left edge
#define HUD_W       21      // HUD width (x=43..63)

// ── Timing ───────────────────────────────────────────────────────────────────
#define FPS           30
#define FRAME_US      (1000000L / FPS)
#define MOVE_REPEAT   140   // ms between repeated tilt-moves when held
#define DROP_COOLDOWN 700   // ms before another hard-drop can fire

uint32_t fallInterval(int lv) {
  return (uint32_t)max(100, 800 - (lv - 1) * 70);
}

// ── Controls ─────────────────────────────────────────────────────────────────
#define TILT_THRESH  2.5f   // m/s²  x-axis threshold for left/right tilt
#define DROP_THRESH  5.5f   // m/s²  delta-Y per frame threshold for hard drop
#define ROTATE_PIN   0      // GPIO 0 = boot button, active LOW

// ── Piece data ───────────────────────────────────────────────────────────────
// PIECES[type 1-7][rotation 0-3][row 0-3]
// Each row is a 4-bit mask: bit3=col0, bit2=col1, bit1=col2, bit0=col3
// Types: 1=I  2=O  3=T  4=S  5=Z  6=J  7=L
const uint8_t PIECES[8][4][4] = {
  // 0: unused
  {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}},
  // 1: I
  {{0b0000, 0b1111, 0b0000, 0b0000},   // rot 0: .... XXXX .... ....
   {0b0010, 0b0010, 0b0010, 0b0010},   // rot 1: ..X. (×4)
   {0b0000, 0b1111, 0b0000, 0b0000},   // rot 2: same as 0
   {0b0010, 0b0010, 0b0010, 0b0010}},  // rot 3: same as 1
  // 2: O
  {{0b0000, 0b0110, 0b0110, 0b0000},
   {0b0000, 0b0110, 0b0110, 0b0000},
   {0b0000, 0b0110, 0b0110, 0b0000},
   {0b0000, 0b0110, 0b0110, 0b0000}},
  // 3: T   spawn: .X. / XXX
  {{0b0100, 0b1110, 0b0000, 0b0000},   // rot 0
   {0b1000, 0b1100, 0b1000, 0b0000},   // rot 1 (CW)
   {0b1110, 0b0100, 0b0000, 0b0000},   // rot 2
   {0b0100, 0b1100, 0b0100, 0b0000}},  // rot 3 (CCW)
  // 4: S   spawn: .XX / XX.
  {{0b0110, 0b1100, 0b0000, 0b0000},
   {0b1000, 0b1100, 0b0100, 0b0000},
   {0b0110, 0b1100, 0b0000, 0b0000},
   {0b1000, 0b1100, 0b0100, 0b0000}},
  // 5: Z   spawn: XX. / .XX
  {{0b1100, 0b0110, 0b0000, 0b0000},
   {0b0100, 0b1100, 0b1000, 0b0000},
   {0b1100, 0b0110, 0b0000, 0b0000},
   {0b0100, 0b1100, 0b1000, 0b0000}},
  // 6: J   spawn: X.. / XXX
  {{0b1000, 0b1110, 0b0000, 0b0000},
   {0b1100, 0b1000, 0b1000, 0b0000},
   {0b1110, 0b0010, 0b0000, 0b0000},
   {0b0100, 0b0100, 0b1100, 0b0000}},
  // 7: L   spawn: ..X / XXX
  {{0b0010, 0b1110, 0b0000, 0b0000},
   {0b1000, 0b1000, 0b1100, 0b0000},
   {0b1110, 0b1000, 0b0000, 0b0000},
   {0b1100, 0b0100, 0b0100, 0b0000}},
};

// ── Colors ───────────────────────────────────────────────────────────────────
uint16_t COL_BG, COL_BORDER, COL_DIVIDER, COL_GHOST;
uint16_t COL_HUD_BG, COL_LABEL, COL_VALUE;
uint16_t PIECE_COL[8]; // index 0 unused

// ── Game state ───────────────────────────────────────────────────────────────
uint8_t board[FIELD_ROWS][FIELD_COLS]; // 0=empty, 1-7=piece type

int  curType, curRot, curX, curY;
int  nextType;
long score;
int  level, linesCleared;
bool gameOver;

uint32_t tLastFrame, tLastFall, tLastMove, tLastDrop;
bool     prevBtn;
float    prevAY;

// ── Piece helpers ─────────────────────────────────────────────────────────────
inline bool cellSet(int t, int rot, int row, int col) {
  return (PIECES[t][rot][row] >> (3 - col)) & 1;
}

bool canPlace(int t, int rot, int px, int py) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(t, rot, r, c)) continue;
      int bx = px + c, by = py + r;
      if (bx < 0 || bx >= FIELD_COLS || by >= FIELD_ROWS) return false;
      if (by >= 0 && board[by][bx]) return false;
    }
  }
  return true;
}

// ── Game logic ────────────────────────────────────────────────────────────────
void lockPiece() {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!cellSet(curType, curRot, r, c)) continue;
      int bx = curX + c, by = curY + r;
      if (by >= 0 && by < FIELD_ROWS && bx >= 0 && bx < FIELD_COLS)
        board[by][bx] = curType;
    }
  }
}

void checkClearLines() {
  // Count consecutive clears this pass for combo scoring
  int cleared = 0;
  for (int r = FIELD_ROWS - 1; r >= 0; r--) {
    bool full = true;
    for (int c = 0; c < FIELD_COLS; c++) {
      if (!board[r][c]) { full = false; break; }
    }
    if (!full) continue;
    for (int rr = r; rr > 0; rr--)
      memcpy(board[rr], board[rr - 1], FIELD_COLS);
    memset(board[0], 0, FIELD_COLS);
    r++; // re-check this row index after shift
    cleared++;
  }
  if (cleared > 0) {
    static const int pts[] = {0, 100, 300, 500, 800};
    score        += (long)pts[min(cleared, 4)] * level;
    linesCleared += cleared;
    level         = linesCleared / 10 + 1;
  }
}

// Per-frame sand gravity: cells fall straight down or slide diagonally
void sandStep() {
  for (int r = FIELD_ROWS - 2; r >= 0; r--) {
    bool leftFirst = r & 1; // alternate bias each row
    for (int ci = 0; ci < FIELD_COLS; ci++) {
      int c = leftFirst ? ci : (FIELD_COLS - 1 - ci);
      if (!board[r][c]) continue;
      if (!board[r + 1][c]) {
        board[r + 1][c] = board[r][c];
        board[r][c] = 0;
      } else {
        bool canL = (c > 0            && !board[r + 1][c - 1]);
        bool canR = (c < FIELD_COLS-1 && !board[r + 1][c + 1]);
        if      (canL && canR) {
          if (random(2)) board[r + 1][c - 1] = board[r][c];
          else           board[r + 1][c + 1] = board[r][c];
          board[r][c] = 0;
        } else if (canL) { board[r + 1][c - 1] = board[r][c]; board[r][c] = 0; }
        else if  (canR) { board[r + 1][c + 1] = board[r][c]; board[r][c] = 0; }
      }
    }
  }
}

void spawnPiece() {
  curType  = nextType;
  curRot   = 0;
  curX     = (FIELD_COLS - 4) / 2; // centre the 4-wide bounding box
  curY     = -1;                    // start one row above visible area
  nextType = random(1, 8);
  if (!canPlace(curType, curRot, curX, curY)) gameOver = true;
  tLastFall = millis();
}

// Animated settling — runs FIELD_ROWS physics steps with a render each time
// so the player can SEE cells cascade into gaps after a piece locks.
void settleAnimation() {
  for (int iter = 0; iter < FIELD_ROWS; iter++) {
    sandStep();
    drawPlayField();
    drawHUD();
    matrix.show();
    delay(10); // 10 ms per step → up to 200 ms total cascade
  }
  checkClearLines();
}

void hardDrop() {
  while (canPlace(curType, curRot, curX, curY + 1)) curY++;
  lockPiece();
  settleAnimation();
  spawnPiece();
  tLastDrop = millis();
}

void initGame() {
  memset(board, 0, sizeof(board));
  score = 0; level = 1; linesCleared = 0; gameOver = false;
  nextType = random(1, 8);
  spawnPiece();
}

// ── Rendering ─────────────────────────────────────────────────────────────────
void drawCell(int col, int row, uint16_t color) {
  matrix.fillRect(PLAY_X0 + col * CELL, PLAY_Y0 + row * CELL,
                  CELL, CELL, color);
}

void drawPlayField() {
  matrix.fillRect(0, 0, DIVIDER_X, MATRIX_H, COL_BG);

  // Borders
  matrix.drawFastVLine(0, 0, MATRIX_H, COL_BORDER);
  int rightEdge = PLAY_X0 + FIELD_COLS * CELL; // x = 40
  matrix.drawFastVLine(rightEdge, 0, MATRIX_H, COL_BORDER);
  matrix.drawFastHLine(0, PLAY_Y0 + FIELD_ROWS * CELL, rightEdge + 1, COL_BORDER);

  // Sand / locked cells
  for (int r = 0; r < FIELD_ROWS; r++)
    for (int c = 0; c < FIELD_COLS; c++)
      if (board[r][c]) drawCell(c, r, PIECE_COL[board[r][c]]);

  if (gameOver) return;

  // Ghost piece (where the active piece will land)
  int ghostY = curY;
  while (canPlace(curType, curRot, curX, ghostY + 1)) ghostY++;
  if (ghostY > curY) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
      if (!cellSet(curType, curRot, r, c)) continue;
      int bx = curX + c, by = ghostY + r;
      if (by >= 0 && by < FIELD_ROWS) drawCell(bx, by, COL_GHOST);
    }
  }

  // Active piece
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    if (!cellSet(curType, curRot, r, c)) continue;
    int bx = curX + c, by = curY + r;
    if (by >= 0 && by < FIELD_ROWS) drawCell(bx, by, PIECE_COL[curType]);
  }
}

void drawHUD() {
  matrix.fillRect(HUD_X, 0, HUD_W, MATRIX_H, COL_HUD_BG);
  matrix.drawFastVLine(DIVIDER_X, 0, MATRIX_H, COL_DIVIDER);

  matrix.setFont(&TomThumb);
  matrix.setTextWrap(false);

  // Score
  matrix.setTextColor(COL_LABEL);
  matrix.setCursor(HUD_X + 1, 6);
  matrix.print("SCR");
  matrix.setTextColor(COL_VALUE);
  matrix.setCursor(HUD_X + 1, 13);
  char buf[10];
  snprintf(buf, sizeof(buf), "%ld", score);
  matrix.print(buf);

  // Level
  matrix.setTextColor(COL_LABEL);
  matrix.setCursor(HUD_X + 1, 22);
  matrix.print("LVL");
  matrix.setTextColor(COL_VALUE);
  matrix.setCursor(HUD_X + 1, 29);
  matrix.print(level);

  // Lines
  matrix.setTextColor(COL_LABEL);
  matrix.setCursor(HUD_X + 1, 38);
  matrix.print("LNS");
  matrix.setTextColor(COL_VALUE);
  matrix.setCursor(HUD_X + 1, 45);
  matrix.print(linesCleared);

  // Next piece label
  matrix.setTextColor(COL_LABEL);
  matrix.setCursor(HUD_X + 1, 53);
  matrix.print("NXT");

  // Next piece preview — 2px cells, centered in HUD
  const int previewSz = 2;
  int ox = HUD_X + (HUD_W - 4 * previewSz) / 2;
  int oy = 55;
  matrix.fillRect(ox, oy, 4 * previewSz, 4 * previewSz, COL_BG);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    if (cellSet(nextType, 0, r, c))
      matrix.fillRect(ox + c * previewSz, oy + r * previewSz,
                      previewSz, previewSz, PIECE_COL[nextType]);
  }

  matrix.setFont(NULL); // restore default
}

void drawGameOver() {
  uint16_t red  = matrix.color565(220, 40,  40);
  uint16_t gray = matrix.color565(160, 160, 180);
  matrix.fillRect(2, 18, DIVIDER_X - 4, 30, matrix.color565(0, 0, 0));
  matrix.setFont(&TomThumb);
  matrix.setTextColor(red);
  matrix.setCursor(8, 25); matrix.print("GAME");
  matrix.setCursor(8, 33); matrix.print("OVER");
  matrix.setTextColor(gray);
  char buf[10];
  snprintf(buf, sizeof(buf), "%ld", score);
  matrix.setCursor(8, 41);
  matrix.print(buf);
  matrix.setFont(NULL);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  ProtomatterStatus ps = matrix.begin();
  if (ps != PROTOMATTER_OK) {
    Serial.printf("Protomatter error %d\n", ps);
    while (1);
  }
  matrix.setTextWrap(false);

  if (!accel.begin(0x19)) {
    Serial.println("LIS3DH not found — check wiring");
    while (1);
  }
  accel.setRange(LIS3DH_RANGE_4_G);

  pinMode(ROTATE_PIN, INPUT_PULLUP);

  // Colours
  COL_BG      = matrix.color565(  6,   6,  10);
  COL_BORDER  = matrix.color565( 40,  40,  60);
  COL_DIVIDER = matrix.color565( 25,  25,  40);
  COL_GHOST   = matrix.color565( 28,  28,  38);
  COL_HUD_BG  = matrix.color565(  4,   4,   8);
  COL_LABEL   = matrix.color565(100, 100, 130);
  COL_VALUE   = matrix.color565(200, 200, 255);

  PIECE_COL[0] = 0; // unused
  PIECE_COL[1] = matrix.color565(  0, 200, 220); // I — cyan
  PIECE_COL[2] = matrix.color565(220, 200,   0); // O — yellow
  PIECE_COL[3] = matrix.color565(180,   0, 180); // T — magenta
  PIECE_COL[4] = matrix.color565(  0, 180,  50); // S — green
  PIECE_COL[5] = matrix.color565(200,  30,  30); // Z — red
  PIECE_COL[6] = matrix.color565( 30,  80, 200); // J — blue
  PIECE_COL[7] = matrix.color565(220, 110,   0); // L — orange

  randomSeed(esp_random());
  initGame();

  // Prime timing so first frame has no false gestures
  sensors_event_t ev;
  accel.getEvent(&ev);
  prevAY = ev.acceleration.y;

  tLastFrame = micros();
  tLastFall  = millis();
  tLastMove  = 0;
  tLastDrop  = millis(); // prevent immediate drop on first frame
  prevBtn    = false;
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {
  // Frame-rate cap
  while ((micros() - tLastFrame) < FRAME_US);
  tLastFrame = micros();

  uint32_t now = millis();

  // Sand physics runs every frame so existing sand stays dynamic.
  // Two steps per frame makes cascades snappier without being chaotic.
  sandStep();
  sandStep();
  checkClearLines();

  // ── Game over ──────────────────────────────────────────────────────────────
  if (gameOver) {
    drawPlayField();
    drawGameOver();
    drawHUD();
    matrix.show();
    delay(4000);
    initGame();
    return;
  }

  // ── Accelerometer input ────────────────────────────────────────────────────
  sensors_event_t ev;
  accel.getEvent(&ev);
  float ax  = ev.acceleration.x;
  float ay  = ev.acceleration.y;
  float dAY = ay - prevAY;
  prevAY    = ay;

  // Left / right movement (threshold + hold-to-repeat)
  // ax > 0 → tilting left  → move piece left
  // ax < 0 → tilting right → move piece right
  // (swap the sign on ax if your board is mounted the other way)
  if ((now - tLastMove) >= (uint32_t)MOVE_REPEAT) {
    if (ax > TILT_THRESH) {
      if (canPlace(curType, curRot, curX - 1, curY)) { curX--; tLastMove = now; }
    } else if (ax < -TILT_THRESH) {
      if (canPlace(curType, curRot, curX + 1, curY)) { curX++; tLastMove = now; }
    }
  }

  // Hard drop: swift downward tilt = large |dAY| spike
  if (fabsf(dAY) > DROP_THRESH && (now - tLastDrop) > (uint32_t)DROP_COOLDOWN) {
    hardDrop();
  }

  // Rotate CW: boot button (GPIO 0, active LOW), with basic wall-kick
  bool btnNow = !digitalRead(ROTATE_PIN);
  if (btnNow && !prevBtn) {
    int newRot = (curRot + 1) % 4;
    if      (canPlace(curType, newRot, curX,     curY)) { curRot = newRot; }
    else if (canPlace(curType, newRot, curX - 1, curY)) { curX--; curRot = newRot; }
    else if (canPlace(curType, newRot, curX + 1, curY)) { curX++; curRot = newRot; }
  }
  prevBtn = btnNow;

  // ── Gravity ────────────────────────────────────────────────────────────────
  if ((now - tLastFall) >= fallInterval(level)) {
    if (canPlace(curType, curRot, curX, curY + 1)) {
      curY++;
    } else {
      lockPiece();
      settleAnimation(); // animated cascade — also calls checkClearLines()
      spawnPiece();
    }
    tLastFall = millis(); // re-read millis() after the settling pause
  }

  // ── Render ────────────────────────────────────────────────────────────────
  drawPlayField();
  drawHUD();
  matrix.show();
}
