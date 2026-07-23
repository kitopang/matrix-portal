/* -----------------------------------------------------------------------
   Cityscape — Manhattan skyline from Brooklyn, nighttime/daytime
   For Adafruit MatrixPortal with 64x64 RGB LED matrix.

   Features:
     - Manhattan skyline silhouette
     - Day mode: sky icon reflects current weather (sun/cloud/rain/snow/storm), birds fly when clear
     - Night mode: twinkling stars, crescent moon, lit building windows
     - Twilight mode: night-mode buildings, no stars, low setting-sun icon —
       used automatically for the 30 min around real sunrise/sunset in auto mode
     - Auto mode: switches at actual sunrise/sunset via Open-Meteo API
     - 24-hour digital clock (top-left, via WiFi/NTP)
     - Temperature and minutes-to-next-M-train (Central Av, Manhattan-bound) below the clock
     - UP button cycles modes; active mode shown briefly below clock

   DISPLAY_MODE options:  MODE_AUTO | MODE_DAY | MODE_NIGHT | MODE_TWILIGHT
   ----------------------------------------------------------------------- */

#include <Adafruit_Protomatter.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>

// ── WiFi / NTP ──────────────────────────────────────────────────────────────
#define WIFI_SSID  "MyOptimum 962dc1"
#define WIFI_PASS  "sepia-505-636"
#define NTP_SERVER "pool.ntp.org"
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"  // US Eastern (handles DST)

// ── Display mode ─────────────────────────────────────────────────────────────
// UP button (pin 6) cycles:  AUTO → DAY → NIGHT → TWILIGHT → AUTO …
// TWILIGHT is a manual-only mode for previewing the dawn/dusk look; AUTO
// switches into it on its own near real sunrise/sunset (see resolvePhase()).
// Change the initial value here to start in a different mode.
enum DisplayMode { MODE_AUTO, MODE_DAY, MODE_NIGHT, MODE_TWILIGHT };
DisplayMode displayMode = MODE_AUTO;

// Weather condition for the day-mode sky icon (moved up near DisplayMode so the
// Arduino IDE's auto-generated function prototypes, inserted near the top of the
// file, see this type before they reference it).
enum WeatherCond { WX_CLEAR, WX_CLOUDY, WX_RAIN, WX_SNOW, WX_STORM };

// Time-of-day phase for AUTO mode: dawn/dusk (TWILIGHT) gets night-mode
// buildings with no stars and a low setting-sun icon (see resolvePhase()).
enum DayPhase { PHASE_NIGHT, PHASE_TWILIGHT, PHASE_DAY };

// Minimal protobuf byte-buffer view (moved up near the other early type
// declarations for the same reason as WeatherCond above — Arduino's
// auto-generated prototypes need this type defined before they run).
struct PBBuf {
  const uint8_t *data;
  size_t len;
  size_t pos;
};

#define BUTTON_UP 6   // MatrixPortal S3 UP button (active LOW)

// ── Sunrise/sunset (Open-Meteo, Manhattan) ───────────────────────────────────
#define WEATHER_LAT "40.7128"
#define WEATHER_LON "-74.0060"
int sunriseMin = 6 * 60;   // fallback: 6:00 AM
int sunsetMin  = 18 * 60;  // fallback: 6:00 PM
float currentTempF = NAN;  // current temperature, updated periodically
int   currentWeatherCode = 0;  // WMO weather code, updated periodically (0 = clear)
#define WEATHER_REFRESH_MS (10UL * 60UL * 1000UL)  // 10 minutes
uint32_t lastWeatherFetchMs = 0;

