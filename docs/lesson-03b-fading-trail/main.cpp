// ============================================================================
//  Stage 3b: Retained-mode framebuffer — a fading touch trail
//
//  The delightful effect we deferred back in Lesson 02: draw with your finger
//  and the dots fade + shrink away behind it, like a comet's tail.
//
//  The key change is RETAINED MODE. Until now we drew straight to the panel
//  (immediate mode), so "fade everything a bit each frame" was impractical. Now
//  an Arduino_Canvas holds a full-screen framebuffer in PSRAM; we redraw the
//  ENTIRE scene into it every frame — clear, then repaint every live trail point
//  dimmer/smaller by its age — and flush() blits the whole buffer to the panel
//  in one shot. Flicker-free, and whole-scene redraws are suddenly cheap.
//
//  Why PSRAM: the framebuffer is 222*480*2 = ~213 KB, far too big for internal
//  RAM. platformio.ini now enables the board's 8 MB octal PSRAM (BOARD_HAS_PSRAM
//  + memory_type = qio_opi); Arduino_Canvas::begin() allocates the buffer there.
//
//  This is also exactly how LVGL works under the hood — the bridge to Stage 4.
//  See docs/lesson-03b-*.md for the write-up.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>                   // I2C bus library (SDA + SCL, shared by chips)
#include <Arduino_GFX_Library.h>    // display driver + Arduino_Canvas
#include <TouchDrvCSTXXX.hpp>       // SensorLib's touch driver; auto-detects CST226SE

// ---- Display pins (SPI) ----
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

// ---- The physical panel, and the framebuffer that draws to it --------------
// `panel` is the real ST7796 (with its 49-px column offset). `gfx` is now the
// Canvas: we draw into its PSRAM buffer, then gfx->flush() pushes it to `panel`.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *panel = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

Arduino_Canvas *gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);

// ---- Touch object ----
TouchDrvCSTXXX touch;
int16_t touchX[5];
int16_t touchY[5];

// ---- The fading trail: a ring buffer of recent touch points ----------------
// Each point remembers WHEN it was placed; its age drives how bright/big it is,
// and points older than TRAIL_TTL_MS are gone. At ~40 touch frames/sec over a
// 1.2 s life that's ≤ ~50 live points, so 160 slots is plenty of headroom.
#define TRAIL_MAX     512
#define TRAIL_TTL_MS  1500
#define DOT_RADIUS    4       // constant brush size — fade COLOUR, not size (avoids
                              // the visible "size popping" of an integer-stepped radius)
struct TrailPoint {
  int16_t x, y;
  uint32_t t;          // millis() when placed (0 = empty slot)
};
TrailPoint trail[TRAIL_MAX];
uint16_t trailHead = 0;       // next slot to overwrite (oldest)
uint16_t prevLive  = 0;       // live-point count last frame (for idle skipping)

// Stroke interpolation (the Stage 2c trick): fill in points BETWEEN sparse touch
// samples, so a fast swipe is a continuous ribbon rather than scattered dots —
// independent of how slow the framebuffer flush makes our sample rate.
#define STROKE_GAP_MS  250    // longer than this since the last sample ⇒ new stroke
#define STROKE_STEP_PX 8      // drop an interpolated point roughly every 8 px
int16_t  lastX = 0, lastY = 0;
uint32_t lastAddMs = 0;
bool     haveLast = false;

#define TITLE_GREY 0x630C     // a dim grey (RGB565) for the hint text

// Add a single point at the current time.
void addPoint(int16_t x, int16_t y) {
  trail[trailHead].x = x;
  trail[trailHead].y = y;
  uint32_t now = millis();
  trail[trailHead].t = (now == 0) ? 1 : now;   // never store 0 (means "empty")
  trailHead = (trailHead + 1) % TRAIL_MAX;
}

