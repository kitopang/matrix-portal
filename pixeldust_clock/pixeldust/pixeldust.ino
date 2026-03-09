/* ----------------------------------------------------------------------
   "Pixel dust" Protomatter library example. As written, this is
   SPECIFICALLY FOR THE ADAFRUIT MATRIXPORTAL with 64x32 pixel matrix.
   Change "HEIGHT" below for 64x64 matrix. Could also be adapted to other
   Protomatter-capable boards with an attached LIS3DH accelerometer.

   PLEASE SEE THE "simple" EXAMPLE FOR AN INTRODUCTORY SKETCH,
   or "doublebuffer" for animation basics.
   ------------------------------------------------------------------------- */

#include <Wire.h>                 // For I2C communication
#include <Adafruit_LIS3DH.h>      // For accelerometer
#include <Adafruit_PixelDust.h>   // For sand simulation
#include <Adafruit_Protomatter.h> // For RGB matrix
#include <WiFi.h>                 // For WiFi (ESP32-S3)
#include <time.h>                 // For NTP time sync

#define HEIGHT  64 // Matrix height (pixels) - SET TO 64 FOR 64x64 MATRIX!
#define WIDTH   64 // Matrix width (pixels)
#define MAX_FPS 300 // Maximum redraw rate, frames/second

// ── WiFi / NTP ─────────────────────────────────────────────────────────────
#define WIFI_SSID   "CourierHealthCorp"       // <-- fill in your WiFi SSID
#define WIFI_PASS   "PXinnov@tors115!"   // <-- fill in your WiFi password
#define NTP_SERVER  "pool.ntp.org"

// ── Buttons ────────────────────────────────────────────────────────────────
#define BUTTON_UP   6  // MatrixPortal S3 UP button (active LOW)

// ── Clock geometry (64x64 display, center at pixel 32,32) ──────────────────
#define CENTER_X    32
#define CENTER_Y    32
#define HOUR_LEN    13   // hour hand length in pixels from center
#define MINUTE_LEN  20   // minute hand length in pixels from center
#define SECOND_LEN  21   // second hand length in pixels from center
#define MAX_HAND_PX 32   // fixed array upper bound for hand pixel lists

#if defined(_VARIANT_MATRIXPORTAL_M4_) // MatrixPortal M4
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

#if HEIGHT == 16
#define NUM_ADDR_PINS 3
#elif HEIGHT == 32
#define NUM_ADDR_PINS 4
#elif HEIGHT == 64
#define NUM_ADDR_PINS 5
#endif

Adafruit_Protomatter matrix(
  WIDTH, 4, 1, rgbPins, NUM_ADDR_PINS, addrPins,
  clockPin, latchPin, oePin, true);

Adafruit_LIS3DH accel = Adafruit_LIS3DH();

#define N_COLORS   8
#define BOX_HEIGHT 8  // Increased from 8 for more sand
#define N_GRAINS (BOX_HEIGHT*N_COLORS*8)
uint16_t colors[N_COLORS];

Adafruit_PixelDust sand(WIDTH, HEIGHT, N_GRAINS, 1, 128, false);

uint32_t prevTime = 0; // Used for frames-per-second throttle

// ── Clock hand state ────────────────────────────────────────────────────────
uint16_t hourColor, minuteColor, secondColor;

// Stored pixel lists for the currently-active obstacle positions of each hand.
int8_t hourPixelsX[MAX_HAND_PX],   hourPixelsY[MAX_HAND_PX];
int8_t minutePixelsX[MAX_HAND_PX], minutePixelsY[MAX_HAND_PX];
int8_t secondPixelsX[MAX_HAND_PX], secondPixelsY[MAX_HAND_PX];
uint8_t hourCount   = 0;
uint8_t minuteCount = 0;
uint8_t secondCount = 0;

bool sandEnabled     = false; // toggled by UP button
uint32_t lastBtnTime = 0;    // debounce timestamp

// SETUP - RUNS ONCE AT PROGRAM START -----------------------------------------