// ── Next M train, Manhattan-bound (Central Av, MTA GTFS-realtime) ───────────
// Stop M10 = Central Av on the Myrtle Av line; "N" = the Manhattan-bound
// direction (MTA's North Direction Label for this stop is "Manhattan").
#define MTA_FEED_URL "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-bdfm"
#define MTA_STOP_ID  "M10N"
#define MTA_ROUTE_ID "M"
#define TRAIN_REFRESH_MS (60UL * 1000UL)  // 1 minute — MTA feed is HTTPS-only, see fetchNextTrain()
int64_t  nextTrainEpoch  = -1;  // unix time of next arrival, -1 = unknown
uint32_t lastTrainFetchMs = 0;

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
// Grid: 8 cols × 5 rows = 40 cells — one star per cell for even coverage.
// SKY_MAX_Y: 5px above the tallest building (min buildingTop = 43).
#define NUM_STARS   40
#define STAR_COLS    8
#define STAR_ROWS    5
#define SKY_MAX_Y   40   // stars kept in y = 0..40 (2px above tallest building at y=43)
// Clock/temp/train text block, banded per row (each row is a different
// width) rather than one bounding rectangle, so stars can fill the sky
// space next to a narrow row instead of being excluded by the widest one.
#define CLOCK_Y2    29   // bottom of the whole text block (used for bird spawn Y)
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

static bool parseTemperature(const String &json, float &outTemp) {
  // Skip past "current_units", which also has a (string-valued) temperature_2m key.
  int curIdx = json.indexOf("\"current\":{");
  if (curIdx < 0) return false;
  String needle = "\"temperature_2m\":";
  int idx = json.indexOf(needle, curIdx);
  if (idx < 0) return false;
  idx += needle.length();
  int endIdx = idx;
  while (endIdx < (int)json.length() &&
         (isDigit(json[endIdx]) || json[endIdx] == '.' || json[endIdx] == '-'))
    endIdx++;
  if (endIdx == idx) return false;
  outTemp = json.substring(idx, endIdx).toFloat();
  return true;
}

static bool parseWeatherCode(const String &json, int &outCode) {
  // Same "current" object as temperature_2m — see note above about current_units.
  int curIdx = json.indexOf("\"current\":{");
  if (curIdx < 0) return false;
  String needle = "\"weather_code\":";
  int idx = json.indexOf(needle, curIdx);
  if (idx < 0) return false;
  idx += needle.length();
  int endIdx = idx;
  while (endIdx < (int)json.length() && isDigit(json[endIdx]))
    endIdx++;
  if (endIdx == idx) return false;
  outCode = json.substring(idx, endIdx).toInt();
  return true;
}

// WMO weather codes: https://open-meteo.com/en/docs
WeatherCond classifyWeather(int code) {
  if (code == 95 || code == 96 || code == 99) return WX_STORM;
  if (code == 71 || code == 73 || code == 75 || code == 77 ||
      code == 85 || code == 86) return WX_SNOW;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
  if (code == 2 || code == 3 || code == 45 || code == 48) return WX_CLOUDY;
  return WX_CLEAR;  // 0, 1, or unrecognized
}

void fetchSunTimes() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=" WEATHER_LAT
    "&longitude=" WEATHER_LON
    "&daily=sunrise,sunset"
    "&current=temperature_2m,weather_code"
    "&temperature_unit=fahrenheit"
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
    float temp;
    if (parseTemperature(body, temp)) {
      currentTempF = temp;
      Serial.printf("Temperature: %.1f F\n", temp);
    } else {
      Serial.println("Temperature: parse failed.");
    }
    int code;
    if (parseWeatherCode(body, code)) {
      currentWeatherCode = code;
      Serial.printf("Weather code: %d\n", code);
    } else {
      Serial.println("Weather code: parse failed.");
    }
  }
  http.end();
  lastWeatherFetchMs = millis();
}