// Add a touch sample, interpolating from the previous one within the same stroke
// so gaps are filled. A pause longer than STROKE_GAP_MS (or a fresh touch) starts
// a new stroke, so we never draw a ribbon across the screen from the last one.
void addStroke(int16_t x, int16_t y) {
  uint32_t now = millis();
  bool cont = haveLast && (now - lastAddMs) < STROKE_GAP_MS;
  if (cont) {
    int16_t dx = x - lastX, dy = y - lastY;
    int16_t span = max(abs(dx), abs(dy));
    int16_t steps = span / STROKE_STEP_PX;
    if (steps < 1)  steps = 1;
    if (steps > 40) steps = 40;                // cap work on a huge jump
    for (int16_t i = 1; i <= steps; i++) {
      addPoint(lastX + (int32_t)dx * i / steps,
               lastY + (int32_t)dy * i / steps);
    }
  } else {
    addPoint(x, y);                            // first point of a fresh stroke
  }
  lastX = x; lastY = y; lastAddMs = now; haveLast = true;
}

// Yellow scaled toward black by k (1 = full brightness, 0 = black). RGB565 packs
// 5 bits red, 6 bits green, 5 bits blue; yellow is red+green, so we dim both.
uint16_t fadedYellow(float k) {
  if (k < 0) k = 0;
  uint8_t r = (uint8_t)(31 * k);
  uint8_t g = (uint8_t)(63 * k);
  return (uint16_t)((r << 11) | (g << 5));
}

// Repaint the whole scene into the framebuffer. Returns how many points are
// still alive (so the loop can skip flushing once the trail has fully faded).
uint16_t buildScene() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(TITLE_GREY);
  gfx->setTextSize(2);
  gfx->setCursor(10, 10);
  gfx->print("Draw with your finger");

  uint32_t now = millis();
  uint16_t live = 0;
  for (int i = 0; i < TRAIL_MAX; i++) {
    if (trail[i].t == 0) continue;                 // empty slot
    uint32_t age = now - trail[i].t;
    if (age >= TRAIL_TTL_MS) continue;             // expired

    float k = 1.0f - (float)age / TRAIL_TTL_MS;    // 1 at birth → 0 at death
    gfx->fillCircle(trail[i].x, trail[i].y, DOT_RADIUS, fadedYellow(k));
    live++;
  }
  return live;
}

// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // native-USB: non-blocking writes (see lesson 02)
  delay(500);
  if (Serial) Serial.println("Stage 3b: framebuffer fading trail...");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Canvas begin() begins the panel AND allocates the 213 KB framebuffer in
  // PSRAM. If PSRAM weren't enabled the alloc would fail and begin() return false.
  if (!gfx->begin(80000000)) {   // 80 MHz SPI: halves the full-frame flush time
    if (Serial) Serial.println("ERROR: canvas begin failed (framebuffer alloc?)");
    // Fall back to the raw panel to show the error, since the canvas is unusable.
    panel->begin();
    panel->fillScreen(RGB565_RED);
    panel->setTextColor(RGB565_WHITE);
    panel->setTextSize(2);
    panel->setCursor(10, 40);
    panel->print("Framebuffer alloc failed");
    while (true) delay(1000);
  }

  // Touch
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, TOUCH_SDA, TOUCH_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  } else {
    if (Serial) Serial.println("Touch controller online.");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  buildScene();
  gfx->flush();               // show the initial (empty) scene
  if (Serial) Serial.println("Setup done. Draw!");
}

void loop() {
  // Read a touch point (design B: only over I2C when the IRQ says so) and add it.
  // Poll the touch point directly every frame (NOT gated on isPressed): the
  // blocking flush leaves us only ~20 checks/sec, too coarse to catch the
  // flickering IRQ reliably. A direct getPoint() returns the current point when
  // a finger is down and count 0 when it lifts — giving one solid sample per
  // frame and a clean stroke break.
  uint8_t count = touch.getPoint(touchX, touchY, 5);
  if (count > 0) {
    addStroke(touchX[0], touchY[0]);
  } else {
    haveLast = false;          // finger up → the next touch starts a fresh stroke
  }

  // Redraw the whole scene, then flush — but only while something is animating
  // (any live points now, or there were some last frame so we clear once more).
  uint16_t live = buildScene();
  if (live > 0 || prevLive > 0) {
    gfx->flush();
  }
  prevLive = live;

  delay(1);
}
