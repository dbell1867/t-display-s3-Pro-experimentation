// ============================================================================
//  Stage 2c: A smooth touch trail (interpolation)
//
//  Touch the screen to draw; tap the on-screen CLEAR button to wipe. Builds on
//  Stage 2b, and fixes the "gaps get wider the faster you drag" problem we
//  investigated by measuring the sample rate (see lesson Module 8).
//
//  THE FIX — INTERPOLATION. The touch controller only reports ~40 frames/sec, so
//  a fast finger jumps several pixels between reports. Instead of drawing an
//  isolated dot per report (which leaves gaps), we CONNECT consecutive reports:
//  drawStroke() fills in dots along the straight line between the previous point
//  and the current one. The trail is then continuous at ANY finger speed —
//  geometry fills what the hardware sampling can't. Each new touch starts a fresh
//  stroke so we never draw a line across the screen from the last tap.
//
//  Also carried over from Stage 2b:
//   - Native-USB Serial.print can BLOCK; Serial.setTxTimeoutMs(0) makes it drop.
//   - HIT-TESTING: the CLEAR button is a rectangle; a touch inside it "presses".
//
//  Calibration (portrait, rotation 0): the CST226SE reports coordinates already
//  aligned to the display, so NO setSwapXY / setMirrorXY is needed.
//  Input design: IRQ-gated polling (design B) via touch.isPressed() on GPIO 21.
//  Loop delay(2) (~40 Hz) balances responsiveness vs. spinning the CPU flat out.
//  See docs/lesson-02-touch.md for the full write-up.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>                   // I2C bus library (SDA + SCL, shared by chips)
#include <Arduino_GFX_Library.h>    // display driver (from Stage 2a)
#include <TouchDrvCSTXXX.hpp>       // SensorLib's touch driver; auto-detects CST226SE

// ---- Display pins (SPI) — unchanged from Stage 2a ----
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_MISO  8
#define TFT_CS   39
#define TFT_DC    9
#define TFT_RST  47
#define TFT_BL   48

// ---- Touch pins (I2C) — researched from LilyGO's CapacitiveTouch example ----
#define TOUCH_SDA  5
#define TOUCH_SCL  6
#define TOUCH_RST 13
#define TOUCH_IRQ 21

#define SCREEN_W 222
#define SCREEN_H 480

// ---- CLEAR button geometry: a bar across the bottom of the screen ----
#define BTN_X 0
#define BTN_Y 430
#define BTN_W 222
#define BTN_H 50

// ---- Display objects (pointers via `new`, so we use `->`) ----
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *gfx = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

// ---- Touch object (a plain object, so we use `.` not `->`) ----
TouchDrvCSTXXX touch;
int16_t touchX[5];
int16_t touchY[5];

#define DOT_RADIUS 3               // stroke thickness (radius of each brush dot)

// The touch IRQ only PULSES per frame, so isPressed() reads false BETWEEN frames
// even while a finger is down (we discovered this while measuring the sample rate
// — see lesson Module 8). So we can't use "is the pin asserted right now?" to
// tell a continuous drag from a fresh tap. Instead we time the gap between
// accepted frames: if the previous touch was under STROKE_TIMEOUT_MS ago, this is
// the SAME finger-down session (continue the stroke / don't re-fire the button);
// if it was longer ago, it's a new press.
#define STROKE_TIMEOUT_MS 200

// ---- Stroke state: the previous touch point, so we can connect to it -------
int16_t prevX = 0, prevY = 0;
bool haveStroke = false;           // do we have a previous point to draw a line from?
uint32_t lastTouchMs = 0;          // millis() of the last accepted touch frame

void onHomeButton(void *user_data) {
  if (Serial) Serial.println("[home button pressed]");
}

// ---- Small UI helpers ----------------------------------------------------

// Title text at the top.
void drawTitle() {
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 12);
  gfx->println("Touch to draw");
}

// Draw the CLEAR button. `pressed` flips the colors for a moment of feedback.
void drawClearButton(bool pressed) {
  uint16_t bg = pressed ? RGB565_WHITE      : RGB565_DODGERBLUE;
  uint16_t fg = pressed ? RGB565_DODGERBLUE : RGB565_WHITE;
  gfx->fillRect(BTN_X, BTN_Y, BTN_W, BTN_H, bg);
  gfx->setTextColor(fg);
  gfx->setTextSize(3);
  gfx->setCursor(66, BTN_Y + 13);   // roughly centered ("CLEAR" ~90px wide)
  gfx->print("CLEAR");
}