// ----- Minimal Protobuf reader (GTFS-realtime is binary protobuf, not JSON) -
// Only what's needed to walk FeedMessage → FeedEntity → TripUpdate →
// TripDescriptor.route_id / StopTimeUpdate.stop_id / StopTimeEvent.time.
static bool pbVarint(PBBuf &b, uint64_t &out) {
  out = 0;
  for (int shift = 0; shift <= 63; shift += 7) {
    if (b.pos >= b.len) return false;
    uint8_t byte = b.data[b.pos++];
    out |= (uint64_t)(byte & 0x7F) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}

static bool pbTag(PBBuf &b, uint32_t &field, uint8_t &wireType) {
  uint64_t v;
  if (!pbVarint(b, v)) return false;
  field    = (uint32_t)(v >> 3);
  wireType = (uint8_t)(v & 0x07);
  return true;
}

// Carves out a length-delimited field as a sub-buffer view (no copy).
static bool pbBytes(PBBuf &b, PBBuf &out) {
  uint64_t len;
  if (!pbVarint(b, len)) return false;
  if (b.pos + len > b.len) return false;
  out.data = b.data + b.pos;
  out.len  = len;
  out.pos  = 0;
  b.pos += len;
  return true;
}

static bool pbSkip(PBBuf &b, uint8_t wireType) {
  uint64_t v;
  PBBuf sub;
  switch (wireType) {
    case 0: return pbVarint(b, v);
    case 1: if (b.pos + 8 > b.len) return false; b.pos += 8; return true;
    case 2: return pbBytes(b, sub);
    case 5: if (b.pos + 4 > b.len) return false; b.pos += 4; return true;
    default: return false;
  }
}

static void pbReadString(const PBBuf &b, char *out, size_t outSize) {
  size_t n = b.len < outSize - 1 ? b.len : outSize - 1;
  memcpy(out, b.data, n);
  out[n] = 0;
}

// Scans one TripUpdate sub-message; if its route matches MTA_ROUTE_ID and it
// has a stop_time_update for MTA_STOP_ID, folds that arrival time into *best.
static void scanTripUpdate(PBBuf tu, int64_t *best) {
  char routeId[8] = "";
  {
    PBBuf scan = tu;
    uint32_t field; uint8_t wt;
    while (pbTag(scan, field, wt)) {
      if (field == 1 && wt == 2) {           // trip (TripDescriptor)
        PBBuf trip;
        if (!pbBytes(scan, trip)) return;
        uint32_t f2; uint8_t wt2;
        while (pbTag(trip, f2, wt2)) {
          if (f2 == 5 && wt2 == 2) {          // route_id
            PBBuf rid;
            if (!pbBytes(trip, rid)) return;
            pbReadString(rid, routeId, sizeof(routeId));
          } else if (!pbSkip(trip, wt2)) break;
        }
      } else if (!pbSkip(scan, wt)) break;
    }
  }
  if (strcmp(routeId, MTA_ROUTE_ID) != 0) return;

  PBBuf scan = tu;
  uint32_t field; uint8_t wt;
  while (pbTag(scan, field, wt)) {
    if (field == 2 && wt == 2) {              // stop_time_update
      PBBuf stu;
      if (!pbBytes(scan, stu)) return;
      char stopId[8] = "";
      int64_t arrivalTime = -1;
      uint32_t f2; uint8_t wt2;
      while (pbTag(stu, f2, wt2)) {
        if (f2 == 4 && wt2 == 2) {            // stop_id
          PBBuf sid;
          if (!pbBytes(stu, sid)) return;
          pbReadString(sid, stopId, sizeof(stopId));
        } else if (f2 == 2 && wt2 == 2) {     // arrival (StopTimeEvent)
          PBBuf ev;
          if (!pbBytes(stu, ev)) return;
          uint32_t f3; uint8_t wt3;
          while (pbTag(ev, f3, wt3)) {
            if (f3 == 2 && wt3 == 0) {        // time
              uint64_t t;
              if (!pbVarint(ev, t)) return;
              arrivalTime = (int64_t)t;
            } else if (!pbSkip(ev, wt3)) break;
          }
        } else if (!pbSkip(stu, wt2)) break;
      }
      if (arrivalTime > 0 && strcmp(stopId, MTA_STOP_ID) == 0) {
        if (*best < 0 || arrivalTime < *best) *best = arrivalTime;
      }
    } else if (!pbSkip(scan, wt)) break;
  }
}

static void parseFeedForNextTrain(const uint8_t *data, size_t len) {
  PBBuf msg{data, len, 0};
  int64_t best = -1;
  uint32_t field; uint8_t wt;
  while (pbTag(msg, field, wt)) {
    if (field == 2 && wt == 2) {              // FeedEntity
      PBBuf entity;
      if (!pbBytes(msg, entity)) break;
      uint32_t f2; uint8_t wt2;
      while (pbTag(entity, f2, wt2)) {
        if (f2 == 3 && wt2 == 2) {            // trip_update
          PBBuf tu;
          if (!pbBytes(entity, tu)) break;
          scanTripUpdate(tu, &best);
        } else if (!pbSkip(entity, wt2)) break;
      }
    } else if (!pbSkip(msg, wt)) break;
  }
  nextTrainEpoch = best;
}

void fetchNextTrain() {
  lastTrainFetchMs = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(MTA_FEED_URL);  // HTTPS only — MTA has no plain-HTTP endpoint
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    int len = http.getSize();
    if (len > 0 && len < 300000) {
      uint8_t *buf = (uint8_t *)malloc(len);
      if (buf) {
        // A single readBytes() call isn't reliable at this size: it gives up
        // as soon as any one byte doesn't arrive within its timeout, and a
        // ~140KB HTTPS transfer over WiFi can easily have gaps longer than
        // that. Keep asking for whatever's left until it's all here or an
        // overall deadline passes.
        WiFiClient *stream = http.getStreamPtr();
        stream->setTimeout(5000);
        size_t total = 0;
        uint32_t deadline = millis() + 20000;
        while (total < (size_t)len && millis() < deadline) {
          size_t n = stream->readBytes(buf + total, len - total);
          if (n > 0) total += n;
          else delay(1);
        }
        int got = (int)total;
        if (got == len) {
          parseFeedForNextTrain(buf, len);
          Serial.printf("Next M (Manhattan-bound) epoch: %lld\n", (long long)nextTrainEpoch);
        } else {
          Serial.printf("MTA feed: short read (%d of %d).\n", got, len);
        }
        free(buf);
      } else {
        Serial.println("MTA feed: malloc failed.");
      }
    } else {
      Serial.printf("MTA feed: unexpected size %d.\n", len);
    }
  } else {
    Serial.printf("MTA feed: HTTP %d.\n", code);
  }
  http.end();
}

