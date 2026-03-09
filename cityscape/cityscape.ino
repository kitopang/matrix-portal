/* -----------------------------------------------------------------------
   Cityscape — Manhattan skyline from Brooklyn, nighttime
   For Adafruit MatrixPortal with 64x64 RGB LED matrix.

   Features:
     - Manhattan skyline silhouette with uniform building heights
     - Twinkling stars on pitch-black sky
     - Crescent moon
     - Randomly lit warm-yellow building windows that occasionally toggle
     - 24-hour digital clock in the top-left corner (via WiFi/NTP)
   ----------------------------------------------------------------------- */

#include <Adafruit_Protomatter.h>
#include <WiFi.h>
#include <time.h>

// ── WiFi / NTP ──────────────────────────────────────────────────────────────
#define WIFI_SSID  "MyOptimum 962dc1"
#define WIFI_PASS  "sepia-505-636"
#define NTP_SERVER "pool.ntp.org"
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"  // US Eastern (handles DST)

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
#define GROUND_Y  (HEIGHT - 1)

const uint8_t buildingTop[WIDTH] = {
  49, 49, 49,
  53,
  47, 47,
  51, 51, 51,
  45, 45,
  49,
  43, 43, 43,
  47, 47,
  53, 53,
  45, 45, 45,
  51,
  49, 49,
  43, 43,
  47, 47, 47,
  53, 53,
  45, 45,
  49, 49, 49,
  43, 43,
  51, 51, 51,
  47, 47,
  53,
  45, 45, 45,
  49, 49,
  43, 43,
  51, 51, 51,
  47, 47,
  53, 53,
  45, 45,
  49, 49, 49
};

// Precomputed colors
uint16_t COL_BUILDING;
uint16_t COL_MOON;

// ----- Stars ------------------------------------------------------------
#define NUM_STARS 38
uint8_t starX[NUM_STARS];
uint8_t starY[NUM_STARS];
uint8_t starBright[NUM_STARS];   // current brightness
uint8_t starPeak[NUM_STARS];     // max brightness this star ever reaches
uint8_t starTarget[NUM_STARS];   // twinkle target (fading toward this)
uint8_t starSpeed[NUM_STARS];    // steps per frame toward target

// ----- Windows ----------------------------------------------------------
#define NUM_WINDOWS 100
uint8_t  winX[NUM_WINDOWS];
uint8_t  winY[NUM_WINDOWS];
bool     winLit[NUM_WINDOWS];
uint16_t winColors[2];

// ----- Timing -----------------------------------------------------------
uint32_t prevTime   = 0;
uint32_t frameCount = 0;

// -----------------------------------------------------------------------
void setupWiFiNTP() {
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, NTP_SERVER, "time.nist.gov");
    setenv("TZ", TZ_STRING, 1);
    tzset();
    struct tm t;
    if (getLocalTime(&t, 10000)) {
      Serial.printf("Time synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial.println("NTP sync timed out.");
    }
  } else {
    Serial.println("\nWiFi failed — clock will show 00:00.");
  }
}

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

  COL_BUILDING = matrix.color565(18, 18, 25);
  COL_MOON     = matrix.color565(255, 248, 200);
  winColors[0] = matrix.color565(220, 180,  60);
  winColors[1] = matrix.color565(200, 120,  30);

  // Clock occupies x=1..30, y=1..8 — keep stars clear of that area
  #define CLOCK_X2 31
  #define CLOCK_Y2  9
  for (int i = 0; i < NUM_STARS; i++) {
    uint8_t sx, sy;
    do {
      sx = random(WIDTH);
      sy = random(41);
    } while (sx <= CLOCK_X2 && sy <= CLOCK_Y2);
    starX[i]      = sx;
    starY[i]      = sy;
    starPeak[i]   = random(100, 256);
    starBright[i] = random(starPeak[i] / 4, starPeak[i] + 1);
    starTarget[i] = random(starPeak[i] / 4, starPeak[i] + 1);
    starSpeed[i]  = random(1, 5);
  }

  int wIdx = 0, attempts = 0;
  while (wIdx < NUM_WINDOWS && attempts < 10000) {
    attempts++;
    uint8_t wx   = random(WIDTH);
    uint8_t bTop = buildingTop[wx];
    uint8_t wy   = random(bTop + 1, GROUND_Y);
    winX[wIdx]   = wx;
    winY[wIdx]   = wy;
    winLit[wIdx] = (random(4) != 0);
    wIdx++;
  }

  setupWiFiNTP();
}

// -----------------------------------------------------------------------
void loop() {
  uint32_t t;
  while (((t = micros()) - prevTime) < (1000000L / MAX_FPS));
  prevTime = t;
  frameCount++;

  matrix.fillScreen(0);

  // --- Stars: smooth twinkle (fade toward target, pick new target on arrival) ---
  for (int i = 0; i < NUM_STARS; i++) {
    if (starBright[i] < starTarget[i]) {
      starBright[i] = (uint8_t)min((int)starBright[i] + starSpeed[i], (int)starTarget[i]);
    } else if (starBright[i] > starTarget[i]) {
      starBright[i] = (uint8_t)max((int)starBright[i] - starSpeed[i], (int)starTarget[i]);
    } else {
      // Reached target — pick a new one at the opposite end of the range
      if (starTarget[i] == starPeak[i] / 4) {
        starTarget[i] = starPeak[i];
      } else {
        starTarget[i] = starPeak[i] / 4;
      }
    }
    uint8_t b = starBright[i];
    matrix.drawPixel(starX[i], starY[i], matrix.color565(b, b, b));
  }

  // --- Moon (crescent) ---
  matrix.fillCircle(52, 8, 3, COL_MOON);
  matrix.fillCircle(50, 7, 2, 0);

  // --- Building silhouette ---
  for (int x = 0; x < WIDTH; x++) {
    uint8_t top = buildingTop[x];
    matrix.drawFastVLine(x, top, GROUND_Y - top + 1, COL_BUILDING);
  }

  // --- Lit windows ---
  for (int i = 0; i < NUM_WINDOWS; i++) {
    if (winLit[i]) {
      uint16_t c = winColors[winY[i] & 1];
      matrix.drawPixel(winX[i], winY[i], c);
    }
  }

  // --- Occasionally toggle a window ---
  if (frameCount % 60 == 0) {
    int idx     = random(NUM_WINDOWS);
    winLit[idx] = !winLit[idx];
  }

  // --- Digital clock: HH:MM top-left ---
  // Cache last good time so a missed getLocalTime() doesn't blank the display.
  {
    static char timeBuf[6] = "--:--";
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
    matrix.setTextWrap(false);
    matrix.setTextSize(1);
    matrix.setTextColor(matrix.color565(220, 180, 60));
    matrix.setCursor(1, 1);
    matrix.print(timeBuf);
  }

  matrix.show();
}
