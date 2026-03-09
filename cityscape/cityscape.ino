/* -----------------------------------------------------------------------
   Cityscape — Manhattan skyline from Brooklyn, nighttime/daytime
   For Adafruit MatrixPortal with 64x64 RGB LED matrix.

   Features:
     - Manhattan skyline silhouette
     - Day mode: sun with rays, birds flying across the scene
     - Night mode: twinkling stars, crescent moon, lit building windows
     - Auto mode: switches at actual sunrise/sunset via Open-Meteo API
     - 24-hour digital clock (top-left, via WiFi/NTP)
     - UP button cycles modes; active mode shown briefly below clock

   DISPLAY_MODE options:  MODE_AUTO | MODE_DAY | MODE_NIGHT
   ----------------------------------------------------------------------- */

#include <Adafruit_Protomatter.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ── WiFi / NTP ──────────────────────────────────────────────────────────────
#define WIFI_SSID  "MyOptimum 962dc1"
#define WIFI_PASS  "sepia-505-636"
#define NTP_SERVER "pool.ntp.org"
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"  // US Eastern (handles DST)

// ── Display mode ─────────────────────────────────────────────────────────────
// UP button (pin 6) cycles:  AUTO → DAY → NIGHT → AUTO …
// Change the initial value here to start in a different mode.
enum DisplayMode { MODE_AUTO, MODE_DAY, MODE_NIGHT };
DisplayMode displayMode = MODE_AUTO;

#define BUTTON_UP 6   // MatrixPortal S3 UP button (active LOW)

// ── Sunrise/sunset (Open-Meteo, Manhattan) ───────────────────────────────────
#define WEATHER_LAT "40.7128"
#define WEATHER_LON "-74.0060"
int sunriseMin = 6 * 60;   // fallback: 6:00 AM
int sunsetMin  = 18 * 60;  // fallback: 6:00 PM

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
#else
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
#define GROUND_Y (HEIGHT - 1)

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
uint16_t COL_BUILDING;      // night: very dark
uint16_t COL_BUILDING_DAY;  // day: lighter grey
uint16_t COL_MOON;

// ----- Stars ------------------------------------------------------------
#define NUM_STARS 38
#define CLOCK_X2  31   // clock occupies x=1..30, y=1..8
#define CLOCK_Y2   9
uint8_t starX[NUM_STARS];
uint8_t starY[NUM_STARS];
uint8_t starBright[NUM_STARS];
uint8_t starPeak[NUM_STARS];
uint8_t starTarget[NUM_STARS];
uint8_t starSpeed[NUM_STARS];

// ----- Windows ----------------------------------------------------------
#define NUM_WINDOWS 100
uint8_t  winX[NUM_WINDOWS];
uint8_t  winY[NUM_WINDOWS];
bool     winLit[NUM_WINDOWS];
uint16_t winColors[2];

// ----- Birds (day mode) -------------------------------------------------
// Simple 3-pixel "v" shape that flaps wings up/down as it flies left→right.
#define NUM_BIRDS 3
float   birdX[NUM_BIRDS];
float   birdY[NUM_BIRDS];
float   birdSpeedX[NUM_BIRDS];   // pixels per frame
uint8_t birdFlapRate[NUM_BIRDS]; // frames per wing-beat half-cycle

// ----- Timing / UI ------------------------------------------------------
uint32_t prevTime      = 0;
uint32_t frameCount    = 0;
uint32_t lastBtnMs     = 0;       // debounce timestamp
uint32_t modeShowUntil = 0;       // millis() until which to show mode label

// -----------------------------------------------------------------------
static bool parseSunField(const String &json, const char *key, int &outMin) {
  String needle = "\""; needle += key; needle += "\":[\"";
  int idx = json.indexOf(needle);
  if (idx < 0) return false;
  int tPos = json.indexOf('T', idx + needle.length());
  if (tPos < 0 || tPos > idx + (int)needle.length() + 30) return false;
  tPos++;
  outMin = json.substring(tPos, tPos + 2).toInt() * 60
         + json.substring(tPos + 3, tPos + 5).toInt();
  return true;
}