#define TWILIGHT_MINS 30  // dawn/dusk window length, each side of sunrise/sunset

// AUTO-mode time-of-day phase: night → twilight (30 min after sunrise, and the
// 30 min before sunset) → day → twilight → night.
DayPhase resolvePhase() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return PHASE_NIGHT;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int dawnEnd   = sunriseMin + TWILIGHT_MINS;
  int duskStart = sunsetMin  - TWILIGHT_MINS;
  if (nowMin >= sunriseMin && nowMin < dawnEnd)   return PHASE_TWILIGHT;
  if (nowMin >= duskStart  && nowMin < sunsetMin) return PHASE_TWILIGHT;
  if (nowMin >= dawnEnd    && nowMin < duskStart) return PHASE_DAY;
  return PHASE_NIGHT;
}

uint16_t tempColorF(float f) {
  if      (f < 50) return matrix.color565( 40,  70, 230);  // deep blue
  else if (f < 65) return matrix.color565(120, 200, 255);  // light blue
  else if (f < 75) return matrix.color565(255, 220,  60);  // yellow
  else if (f < 85) return matrix.color565(255, 140,  30);  // orange
  else             return matrix.color565(255,  60,  40);  // red
}

// Is (x,y) inside the clock/temp/train text block? Banded per row instead of
// one rectangle, since the clock ("HH:MM") is much wider than the temp/train
// rows below it.
bool inTextZone(uint8_t x, uint8_t y) {
  if (y <= 11) return x <= 34;  // clock row
  if (y <= 20) return x <= 20;  // temperature row
  if (y <= 29) return x <= 17;  // train countdown row
  return false;
}

