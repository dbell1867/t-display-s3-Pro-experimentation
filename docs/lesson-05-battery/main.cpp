// ============================================================================
//  Stage 5 (step 2): An LVGL battery gauge.
//
//  Turns the raw SY6970 readout from step 1 into a proper instrument: a
//  percentage, a colour-coded bar (green -> amber -> red), the pack voltage,
//  and a clean charge status. This is the gauge we'll watch during the
//  low-power lessons to see consumption drop.
//
//  Honest caveat baked in: the SY6970 has no fuel gauge, so "%" is DERIVED FROM
//  VOLTAGE via a rough LiPo curve. Voltage is not true state-of-charge and reads
//  optimistically WHILE CHARGING (charge current props the voltage up). It's
//  most accurate on battery under light load — exactly the low-power condition.
//
//  See docs/lesson-05-*.md (to be written) for the write-up.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>

// ---- Display pins (SPI) ----
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_MISO  8
#define TFT_CS   39
#define TFT_DC    9
#define TFT_RST  47
#define TFT_BL   48

// ---- Touch + PMU share this I2C bus ----
#define I2C_SDA    5
#define I2C_SCL    6
#define TOUCH_RST 13
#define TOUCH_IRQ 21

#define SCREEN_W 222
#define SCREEN_H 480

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *panel = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

TouchDrvCSTXXX touch;
PowersSY6970   PPM;
bool           pmuOk = false;

// ---- LVGL draw buffer (aligned — see Stage 4 alignment bug) ----
#define LVBUF_LINES 48
static uint8_t drawBuf[SCREEN_W * LVBUF_LINES * (LV_COLOR_DEPTH / 8)]
    __attribute__((aligned(64)));

static int16_t lastTouchX = 0, lastTouchY = 0;

// Widgets we update each refresh.
static lv_obj_t *pctLabel    = NULL;
static lv_obj_t *bar         = NULL;
static lv_obj_t *voltLabel   = NULL;
static lv_obj_t *statusLabel = NULL;
static lv_obj_t *detailLabel = NULL;

static uint32_t lv_tick_cb(void) { return millis(); }

// ---- Glue callbacks (unchanged) ----
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  panel->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}
static void my_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  int16_t x[5], y[5];
  uint8_t count = touch.getPoint(x, y, 1);
  if (count > 0) { lastTouchX = x[0]; lastTouchY = y[0]; data->state = LV_INDEV_STATE_PRESSED; }
  else           { data->state = LV_INDEV_STATE_RELEASED; }
  data->point.x = lastTouchX;
  data->point.y = lastTouchY;
}

// ---- Approximate LiPo state-of-charge from resting voltage ------------------
// A single-cell LiPo's discharge curve is NON-linear (flat in the middle), so a
// plain (v-3300)/(4200-3300) map is poor. This small table + linear
// interpolation between points is a decent cheap approximation. Still just an
// estimate — see the caveat at the top of the file.
static int battPercent(uint16_t mv) {
  static const struct { uint16_t mv; uint8_t pct; } pts[] = {
    {3300, 0}, {3600, 10}, {3700, 25}, {3750, 40}, {3800, 55},
    {3850, 68}, {3900, 78}, {4000, 90}, {4100, 97}, {4200, 100},
  };
  const int n = sizeof(pts) / sizeof(pts[0]);
  if (mv <= pts[0].mv)     return 0;
  if (mv >= pts[n - 1].mv) return 100;
  for (int i = 1; i < n; i++) {
    if (mv < pts[i].mv) {
      int v0 = pts[i - 1].mv, v1 = pts[i].mv;
      int p0 = pts[i - 1].pct, p1 = pts[i].pct;
      return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
    }
  }
  return 100;
}

// A colour for the level: green >= 50%, amber >= 20%, else red.
static uint32_t levelColour(int pct) {
  if (pct >= 50) return 0x30E060;
  if (pct >= 20) return 0xE0C020;
  return 0xE04040;
}

static void updateGauge(void) {
  if (!pmuOk) {
    lv_label_set_text(statusLabel, "PMU init FAILED");
    return;
  }
  uint16_t vbat = PPM.getBattVoltage();       // mV
  bool     vin  = PPM.isVbusIn();
  bool     chg  = PPM.isCharging();
  int      pct  = battPercent(vbat);
  uint32_t col  = levelColour(pct);

  lv_bar_set_value(bar, pct, LV_ANIM_ON);
  lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);

  lv_label_set_text_fmt(pctLabel, "%d%%", pct);
  lv_obj_set_style_text_color(pctLabel, lv_color_hex(col), LV_PART_MAIN);

  lv_label_set_text_fmt(voltLabel, "%u.%03u V", vbat / 1000, vbat % 1000);

  const char *st = !vin ? "On battery" : (chg ? "Charging" : "Charged");
  lv_label_set_text(statusLabel, st);

  lv_label_set_text_fmt(detailLabel, "VBUS %u mV   I %u mA",
                        PPM.getVbusVoltage(), PPM.getChargeCurrent());
}

static void build_ui(void) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Battery");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  // Big percentage.
  pctLabel = lv_label_create(scr);
  lv_label_set_text(pctLabel, "--%");
  lv_obj_set_style_text_font(pctLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(pctLabel, LV_ALIGN_TOP_MID, 0, 80);

  // The gauge bar.
  bar = lv_bar_create(scr);
  lv_obj_set_size(bar, 180, 26);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 130);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x303040), LV_PART_MAIN);      // track
  lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);

  // Voltage.
  voltLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(voltLabel, lv_color_hex(0xC0C0C0), LV_PART_MAIN);
  lv_obj_align(voltLabel, LV_ALIGN_TOP_MID, 0, 175);
  lv_label_set_text(voltLabel, "-.--- V");

  // Charge status.
  statusLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 205);
  lv_label_set_text(statusLabel, "...");

  // VBUS / current detail.
  detailLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(detailLabel, lv_color_hex(0x808088), LV_PART_MAIN);
  lv_obj_align(detailLabel, LV_ALIGN_TOP_MID, 0, 235);
  lv_label_set_text(detailLabel, "");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  panel->begin();

  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  pmuOk = PPM.init(Wire, I2C_SDA, I2C_SCL, SY6970_SLAVE_ADDRESS);
  if (pmuOk) { PPM.enableMeasure(); if (Serial) Serial.println("SY6970 online."); }
  else if (Serial) Serial.println("ERROR: SY6970 init failed!");

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
  updateGauge();
}

void loop() {
  lv_timer_handler();
  static uint32_t lastUpd = 0;
  if (millis() - lastUpd >= 1000) {
    updateGauge();
    lastUpd = millis();
  }
  delay(5);
}
