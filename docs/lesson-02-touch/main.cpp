// ============================================================================
//  Stage 2b (polished): Capacitive touch you can actually play with
//
//  Touch the screen to leave yellow dots; tap the on-screen CLEAR button to
//  wipe them. Builds directly on the Stage-2a display and the SensorLib touch
//  driver, and fixes a real bug we hit along the way.
//
//  Two lessons baked into this version:
//   1) FREEZE/LAG FIX (two parts). On the ESP32-S3's native USB, Serial.print()
//      BLOCKS when the TX buffer fills and no host drains it — froze the loop
//      after ~15 touches. First fix (`if (Serial)` guard) helped but wasn't
//      enough: `Serial` reads true whenever the port is USB-connected, even with
//      no monitor app reading, so prints still queued and stalled (growing lag).
//      Real fix: Serial.setTxTimeoutMs(0) makes writes DROP instead of wait.
//   2) HIT-TESTING. The CLEAR button is just a rectangle we draw; a touch
//      "presses" it when its (x,y) falls inside that rectangle. This is the core
//      idea behind every on-screen button (and a preview of Stage 3).
//
//  Calibration (portrait, rotation 0): the CST226SE reports coordinates already
//  aligned to the display, so NO setSwapXY / setMirrorXY is needed.
//  Input design: IRQ-gated polling (design B) via touch.isPressed() on GPIO 21.
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

// Edge detection: remember if we were touching last loop, so the CLEAR button
// fires ONCE per tap instead of repeatedly while a finger rests on it.
bool wasPressed = false;

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
  if (Serial) Serial.println("Stage 2b (polished): touch + clear...");

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
  bool pressed = touch.isPressed();

  if (pressed) {
    uint8_t count = touch.getPoint(touchX, touchY, 5);
    for (uint8_t i = 0; i < count; i++) {
      int16_t x = touchX[i];
      int16_t y = touchY[i];

      // Optimization: skip formatting the string when nothing is listening.
      // (Safety against stalls comes from setTxTimeoutMs(0) in setup(), not this.)
      if (Serial) {
        Serial.printf("touch %d: x=%d y=%d\n", i, x, y);
      }

      if (insideClearButton(x, y)) {
        // Edge-triggered: only act on the initial press of a fresh tap.
        if (!wasPressed) {
          drawClearButton(true);   // brief visual feedback
          delay(60);
          clearScreen();
        }
      } else {
        // Anywhere else = drawing area. Continuous, so dragging leaves a trail.
        gfx->fillCircle(x, y, 5, RGB565_YELLOW);
      }
    }
  }

  wasPressed = pressed;   // remember for next loop's edge detection
  delay(10);
}