void err(int x) {
  uint8_t i;
  pinMode(LED_BUILTIN, OUTPUT);       // Using onboard LED
  for(i=1;;i++) {                     // Loop forever...
    digitalWrite(LED_BUILTIN, i & 1); // LED on/off blink to alert user
    delay(x);
  }
}

// Connect to WiFi and sync time via NTP.
// Uses POSIX TZ string so DST transitions are handled automatically.
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
    // configTime syncs NTP as UTC and resets TZ internally.
    // We then immediately override TZ with the correct POSIX rule.
    // Change the TZ string if you're in a different timezone:
    //   US Pacific:  "PST8PDT,M3.2.0,M11.1.0"
    //   US Mountain: "MST7MDT,M3.2.0,M11.1.0"
    //   US Central:  "CST6CDT,M3.2.0,M11.1.0"
    //   US Eastern:  "EST5EDT,M3.2.0,M11.1.0"
    configTime(0, 0, NTP_SERVER, "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    Serial.println("NTP sync requested. Waiting...");
    struct tm t;
    if (getLocalTime(&t, 10000)) {
      Serial.printf("Time synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial.println("NTP sync timed out. Clock will show 12:00 until synced.");
    }
  } else {
    Serial.println("\nWiFi connection failed. Clock will show 12:00.");
  }
}

