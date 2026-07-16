// ============================================================================
//  Stage 3: A small interactive app — Tap Counter
//
//  Our first real event-driven app with STATE. A big number in the middle; a
//  "TAP +1" button that increments it; a "RESET" button that zeroes it. Each
//  button flashes when pressed for feedback.
//
//  New ideas this stage:
//   1) struct Button — bundle a button's data (position, label, colour) into one
//      named type (like a Python @dataclass), so ONE inside()/drawButton() works
//      for ANY button instead of copy-pasting geometry.
//   2) STATE — an app variable (`counter`) that the UI reflects and buttons change.
//   3) EDGE DETECTION done right — a debounced "finger down" state that rises
//      ONCE per press (counter ticks once per tap; a held finger doesn't run
//      away) and falls on release. Because the touch IRQ pulses (isPressed()
//      flickers — lesson 02 Module 9), "released" is detected by a short timeout
//      with no new frame, not by trusting the flag. Buttons highlight while held.
//
//  Builds on Stage 2c's touch + hit-testing. See docs/lesson-03-*.md.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>                   // I2C bus library (SDA + SCL, shared by chips)
#include <Arduino_GFX_Library.h>    // display driver (from Stage 2a)
#include <TouchDrvCSTXXX.hpp>       // SensorLib's touch driver; auto-detects CST226SE

// ---- Display pins (SPI) — unchanged since Stage 2a ----
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_MISO  8
#define TFT_CS   39
#define TFT_DC    9
#define TFT_RST  47
#define TFT_BL   48

// ---- Touch pins (I2C) ----
#define TOUCH_SDA  5
#define TOUCH_SCL  6
#define TOUCH_RST 13
#define TOUCH_IRQ 21

#define SCREEN_W 222
#define SCREEN_H 480

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

// ---- Press/release tracking (the IRQ pulses; see lesson 02 Module 9) --------
// isPressed() flickers false between frames, so we can't read "released"
// directly. Instead: every accepted frame refreshes lastTouchMs; if no frame
// arrives for TOUCH_RELEASE_MS, the finger has lifted. `touchActive` is the
// debounced "finger is down" state — it RISES once per real press (so the
// counter ticks once per tap and a held finger doesn't run away) and FALLS on
// release. Keep TOUCH_RELEASE_MS comfortably above the ~25 ms frame interval,
// but small enough that fast tapping still registers every tap.
#define TOUCH_RELEASE_MS 60
uint32_t lastTouchMs = 0;
bool touchActive = false;

// ---- App state ----
int counter = 0;

// ---- A reusable on-screen button -------------------------------------------
// A struct is a named bundle of fields (like a Python @dataclass). One copy of
// inside()/drawButton() then works for every Button we define.
struct Button {
  int16_t x, y, w, h;    // rectangle: top-left corner + size
  const char *label;     // text drawn on it
  uint16_t color;        // its normal (unpressed) colour
};

Button tapBtn   = {  11, 300, 200, 70, "TAP +1", RGB565_DODGERBLUE };
Button resetBtn = {  11, 390, 200, 70, "RESET",  RGB565_RED        };

// The button currently shown "pressed" (highlighted while held), or nullptr.
// A pointer so it can refer to whichever Button is active — or to none.
Button *activeBtn = nullptr;

// Hit-testing: is (x, y) inside button b? `const Button &b` = pass a read-only
// reference (no copy). Same test we used for the CLEAR button, now generalized.
bool inside(const Button &b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w &&
         y >= b.y && y < b.y + b.h;
}

// Draw a button. `pressed` swaps fg/bg for a moment of visual feedback.
void drawButton(const Button &b, bool pressed) {
  uint16_t bg = pressed ? RGB565_WHITE : b.color;
  uint16_t fg = pressed ? b.color      : RGB565_WHITE;
  gfx->fillRect(b.x, b.y, b.w, b.h, bg);
  gfx->setTextColor(fg);
  gfx->setTextSize(3);
  int16_t tw = strlen(b.label) * 18;   // size-3 char ≈ 18 px wide
  int16_t th = 24;                     // size-3 char ≈ 24 px tall
  gfx->setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2);   // centered
  gfx->print(b.label);
}

// Draw the big counter value, centered. We clear just its band, not the screen,
// so the buttons don't flicker on every tap.
void drawCounter() {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", counter);
  gfx->fillRect(0, 120, SCREEN_W, 110, RGB565_BLACK);   // clear the number area
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(10);
  int16_t tw = strlen(buf) * 60;       // size-10 char ≈ 60 px wide
  gfx->setCursor((SCREEN_W - tw) / 2, 140);
  gfx->print(buf);
}

void drawTitle() {
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 24);
  gfx->println("Tap Counter");
}

// Paint the whole screen once (title + number + both buttons).
void drawUI() {
  gfx->fillScreen(RGB565_BLACK);
  drawTitle();
  drawCounter();
  drawButton(tapBtn, false);
  drawButton(resetBtn, false);
}

// Finger lifted: clear the pressed state and un-highlight the held button.
// Called both from the explicit touch-up event and from the timeout fallback.
void releaseTouch() {
  if (!touchActive) return;
  touchActive = false;
  if (activeBtn) {
    drawButton(*activeBtn, false);
    activeBtn = nullptr;
  }
}

// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // native-USB: non-blocking writes (see lesson 02)
  delay(500);
  if (Serial) Serial.println("Stage 3: tap counter...");

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

  drawUI();
  if (Serial) Serial.println("Setup done. Tap a button.");
}

void loop() {
  // Fallback release: if no touch frame has arrived for a while, assume the
  // finger lifted (covers controllers that don't send an explicit up-event).
  if (touchActive && (millis() - lastTouchMs) > TOUCH_RELEASE_MS) {
    releaseTouch();
  }

  // Design B: IRQ-gated polling — only read over I2C when the IRQ says so.
  if (touch.isPressed()) {
    uint8_t count = touch.getPoint(touchX, touchY, 5);

    if (count > 0) {
      int16_t x = touchX[0];
      int16_t y = touchY[0];
      lastTouchMs = millis();

      if (!touchActive) {          // RISING EDGE — a fresh press, act exactly once
        touchActive = true;
        if (inside(tapBtn, x, y)) {
          counter++;
          drawCounter();
          drawButton(tapBtn, true);      // highlight while held
          activeBtn = &tapBtn;
        } else if (inside(resetBtn, x, y)) {
          counter = 0;
          drawCounter();
          drawButton(resetBtn, true);
          activeBtn = &resetBtn;
        }
      }
    } else {
      // The IRQ fired but there are no points: a "touch-up" event. Release now,
      // so the next tap registers immediately instead of waiting out the timeout.
      releaseTouch();
    }
  }

  delay(2);
}