uint16_t trainColorMins(int mins) {
  if      (mins <= 5) return matrix.color565(255,  60,  40);  // red
  else if (mins <= 7) return matrix.color565(255, 140,  30);  // orange
  else                return matrix.color565( 80, 220, 100);  // green
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

// Low sun peeking over a horizon line — used for the dawn/dusk twilight icon.
void drawTwilightSun(int cx, int cy) {
  uint16_t core = matrix.color565(255, 130,  40);
  uint16_t glow = matrix.color565(255,  80,  30);
  matrix.fillCircle(cx, cy, 4, core);
  matrix.fillRect(cx - 5, cy + 1, 11, 5, 0);        // clip the lower half away
  matrix.drawFastHLine(cx - 6, cy + 1, 13, glow);   // horizon line

  // Reflection: 2 horizontal lines below the horizon (3 lines total with the
  // horizon), 2 rows apart.
  matrix.drawFastHLine(cx - 3, cy + 3, 7, glow);
  matrix.drawFastHLine(cx - 1, cy + 5, 3, glow);
}

void drawCloud(int cx, int cy, uint16_t color) {
  // Irregular bumps on top, all sized/placed so none dip below the base's
  // bottom row (cy+3) — that row stays a clean flat line.
  matrix.fillCircle(cx - 5, cy + 1, 2, color);
  matrix.fillCircle(cx - 2, cy - 1, 3, color);
  matrix.fillCircle(cx + 1, cy - 2, 3, color);
  matrix.fillCircle(cx + 4, cy,     2, color);
  matrix.fillCircle(cx + 6, cy + 1, 1, color);
  matrix.fillRoundRect(cx - 7, cy, 15, 4, 1, color);  // flat base, rounded corners
}

void drawRain(int cx, int cy) {
  drawCloud(cx, cy, matrix.color565(110, 110, 122));
  uint16_t dropColor = matrix.color565(90, 160, 255);
  const int8_t dropX[] = { -5, -1, 3, 6 };
  for (int i = 0; i < 4; i++) {
    int y = cy + 5 + ((frameCount / 2 + i * 3) % 8);
    matrix.drawPixel(cx + dropX[i], y, dropColor);
  }
}

void drawSnow(int cx, int cy) {
  drawCloud(cx, cy, matrix.color565(170, 172, 180));
  uint16_t flakeColor = matrix.color565(230, 235, 245);
  const int8_t flakeX[] = { -5, -1, 3, 6 };
  for (int i = 0; i < 4; i++) {
    int y = cy + 5 + ((frameCount / 6 + i * 3) % 8);
    int x = cx + flakeX[i] + (((frameCount / 6 + i) % 4 < 2) ? 0 : 1);
    matrix.drawPixel(x, y, flakeColor);
  }
}

void drawStorm(int cx, int cy) {
  drawCloud(cx, cy, matrix.color565(75, 75, 85));
  if ((frameCount % 90) < 4) {
    uint16_t boltColor = matrix.color565(255, 255, 180);
    matrix.drawLine(cx - 1, cy + 4, cx + 1, cy + 7,  boltColor);
    matrix.drawLine(cx + 1, cy + 7, cx - 1, cy + 10, boltColor);
  }
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

  // Place one star per grid cell, jittered within the cell.
  // Cells that fully overlap the text zone get relocated outside it entirely.
  {
    int cellW = WIDTH   / STAR_COLS;          // 8 px wide
    int cellH = (SKY_MAX_Y + 1) / STAR_ROWS; // ~7 px tall
    int idx   = 0;
    for (int row = 0; row < STAR_ROWS && idx < NUM_STARS; row++) {
      for (int col = 0; col < STAR_COLS && idx < NUM_STARS; col++) {
        uint8_t sx, sy;
        int tries = 0;
        do {
          sx = col * cellW + random(cellW);
          sy = row * cellH + random(cellH);
          tries++;
        } while (inTextZone(sx, sy) && tries < 10);
        // Cells fully inside the text zone have no valid spot at all, so the
        // retries above can never escape it — fall back to a random spot
        // anywhere outside the zone instead of leaving the star on the text.
        while (inTextZone(sx, sy)) {
          sx = random(WIDTH);
          sy = random(SKY_MAX_Y + 1);
        }
        starX[idx]      = sx;
        starY[idx]      = sy;
        starPeak[idx]   = random(100, 256);
        starBright[idx] = random(10, starPeak[idx] + 1);
        starTarget[idx] = random(10, starPeak[idx] + 1);
        starSpeed[idx]  = random(1, 3);
        idx++;
      }
    }
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
  fetchNextTrain();
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
    displayMode   = (DisplayMode)((displayMode + 1) % 4);
    modeShowUntil = millis() + 3000;  // show label for 3 s
    Serial.printf("Mode: %s\n",
      displayMode == MODE_AUTO ? "AUTO" : displayMode == MODE_DAY ? "DAY"
      : displayMode == MODE_NIGHT ? "NIGHT" : "TWILIGHT");
  }

  // ── Periodic weather refresh (sun times + temperature) ────────────────
  if (millis() - lastWeatherFetchMs > WEATHER_REFRESH_MS)
    fetchSunTimes();

  // ── Periodic MTA refresh (next M train) ────────────────────────────────
  if (millis() - lastTrainFetchMs > TRAIN_REFRESH_MS)
    fetchNextTrain();

  // ── Resolve day/twilight/night ──────────────────────────────────────────
  DayPhase phase;
  if      (displayMode == MODE_DAY)      phase = PHASE_DAY;
  else if (displayMode == MODE_NIGHT)    phase = PHASE_NIGHT;
  else if (displayMode == MODE_TWILIGHT) phase = PHASE_TWILIGHT;
  else                                    phase = resolvePhase();  // AUTO

  // ── Sky background ────────────────────────────────────────────────────
  matrix.fillScreen(0);  // black in all modes

  // ── Day scene ─────────────────────────────────────────────────────────
  if (phase == PHASE_DAY) {
    WeatherCond wx = classifyWeather(currentWeatherCode);
    switch (wx) {
      case WX_CLOUDY: drawCloud(52, 8, matrix.color565(190, 192, 200)); break;
      case WX_RAIN:   drawRain(52, 8);  break;
      case WX_SNOW:   drawSnow(52, 8);  break;
      case WX_STORM:  drawStorm(52, 8); break;
      default:        drawSun(52, 8);   break;  // WX_CLEAR
    }

    // Birds: only fly in clear/cloudy weather; update position and draw
    if (wx == WX_CLEAR || wx == WX_CLOUDY) {
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
    }

  // ── Twilight scene (dawn/dusk): night-mode buildings, no stars ─────────
  } else if (phase == PHASE_TWILIGHT) {
    drawTwilightSun(52, 8);

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
  uint16_t bldColor = (phase == PHASE_DAY) ? COL_BUILDING_DAY : COL_BUILDING;
  for (int x = 0; x < WIDTH; x++)
    matrix.drawFastVLine(x, buildingTop[x], GROUND_Y - buildingTop[x] + 1, bldColor);

  // ── Lit windows — drawn after buildings so they appear on top ──────────
  if (phase != PHASE_DAY) {
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
    matrix.setCursor(3, 3);
    matrix.print(timeBuf);
  }

  // Temperature (below clock, left-aligned) — degree symbol drawn as a 2x2 square
  {
    char tempBuf[4];
    if (isnan(currentTempF))
      strcpy(tempBuf, "--");
    else
      snprintf(tempBuf, sizeof(tempBuf), "%d", (int)lroundf(currentTempF));
    uint16_t tempColor = isnan(currentTempF) ? matrix.color565(140, 200, 255)
                                              : tempColorF(currentTempF);
    matrix.setTextColor(tempColor);
    matrix.setCursor(3, 12);
    matrix.print(tempBuf);
    matrix.fillRect(matrix.getCursorX(), 12, 2, 2, tempColor);
  }

  // Minutes to next Manhattan-bound M train, Central Av (below temperature)
  {
    char trainBuf[4] = "--";
    uint16_t trainColor = matrix.color565(140, 200, 255);  // neutral, unknown
    if (nextTrainEpoch > 0) {
      long secsLeft = (long)(nextTrainEpoch - time(nullptr));
      if (secsLeft < 0) secsLeft = 0;
      int minsLeft = (secsLeft + 59) / 60;  // round up, like a real countdown board
      if (minsLeft > 99) minsLeft = 99;
      snprintf(trainBuf, sizeof(trainBuf), "%d", minsLeft);
      trainColor = trainColorMins(minsLeft);
    }
    matrix.setTextColor(trainColor);
    matrix.setCursor(3, 21);
    matrix.print(trainBuf);
  }

  // Mode label (A / D / N / T) shown for 3 s after button press
  if (millis() < modeShowUntil) {
    const char *label = (displayMode == MODE_AUTO)  ? "A"
                      : (displayMode == MODE_DAY)    ? "D"
                      : (displayMode == MODE_NIGHT)  ? "N" : "T";
    matrix.setTextColor(matrix.color565(100, 200, 255));
    matrix.setCursor(58, 3);
    matrix.print(label);
  }

  matrix.show();
}
