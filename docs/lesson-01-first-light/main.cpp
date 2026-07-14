// ============================================================================
//  Stage 2a: "First light" — drive the ST7796 display with Arduino_GFX
//
//  Goal: prove the screen, backlight, and pins all work by filling the
//  display with color bars and drawing text.
// ============================================================================

#include <Arduino.h>
#include <Arduino_GFX_Library.h>   // the graphics library we added to lib_deps

// ---- Pin map for the T-Display S3 Pro (from LilyGO's utilities.h) ----
#define TFT_SCLK 18   // SPI clock
#define TFT_MOSI 17   // data out to display
#define TFT_MISO  8   // data in from display (shared SPI bus)
#define TFT_CS   39   // chip-select
#define TFT_DC    9   // data/command select
#define TFT_RST  47   // reset
#define TFT_BL   48   // backlight

// The display is reached in two layers:
//
//  1) a "data bus" object that knows HOW to shove bytes over SPI, and
//  2) a "driver" object that knows the ST7796's command language and geometry.
//
// The `new` keyword allocates the object and hands back a POINTER to it
// (the `*`). Coming from Python: every object is really a reference; here C++
// just makes that reference explicit. `bus->begin()` / `gfx->fillScreen()`
// use `->` to call a method through a pointer (Python would just use `.`).

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

// Args: bus, reset pin, rotation, IPS panel?, width, height, col-offset, row-offset.
// The 222 width + 49 column offset are this panel's specific quirk.
Arduino_GFX *gfx = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    222 /*width*/, 480 /*height*/, 49 /*col offset*/, 0 /*row offset*/);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Stage 2a: bringing up the display...");

  // Turn the backlight ON. Without this the panel stays dark even if drawing
  // succeeds — a classic "why is my screen black" gotcha.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Initialize the display hardware. begin() returns false if it can't talk
  // to the panel — we report that over serial so failures aren't silent.
  if (!gfx->begin()) {
    Serial.println("ERROR: gfx->begin() failed!");
  }

  gfx->fillScreen(RGB565_BLACK);

  // Four color bars top-to-bottom. This proves the coordinate system and that
  // red/green/blue are wired the way we expect. Screen is 222 wide x 480 tall;
  // each bar is 120 px tall (4 * 120 = 480).
  // (Arduino_GFX 1.6 prefixes color names with RGB565_ = 16-bit 5-6-5 color.)
  gfx->fillRect(0,   0, 222, 120, RGB565_RED);
  gfx->fillRect(0, 120, 222, 120, RGB565_GREEN);
  gfx->fillRect(0, 240, 222, 120, RGB565_BLUE);
  gfx->fillRect(0, 360, 222, 120, RGB565_WHITE);

  // Some text on top. Passing ONE color (no background) draws only the glyph
  // pixels, so the bar color shows through around the letters ("transparent").
  // Passing a second color would fill a solid box behind the text instead.
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(12, 20);
  gfx->println("Hello!");

  gfx->setTextSize(2);
  gfx->setCursor(12, 60);
  gfx->println("T-Display");
  gfx->setCursor(12, 82);
  gfx->println("S3 Pro");

  Serial.println("Display setup done.");
}

void loop() {
  // Nothing animated yet — the static image stays on screen.
  delay(1000);
}
