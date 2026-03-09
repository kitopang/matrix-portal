/* -----------------------------------------------------------------------
   Cityscape — Manhattan skyline from Brooklyn, nighttime
   For Adafruit MatrixPortal with 64x64 RGB LED matrix.

   Features:
     - Manhattan skyline silhouette with uniform building heights
     - Twinkling stars on pitch-black sky
     - Crescent moon
     - Randomly lit warm-yellow building windows that occasionally toggle
   ----------------------------------------------------------------------- */

#include <Adafruit_Protomatter.h>

#define HEIGHT   64
#define WIDTH    64
#define MAX_FPS  30

// ----- Pin definitions (M4 vs ESP32-S3) ---------------------------------
#if defined(_VARIANT_MATRIXPORTAL_M4_)
uint8_t rgbPins[]  = {7, 8, 9, 10, 11, 12};
uint8_t addrPins[] = {17, 18, 19, 20, 21};
uint8_t clockPin   = 14;
uint8_t latchPin   = 15;
uint8_t oePin      = 16;
#else // MatrixPortal ESP32-S3
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;
#endif

Adafruit_Protomatter matrix(
  WIDTH, 4, 1, rgbPins, 5, addrPins,
  clockPin, latchPin, oePin, true);

// ----- Building silhouette ----------------------------------------------
// buildingTop[x] = y coordinate of the rooftop for column x.
// Buildings fill from buildingTop[x] down to y=63 (bottom of screen).
// Lower y = taller building. All tops in y=28–40 for uniform skyline.
#define GROUND_Y  (HEIGHT - 1)  // y=63, buildings go to screen bottom

const uint8_t buildingTop[WIDTH] = {
  // Buildings occupy the bottom third (y=43–63). Tops in y=43–53 range.
  49, 49, 49,      // x=0–2
  53,              // x=3
  47, 47,          // x=4–5
  51, 51, 51,      // x=6–8
  45, 45,          // x=9–10
  49,              // x=11
  43, 43, 43,      // x=12–14
  47, 47,          // x=15–16
  53, 53,          // x=17–18
  45, 45, 45,      // x=19–21
  51,              // x=22
  49, 49,          // x=23–24
  43, 43,          // x=25–26
  47, 47, 47,      // x=27–29
  53, 53,          // x=30–31
  45, 45,          // x=32–33
  49, 49, 49,      // x=34–36
  43, 43,          // x=37–38
  51, 51, 51,      // x=39–41
  47, 47,          // x=42–43
  53,              // x=44
  45, 45, 45,      // x=45–47
  49, 49,          // x=48–49
  43, 43,          // x=50–51
  51, 51, 51,      // x=52–54
  47, 47,          // x=55–56
  53, 53,          // x=57–58
  45, 45,          // x=59–60
  49, 49, 49       // x=61–63
};

// Precomputed colors
uint16_t COL_BUILDING;
uint16_t COL_MOON;

// ----- Stars ------------------------------------------------------------
#define NUM_STARS 38
uint8_t starX[NUM_STARS];
uint8_t starY[NUM_STARS];
uint8_t starBright[NUM_STARS];

// ----- Windows ----------------------------------------------------------
// All warm tones only (yellow / amber) — no cool white
#define NUM_WINDOWS 150
uint8_t  winX[NUM_WINDOWS];
uint8_t  winY[NUM_WINDOWS];
bool     winLit[NUM_WINDOWS];
uint16_t winColors[2]; // 0=warm yellow, 1=amber

// ----- Timing -----------------------------------------------------------
uint32_t prevTime   = 0;
uint32_t frameCount = 0;

// -----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  ProtomatterStatus status = matrix.begin();
  Serial.printf("Protomatter begin() status: %d\n", status);

#if defined(_VARIANT_MATRIXPORTAL_M4_)
  randomSeed(analogRead(A0));
#else
  randomSeed(esp_random());
#endif

  // Precompute colors
  COL_BUILDING = matrix.color565(18, 18, 25);
  COL_MOON     = matrix.color565(255, 248, 200);
  winColors[0] = matrix.color565(220, 180,  60); // warm yellow
  winColors[1] = matrix.color565(200, 120,  30); // amber

  // Stars: kept well above all rooftops (min buildingTop = 28, stars at y<26)
  for (int i = 0; i < NUM_STARS; i++) {
    starX[i]      = random(WIDTH);
    starY[i]      = random(41); // stay above all rooftops (min buildingTop=43)
    starBright[i] = random(180, 255);
  }

  // Windows: random positions within each building's bounds
  int wIdx = 0, attempts = 0;
  while (wIdx < NUM_WINDOWS && attempts < 10000) {
    attempts++;
    uint8_t wx   = random(WIDTH);
    uint8_t bTop = buildingTop[wx];
    uint8_t wy   = random(bTop + 1, GROUND_Y);
    winX[wIdx]   = wx;
    winY[wIdx]   = wy;
    winLit[wIdx] = (random(4) != 0); // 75% start lit
    wIdx++;
  }
}

// -----------------------------------------------------------------------
void loop() {
  uint32_t t;
  while (((t = micros()) - prevTime) < (1000000L / MAX_FPS));
  prevTime = t;
  frameCount++;

  // Pitch-black background
  matrix.fillScreen(0);

  // --- Stars: twinkle 3 random stars per frame ---
  for (int i = 0; i < 3; i++) {
    int idx   = random(NUM_STARS);
    int b     = (int)starBright[idx] + random(-30, 31);
    starBright[idx] = (uint8_t)constrain(b, 80, 255);
  }
  for (int i = 0; i < NUM_STARS; i++) {
    uint8_t b = starBright[i];
    matrix.drawPixel(starX[i], starY[i], matrix.color565(b, b, b));
  }

  // --- Moon (crescent) ---
  matrix.fillCircle(52, 8, 3, COL_MOON);
  matrix.fillCircle(50, 7, 2, 0); // dark bite for crescent shape

  // --- Building silhouette ---
  for (int x = 0; x < WIDTH; x++) {
    uint8_t top = buildingTop[x];
    matrix.drawFastVLine(x, top, GROUND_Y - top + 1, COL_BUILDING);
  }

  // --- Lit windows ---
  for (int i = 0; i < NUM_WINDOWS; i++) {
    if (winLit[i]) {
      // Determine color: winY odd = yellow, even = amber (consistent per window)
      uint16_t c = winColors[winY[i] & 1];
      matrix.drawPixel(winX[i], winY[i], c);
    }
  }

  // --- Occasionally toggle a window ---
  if (frameCount % 60 == 0) {
    int idx      = random(NUM_WINDOWS);
    winLit[idx]  = !winLit[idx];
  }

  matrix.show();
}