// Bresenham line: fills px[], py[] with all pixels from (x0,y0) to (x1,y1).
// Returns pixel count. Arrays must be at least MAX_HAND_PX elements.
uint8_t bresenham(int x0, int y0, int x1, int y1, int8_t *px, int8_t *py) {
  uint8_t count = 0;
  int dx =  abs(x1 - x0);
  int dy = -abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  while (true) {
    if (x0 >= 0 && x0 < WIDTH && y0 >= 0 && y0 < HEIGHT && count < MAX_HAND_PX) {
      px[count] = (int8_t)x0;
      py[count] = (int8_t)y0;
      count++;
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
  return count;
}

// Angle in degrees: 0 = 12 o'clock (top), clockwise.
// Hour hand completes one revolution in 12 hours (30 deg/hr + 0.5 deg/min).
float hourAngleDeg(uint8_t h, uint8_t m) {
  return (h % 12) * 30.0f + m * 0.5f;
}

// Minute hand completes one revolution in 60 minutes (6 deg/min).
float minuteAngleDeg(uint8_t m) {
  return m * 6.0f;
}

// Second hand completes one revolution in 60 seconds (6 deg/sec).
float secondAngleDeg(uint8_t s) {
  return s * 6.0f;
}

// Compute pixel list for a clock hand at the given angle and length.
// Writes into px[], py[]; returns pixel count.
uint8_t computeHandPixels(float angleDeg, uint8_t length, int8_t *px, int8_t *py) {
  float rad = angleDeg * (float)M_PI / 180.0f;
  int tipX = CENTER_X + (int)roundf((float)length * sinf(rad));
  int tipY = CENTER_Y - (int)roundf((float)length * cosf(rad));
  tipX = constrain(tipX, 0, WIDTH  - 1);
  tipY = constrain(tipY, 0, HEIGHT - 1);
  return bresenham(CENTER_X, CENTER_Y, tipX, tipY, px, py);
}

// Draw clock hands on the matrix buffer (call after drawing sand grains).
// Hands are drawn on top so they're always visible.
void drawHandsOnMatrix() {
  for (uint8_t i = 0; i < hourCount; i++) {
    matrix.drawPixel(hourPixelsX[i], hourPixelsY[i], hourColor);
  }
  for (uint8_t i = 0; i < minuteCount; i++) {
    matrix.drawPixel(minutePixelsX[i], minutePixelsY[i], minuteColor);
  }
  for (uint8_t i = 0; i < secondCount; i++) {
    matrix.drawPixel(secondPixelsX[i], secondPixelsY[i], secondColor);
  }
  // Center pivot dot — drawn last so it's always on top of all hands.
  matrix.drawPixel(CENTER_X, CENTER_Y, hourColor);
}

void setup(void) {
  Serial.begin(115200);
  //while (!Serial) delay(10);

  ProtomatterStatus status = matrix.begin();
  Serial.printf("Protomatter begin() status: %d\n", status);

  if (!sand.begin()) {
    Serial.println("Couldn't start sand");
    err(1000); // Slow blink = malloc error
  }

  if (!accel.begin(0x19)) {
    Serial.println("Couldn't find accelerometer");
    err(250);  // Fast bink = I2C error
  }
  accel.setRange(LIS3DH_RANGE_4_G);   // 2, 4, 8 or 16 G!

  pinMode(BUTTON_UP, INPUT_PULLUP);   // UP button toggles sand on/off

  //sand.randomize(); // Initialize random sand positions

  // Set up initial sand coordinates, in 8x8 blocks
  int n = 0;
  for(int i=0; i<N_COLORS; i++) {
    int xx = i * WIDTH / N_COLORS;
    int yy =  HEIGHT - BOX_HEIGHT;
    for(int y=0; y<BOX_HEIGHT; y++) {
      for(int x=0; x < WIDTH / N_COLORS; x++) {
        sand.setPosition(n++, xx + x, yy + y);
      }
    }
  }
  Serial.printf("%d total pixels\n", n);

  colors[0] = matrix.color565(32, 32, 32);  // Dark Gray (dimmed)
  colors[1] = matrix.color565(60, 40, 12);  // Brown (dimmed)
  colors[2] = matrix.color565(114, 2,  2);  // Red (dimmed)
  colors[3] = matrix.color565(128, 70, 0);  // Orange (dimmed)
  colors[4] = matrix.color565(128,119, 0);  // Yellow (dimmed)
  colors[5] = matrix.color565(  0, 64, 19); // Green (dimmed)
  colors[6] = matrix.color565(  0, 39,128); // Blue (dimmed)
  colors[7] = matrix.color565( 59,  4, 68); // Purple (dimmed)

  // ── Clock setup ─────────────────────────────────────────────────────────
  hourColor   = matrix.color565(255, 255, 255); // Bright white
  minuteColor = matrix.color565(  0, 200, 255); // Bright cyan
  secondColor = matrix.color565(255,  50,   0); // Bright red-orange

  setupWiFiNTP();

  // Initialize hand obstacles at the current time (12:00 if NTP failed).
  struct tm t;
  uint8_t initH = 0, initM = 0, initS = 0;
  if (getLocalTime(&t, 0)) {
    initH = t.tm_hour;
    initM = t.tm_min;
    initS = t.tm_sec;
  }

  hourCount   = computeHandPixels(hourAngleDeg(initH, initM), HOUR_LEN,   hourPixelsX,   hourPixelsY);
  minuteCount = computeHandPixels(minuteAngleDeg(initM),      MINUTE_LEN, minutePixelsX, minutePixelsY);
  secondCount = computeHandPixels(secondAngleDeg(initS),      SECOND_LEN, secondPixelsX, secondPixelsY);
  for (uint8_t i = 0; i < hourCount;   i++) sand.setPixel(hourPixelsX[i],   hourPixelsY[i]);
  for (uint8_t i = 0; i < minuteCount; i++) sand.setPixel(minutePixelsX[i], minutePixelsY[i]);
  for (uint8_t i = 0; i < secondCount; i++) sand.setPixel(secondPixelsX[i], secondPixelsY[i]);
}

// MAIN LOOP - RUNS ONCE PER FRAME OF ANIMATION --------------------------------

void loop() {
  // Limit the animation frame rate to MAX_FPS.  Because the subsequent sand
  // calculations are non-deterministic (don't always take the same amount
  // of time, depending on their current states), this helps ensure that
  // things like gravity appear constant in the simulation.
  uint32_t t;
  while(((t = micros()) - prevTime) < (1000000L / MAX_FPS));
  prevTime = t;

  // ── UP button: toggle sand on/off (debounced 200 ms) ─────────────────────
  if (digitalRead(BUTTON_UP) == LOW && (millis() - lastBtnTime) > 200) {
    sandEnabled  = !sandEnabled;
    lastBtnTime  = millis();
  }

  // ── Clock: update hand obstacles when time changes ────────────────────────
  // getLocalTime() with 0ms timeout is non-blocking — reads the internal RTC.
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    uint8_t curH = timeinfo.tm_hour;
    uint8_t curM = timeinfo.tm_min;
    uint8_t curS = timeinfo.tm_sec;

    // Compute new pixel lists into temporaries.
    int8_t newHX[MAX_HAND_PX], newHY[MAX_HAND_PX];
    int8_t newMX[MAX_HAND_PX], newMY[MAX_HAND_PX];
    int8_t newSX[MAX_HAND_PX], newSY[MAX_HAND_PX];
    uint8_t newHC = computeHandPixels(hourAngleDeg(curH, curM), HOUR_LEN,   newHX, newHY);
    uint8_t newMC = computeHandPixels(minuteAngleDeg(curM),     MINUTE_LEN, newMX, newMY);
    uint8_t newSC = computeHandPixels(secondAngleDeg(curS),     SECOND_LEN, newSX, newSY);

    // Hour hand: update sand obstacles only if the pixel list changed.
    if (newHC != hourCount
        || memcmp(newHX, hourPixelsX, newHC) != 0
        || memcmp(newHY, hourPixelsY, newHC) != 0) {
      for (uint8_t i = 0; i < hourCount; i++) sand.clearPixel(hourPixelsX[i], hourPixelsY[i]);
      for (uint8_t i = 0; i < newHC;     i++) sand.setPixel(newHX[i], newHY[i]);
      memcpy(hourPixelsX, newHX, newHC);
      memcpy(hourPixelsY, newHY, newHC);
      hourCount = newHC;
    }

    // Minute hand: same pattern.
    if (newMC != minuteCount
        || memcmp(newMX, minutePixelsX, newMC) != 0
        || memcmp(newMY, minutePixelsY, newMC) != 0) {
      for (uint8_t i = 0; i < minuteCount; i++) sand.clearPixel(minutePixelsX[i], minutePixelsY[i]);
      for (uint8_t i = 0; i < newMC;       i++) sand.setPixel(newMX[i], newMY[i]);
      memcpy(minutePixelsX, newMX, newMC);
      memcpy(minutePixelsY, newMY, newMC);
      minuteCount = newMC;
    }

    // Second hand: same pattern, updates every second.
    if (newSC != secondCount
        || memcmp(newSX, secondPixelsX, newSC) != 0
        || memcmp(newSY, secondPixelsY, newSC) != 0) {
      for (uint8_t i = 0; i < secondCount; i++) sand.clearPixel(secondPixelsX[i], secondPixelsY[i]);
      for (uint8_t i = 0; i < newSC;       i++) sand.setPixel(newSX[i], newSY[i]);
      memcpy(secondPixelsX, newSX, newSC);
      memcpy(secondPixelsY, newSY, newSC);
      secondCount = newSC;
    }
  }

  // ── Sand physics ──────────────────────────────────────────────────────────
  sensors_event_t event;
  accel.getEvent(&event);

  double xx, yy, zz;
  xx = event.acceleration.x * 1000;
  yy = event.acceleration.y * 1000;
  zz = event.acceleration.z * 1000;

  if (sandEnabled) sand.iterate(xx, yy, zz);

  // ── Render: sand grains, then clock hands on top ──────────────────────────
  dimension_t x, y;
  matrix.fillScreen(0x0);
  if (sandEnabled) {
    for (int i = 0; i < N_GRAINS; i++) {
      sand.getPosition(i, &x, &y);
      int n = i / ((WIDTH / N_COLORS) * BOX_HEIGHT); // Color index
      matrix.drawPixel(x, y, colors[n]);
    }
  }
  drawHandsOnMatrix(); // Hands drawn after sand so they appear on top
  matrix.show();       // Copy data to matrix buffers
}