void fetchSunTimes() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=" WEATHER_LAT
    "&longitude=" WEATHER_LON
    "&daily=sunrise,sunset"
    "&timezone=America%2FNew_York"
    "&forecast_days=1";
  http.begin(url);
  if (http.GET() == HTTP_CODE_OK) {
    String body = http.getString();
    int rise, set;
    if (parseSunField(body, "sunrise", rise) && parseSunField(body, "sunset", set)) {
      sunriseMin = rise;
      sunsetMin  = set;
      Serial.printf("Sunrise: %02d:%02d  Sunset: %02d:%02d\n",
                    rise / 60, rise % 60, set / 60, set % 60);
    } else {
      Serial.println("Sun times: parse failed, using defaults.");
    }
  }
  http.end();
}

bool isDaytime() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  return nowMin >= sunriseMin && nowMin < sunsetMin;
}

void drawSun(int cx, int cy) {
  uint16_t core = matrix.color565(255, 220,   0);
  uint16_t ray  = matrix.color565(255, 160,   0);
  matrix.fillCircle(cx, cy, 3, core);
  // Two pixels per ray: inner and outer
  const int8_t dx[] = { 0,  0,  0,  0,  7, -7,  7, -7,  5, -5,  5, -5,  6,  6, -6, -6};
  const int8_t dy[] = {-7, -8,  7,  8,  0,  0,  0,  0, -5, -5,  5,  5, -6,  6, -6,  6};
  for (int i = 0; i < 16; i++)
    matrix.drawPixel(cx + dx[i], cy + dy[i], ray);
}

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
    if (getLocalTime(&t, 10000))
      Serial.printf("Time synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    else
      Serial.println("NTP sync timed out.");
  } else {
    Serial.println("\nWiFi failed.");
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

  pinMode(BUTTON_UP, INPUT_PULLUP);

  COL_BUILDING     = matrix.color565( 18,  18,  25);
  COL_BUILDING_DAY = matrix.color565( 80,  80,  95);
  COL_MOON     = matrix.color565(255, 248, 200);
  winColors[0] = matrix.color565(220, 180,  60);
  winColors[1] = matrix.color565(200, 120,  30);

  for (int i = 0; i < NUM_STARS; i++) {
    uint8_t sx, sy;
    do {
      sx = random(WIDTH);
      sy = random(41);
    } while (sx <= CLOCK_X2 && sy <= CLOCK_Y2);
    starX[i]      = sx;
    starY[i]      = sy;
    starPeak[i]   = random(100, 256);
    starBright[i] = random(10, starPeak[i] + 1);
    starTarget[i] = random(10, starPeak[i] + 1);
    starSpeed[i]  = random(2, 7);
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

  // Birds: stagger starting positions across the screen
  for (int i = 0; i < NUM_BIRDS; i++) {
    birdX[i]        = random(WIDTH);
    birdY[i]        = random(CLOCK_Y2 + 3, 40);
    birdSpeedX[i]   = 0.1f + random(20) * 0.01f;  // 0.1–0.3 px/frame
    birdFlapRate[i] = random(8, 18);               // frames per half-flap
  }

  setupWiFiNTP();
  fetchSunTimes();
}

// -----------------------------------------------------------------------
void loop() {
  uint32_t t;
  while (((t = micros()) - prevTime) < (1000000L / MAX_FPS));
  prevTime = t;
  frameCount++;

  // ── Button: cycle mode (debounced 250 ms) ─────────────────────────────
  if (digitalRead(BUTTON_UP) == LOW && (millis() - lastBtnMs) > 250) {
    lastBtnMs = millis();
    displayMode   = (DisplayMode)((displayMode + 1) % 3);
    modeShowUntil = millis() + 3000;  // show label for 3 s
    Serial.printf("Mode: %s\n",
      displayMode == MODE_AUTO ? "AUTO" : displayMode == MODE_DAY ? "DAY" : "NIGHT");
  }

  // ── Resolve day/night ─────────────────────────────────────────────────
  bool dayMode;
  if      (displayMode == MODE_DAY)   dayMode = true;
  else if (displayMode == MODE_NIGHT) dayMode = false;
  else                                dayMode = isDaytime();

  // ── Sky background ────────────────────────────────────────────────────
  matrix.fillScreen(0);  // black in both modes

  // ── Day scene ─────────────────────────────────────────────────────────
  if (dayMode) {
    drawSun(52, 8);

    // Birds: update position and draw (same color as day buildings)
    uint16_t birdCol = COL_BUILDING_DAY;
    for (int i = 0; i < NUM_BIRDS; i++) {
      int x = (int)birdX[i];
      int y = (int)birdY[i];

      // Alternate wings up/down based on per-bird flap rate
      bool wingsUp = ((frameCount / birdFlapRate[i]) % 2 == 0);
      int wy = wingsUp ? y - 1 : y + 1;

      if (x >= 2 && x <= WIDTH - 3) {
        matrix.drawPixel(x - 2, wy, birdCol);  // left wing tip
        matrix.drawPixel(x,     y,  birdCol);  // body
        matrix.drawPixel(x + 2, wy, birdCol);  // right wing tip
      }

      // Advance; wrap to left edge with a fresh random height
      birdX[i] += birdSpeedX[i];
      if (birdX[i] > WIDTH + 3) {
        birdX[i] = -3;
        birdY[i] = random(CLOCK_Y2 + 3, 40);
      }
    }

  // ── Night scene ───────────────────────────────────────────────────────
  } else {
    // Stars: smooth twinkle
    for (int i = 0; i < NUM_STARS; i++) {
      if (starBright[i] < starTarget[i])
        starBright[i] = (uint8_t)min((int)starBright[i] + starSpeed[i], (int)starTarget[i]);
      else if (starBright[i] > starTarget[i])
        starBright[i] = (uint8_t)max((int)starBright[i] - starSpeed[i], (int)starTarget[i]);
      else
        starTarget[i] = (starTarget[i] == 10) ? starPeak[i] : 10;
      uint8_t b = starBright[i];
      matrix.drawPixel(starX[i], starY[i], matrix.color565(b, b, b));
    }

    // Moon (crescent)
    matrix.fillCircle(52, 8, 3, COL_MOON);
    matrix.fillCircle(50, 7, 2, 0);

  }

  // ── Building silhouette (always) ──────────────────────────────────────
  uint16_t bldColor = dayMode ? COL_BUILDING_DAY : COL_BUILDING;
  for (int x = 0; x < WIDTH; x++)
    matrix.drawFastVLine(x, buildingTop[x], GROUND_Y - buildingTop[x] + 1, bldColor);

  // ── Lit windows — drawn after buildings so they appear on top ──────────
  if (!dayMode) {
    for (int i = 0; i < NUM_WINDOWS; i++) {
      if (winLit[i])
        matrix.drawPixel(winX[i], winY[i], winColors[winY[i] & 1]);
    }
    if (frameCount % 60 == 0) {
      int idx     = random(NUM_WINDOWS);
      winLit[idx] = !winLit[idx];
    }
  }

  // ── Clock + mode indicator (always) ──────────────────────────────────
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  // HH:MM
  {
    static char timeBuf[6] = "--:--";
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    matrix.setTextColor(matrix.color565(220, 180, 60));
    matrix.setCursor(1, 1);
    matrix.print(timeBuf);
  }

  // Mode label (A / D / N) shown for 3 s after button press
  if (millis() < modeShowUntil) {
    const char *label = (displayMode == MODE_AUTO) ? "A"
                      : (displayMode == MODE_DAY)  ? "D" : "N";
    matrix.setTextColor(matrix.color565(100, 200, 255));
    matrix.setCursor(1, 10);
    matrix.print(label);
  }

  matrix.show();
}