// Wipe everything and redraw the fixed UI (title + button).
void clearScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawTitle();
  drawClearButton(false);
}

// Hit-testing: is the point (x, y) inside the CLEAR button's rectangle?
bool insideClearButton(int16_t x, int16_t y) {
  return x >= BTN_X && x < BTN_X + BTN_W &&
         y >= BTN_Y && y < BTN_Y + BTN_H;
}

// INTERPOLATION: draw a continuous stroke from (x0,y0) to (x1,y1) by placing a
// brush dot every ~2 px along the straight line between them. This fills the gap
// the touch controller's low frame rate would otherwise leave — the trail looks
// smooth no matter how fast the finger moved between two reports.
//
// This is linear interpolation: for i from 0..steps, the point is
//   (x0 + dx*i/steps, y0 + dy*i/steps)   — a fraction i/steps of the way along.
// (dx*i can exceed a 16-bit range, so we compute in 32-bit to avoid overflow.)
void drawStroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  int16_t dx = x1 - x0;
  int16_t dy = y1 - y0;
  int16_t span = max(abs(dx), abs(dy));   // pixels the finger jumped
  int16_t steps = span / 2;               // one dot per ~2 px
  if (steps < 1) steps = 1;               // always draw at least the endpoint
  for (int16_t i = 0; i <= steps; i++) {
    int16_t x = x0 + (int32_t)dx * i / steps;
    int16_t y = y0 + (int32_t)dy * i / steps;
    gfx->fillCircle(x, y, DOT_RADIUS, RGB565_YELLOW);
  }
}

// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // FREEZE/LAG FIX: make debug serial NON-BLOCKING. On native USB, Serial.print
  // blocks when the TX buffer fills and no host is draining it — and "no host"
  // includes the common case where the port is USB-connected but no monitor app
  // is reading. A 0 ms TX timeout tells the driver to DROP output instead of
  // waiting, so the loop never stalls whether or not anyone is listening.
  // (The `if (Serial)` guards below are then just an optimization.)
  Serial.setTxTimeoutMs(0);
  delay(500);
  if (Serial) Serial.println("Stage 2c: smooth touch trail (interpolation)...");

  // Display
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  if (!gfx->begin()) {
    if (Serial) Serial.println("ERROR: gfx->begin() failed!");
  }

  // Touch
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, TOUCH_SDA, TOUCH_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  } else {
    if (Serial) Serial.println("Touch controller online.");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);
  touch.setHomeButtonCallback(onHomeButton);

  clearScreen();   // paint the initial UI
  if (Serial) Serial.println("Setup done. Touch the screen.");
}

void loop() {
  // Design B: IRQ-gated polling — only hit the I2C bus when there's a touch.
  if (touch.isPressed()) {
    uint8_t count = touch.getPoint(touchX, touchY, 5);
    if (count > 0) {
      // Track a single primary finger (point 0) for a smooth single stroke.
      int16_t x = touchX[0];
      int16_t y = touchY[0];
      uint32_t now = millis();

      // Continuation of the same finger-down, or a brand-new press? The IRQ
      // pulses (see the note by STROKE_TIMEOUT_MS), so we decide by timing.
      bool sameSession = (now - lastTouchMs) < STROKE_TIMEOUT_MS;

      if (insideClearButton(x, y)) {
        if (!sameSession) {          // fire once, only on a NEW press that lands here
          drawClearButton(true);     // brief visual feedback
          delay(60);
          clearScreen();
        }
        haveStroke = false;          // don't connect a stroke across the button
      } else {
        // Drawing area. Continue the stroke if this is the same session and we
        // have a previous point; otherwise start fresh with a single dot.
        if (sameSession && haveStroke) {
          drawStroke(prevX, prevY, x, y);
        } else {
          gfx->fillCircle(x, y, DOT_RADIUS, RGB565_YELLOW);
        }
        prevX = x; prevY = y;
        haveStroke = true;
      }
      lastTouchMs = now;
    }
  }

  delay(2);   // ~40 Hz: responsive without spinning the CPU flat out
}
