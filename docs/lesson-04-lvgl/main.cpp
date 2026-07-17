// ============================================================================
//  Stage 4 (step 4): A real widget with an event — an LVGL Tap Counter.
//
//  This is the payoff of the whole stage. Back in Stage 3 we hand-wrote ~40
//  lines of edge detection (debounced press/release, the IRQ-pulse trap, the
//  touch-up event) just to get ONE clean increment per tap. Here we attach a
//  CLICKED event to an LVGL button and the toolkit fires it exactly once per
//  tap — debouncing, hit-testing, and press/release all handled internally.
//
//  The two glue callbacks from steps 2 & 3 are all the hardware wiring LVGL
//  needs:
//    - my_flush_cb : push LVGL's dirty rectangles to the ST7796 (Arduino_GFX)
//    - my_touch_read_cb : report CST226SE press state + position to LVGL
//
//  Everything else is pure LVGL: create objects, style them, attach events.
//  Because LVGL only repaints what changed, tapping repaints ~a button, not
//  the whole screen — sidestepping the full-frame flush ceiling of Stage 3b.
//
//  See docs/lesson-04-*.md for the write-up.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>

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

// The physical panel.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *panel = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

// The touch controller.
TouchDrvCSTXXX touch;

// ---- LVGL draw buffer ------------------------------------------------------
// Partial (dirty-rectangle) rendering only needs a slice of the screen (LVGL
// recommends >= 1/10). 48 lines * 222 px * 2 bytes = ~21 KB. It MUST be aligned
// (LVGL asserts on this — see the alignment Heisenbug we hit in step 3): a plain
// array is only 1-byte-aligned, so we ask for 64.
#define LVBUF_LINES 48
static uint8_t drawBuf[SCREEN_W * LVBUF_LINES * (LV_COLOR_DEPTH / 8)]
    __attribute__((aligned(64)));

// Held so the read callback can report the last point on release (LVGL uses the
// release position to decide which widget was clicked).
static int16_t lastTouchX = 0, lastTouchY = 0;

// App state.
static int       counter    = 0;
static lv_obj_t *countLabel = NULL;

static uint32_t lv_tick_cb(void) { return millis(); }

// ---- Glue callback 1: flush (LVGL pixels -> panel) ----
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  panel->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}

// ---- Glue callback 2: read (CST226SE touch -> LVGL) ----
static void my_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  int16_t x[5], y[5];
  uint8_t count = touch.getPoint(x, y, 1);
  if (count > 0) {
    lastTouchX = x[0];
    lastTouchY = y[0];
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lastTouchX;
  data->point.y = lastTouchY;
}

// ---- Event handlers --------------------------------------------------------
// LVGL calls these ONCE per completed tap on the widget. No debouncing, no
// edge detection on our side — contrast Stage 3's hand-rolled version.
static void inc_event_cb(lv_event_t *e) {
  counter++;
  lv_label_set_text_fmt(countLabel, "%d", counter);
}
static void reset_event_cb(lv_event_t *e) {
  counter = 0;
  lv_label_set_text_fmt(countLabel, "%d", counter);
}

// ---- The UI ----------------------------------------------------------------
static void build_ui(void) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LVGL Counter");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  // The big live count.
  countLabel = lv_label_create(scr);
  lv_label_set_text(countLabel, "0");
  lv_obj_set_style_text_color(countLabel, lv_color_hex(0xFFD400), LV_PART_MAIN);
  lv_obj_set_style_text_font(countLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(countLabel, LV_ALIGN_TOP_MID, 0, 90);

  // "Tap +1" button -> increment.
  lv_obj_t *incBtn = lv_button_create(scr);
  lv_obj_set_size(incBtn, 170, 90);
  lv_obj_align(incBtn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(incBtn, inc_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *incLbl = lv_label_create(incBtn);
  lv_label_set_text(incLbl, "Tap +1");
  lv_obj_center(incLbl);

  // "Reset" button -> zero.
  lv_obj_t *resetBtn = lv_button_create(scr);
  lv_obj_set_size(resetBtn, 170, 70);
  lv_obj_align(resetBtn, LV_ALIGN_CENTER, 0, 110);
  lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0x803030), LV_PART_MAIN);
  lv_obj_add_event_cb(resetBtn, reset_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *resetLbl = lv_label_create(resetBtn);
  lv_label_set_text(resetLbl, "Reset");
  lv_obj_center(resetLbl);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  panel->begin();

  // Touch controller (identity orientation calibrated in Stage 2b).
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, TOUCH_SDA, TOUCH_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  // LVGL: init, display (flush), input (read).
  lv_init();
  lv_tick_set_cb(lv_tick_cb);

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(disp, my_flush_cb);
  lv_display_set_buffers(disp, drawBuf, NULL, sizeof(drawBuf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read_cb);

  build_ui();

  if (Serial) Serial.println("Stage 4 step 4: LVGL Tap Counter ready.");
}

void loop() {
  lv_timer_handler();   // renders dirty areas + services input
  delay(5);
}
