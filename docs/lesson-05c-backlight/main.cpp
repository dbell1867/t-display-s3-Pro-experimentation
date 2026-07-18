// ============================================================================
//  Stage 5c (step 3): Auto-brightness + screen-off-when-idle (the power feature).
//
//  Ties it together: the LTR-553 ambient-light reading drives the PWM backlight
//  (dim in the dark, bright in light, smoothed + floored), and after BL_OFF_MS
//  with no touch the screen turns OFF entirely — the biggest power saving on this
//  board. Touch, OR a hand near the proximity sensor, restores it.
//
//  Refinement: we light-sleep only once the screen is OFF. A PWM backlight relies
//  on a peripheral clock that can gate during light sleep (glitching the
//  brightness), so we only sleep when duty is 0 (steady low) — which is also when
//  the big win lands: screen dark AND CPU collapsed to ~1 loop/sec together.
//
//  --- Stage 5b (step 3): Light sleep + wake on touch — the low-power payoff. ---
//
//  "Design C", deferred since Lesson 02: instead of busy-polling ~178x/sec, the
//  CPU LIGHT-SLEEPS when idle and wakes on the touch IRQ. Watch the on-screen
//  "CPU: /s" counter collapse when idle, and spring back on touch.
//
//  Design choices (each for a reason):
//   * Wake sources = the touch line (GPIO 21, LEVEL-LOW) + a 1 s TIMER (so the
//     battery gauge keeps refreshing). Light-sleep uses a LEVEL wake, a
//     different mechanism than Step 2's edge attachInterrupt — so we don't use
//     attachInterrupt here (it would fight over the pin's interrupt type). Step 2
//     proved the line responds to touch; that's what lets us trust it as a wake.
//   * Sleep ONLY ON BATTERY (gated on !VBUS). (a) avoids native-USB
//     re-enumeration/flashing pain while plugged in, and (b) mirrors real devices
//     (aggressive sleep on battery only). Live demo: unplug USB -> CPU/s craters;
//     replug -> back to ~178.
//   * A real wake source is never perfectly quiet (the CST226SE emits periodic
//     IRQ pulses), so we design defensively: wake, and if it wasn't a real touch,
//     just sleep again. "Wakes: /s" shows this residual wake rate.
//
//  See docs/lesson-05b-*.md (to be written) for the write-up.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorLTR553.hpp>       // LTR-553 ambient-light + proximity (SensorLib)
#include <XPowersLib.h>
#include "esp_sleep.h"            // esp_light_sleep_start, wake-source config
#include "driver/gpio.h"         // gpio_wakeup_enable

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

// Sleep / backlight tuning.
#define SLEEP_TIMER_US  1000000ULL   // wake every 1 s (refresh gauge) while asleep
#define BL_OFF_MS       5000         // screen off after this long with no activity
#define BL_MIN_DUTY     24           // backlight floor while active (~10%, readable)
#define BL_MAX_DUTY     255          // full brightness
#define LIGHT_DARK      1            // LTR-553 CH0 count mapped to BL_MIN_DUTY
#define LIGHT_BRIGHT    40           // ...and this count mapped to BL_MAX_DUTY
#define PROX_WAKE       30           // proximity above this counts as activity
                                     // (LTR-553 PS is short-range: ~0 until a hand
                                     // is close/over it, then hundreds)

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *panel = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

TouchDrvCSTXXX touch;
PowersSY6970   PPM;
SensorLTR553   als;                 // light + proximity sensor
bool           pmuOk = false;
bool           alsOk = false;

// ---- LVGL draw buffer (aligned — see Stage 4 alignment bug) ----
#define LVBUF_LINES 48
static uint8_t drawBuf[SCREEN_W * LVBUF_LINES * (LV_COLOR_DEPTH / 8)]
    __attribute__((aligned(64)));

static int16_t lastTouchX = 0, lastTouchY = 0;

// Widgets.
static lv_obj_t *pctLabel    = NULL;
static lv_obj_t *bar         = NULL;
static lv_obj_t *voltLabel   = NULL;
static lv_obj_t *statusLabel = NULL;
static lv_obj_t *detailLabel = NULL;
static lv_obj_t *cpuLabel    = NULL;   // loops/sec  (collapses when sleeping)
static lv_obj_t *wakeLabel   = NULL;   // wakes/sec  (residual wake rate)
static lv_obj_t *lightLabel  = NULL;   // LTR-553 light + proximity readout

// Instruments + power-state, sampled once a second.
static uint32_t loopCount      = 0;
static uint32_t wakeCount      = 0;
static uint32_t idleCpu        = 0;    // loops/sec captured while idle (screen off)
static uint32_t lastActivityMs = 0;    // millis() of the last real touch
static bool     onUsbCached    = true; // updated in updateGauge(); gates sleeping

static uint32_t lv_tick_cb(void) { return millis(); }

// Set backlight brightness 0 (off) .. 255 (full) via the LEDC PWM channel.
static void setBacklight(uint8_t duty) { ledcWrite(TFT_BL, duty); }

// Auto-brightness level (smoothed from ambient light); applied by applyBacklight.
static uint8_t autoBrightness = 180;

// Drive the backlight: fully off when the screen has timed out, otherwise the
// auto-brightness level. Called every loop (a cheap register write).
static void applyBacklight(bool idle) {
  setBacklight(idle ? 0 : autoBrightness);
}

// ---- Glue callbacks ----
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  panel->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}
static void my_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  int16_t x[5], y[5];
  uint8_t count = touch.getPoint(x, y, 1);
  if (count > 0) {
    lastTouchX = x[0]; lastTouchY = y[0];
    data->state = LV_INDEV_STATE_PRESSED;
    lastActivityMs = millis();          // a real touch: postpone sleeping
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lastTouchX;
  data->point.y = lastTouchY;
}

// ---- Approximate LiPo state-of-charge from voltage (Stage 5a) ----
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
static uint32_t levelColour(int pct) {
  if (pct >= 50) return 0x30E060;
  if (pct >= 20) return 0xE0C020;
  return 0xE04040;
}

static void updateGauge(void) {
  if (!pmuOk) { lv_label_set_text(statusLabel, "PMU init FAILED"); return; }
  uint16_t vbat = PPM.getBattVoltage();
  bool     vin  = PPM.isVbusIn();
  bool     chg  = PPM.isCharging();
  int      pct  = battPercent(vbat);
  uint32_t col  = levelColour(pct);

  onUsbCached = vin;                     // <-- gate for sleeping (read in loop)

  lv_bar_set_value(bar, pct, LV_ANIM_ON);
  lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
  lv_label_set_text_fmt(pctLabel, "%d%%", pct);
  lv_obj_set_style_text_color(pctLabel, lv_color_hex(col), LV_PART_MAIN);
  lv_label_set_text_fmt(voltLabel, "%u.%03u V", vbat / 1000, vbat % 1000);

  const char *st = !vin ? "On battery" : (chg ? "Charging" : "Charged");
  lv_label_set_text(statusLabel, st);
  lv_label_set_text_fmt(detailLabel, "VBUS %u mV   I %u mA",
                        PPM.getVbusVoltage(), PPM.getChargeCurrent());

  if (alsOk) {
    int light = als.getLightSensor(0);
    int prox  = als.getProximity();
    // Map ambient light -> target duty, then smooth (exponential moving average)
    // so brightness eases rather than jumps between readings.
    int t      = constrain(light, LIGHT_DARK, LIGHT_BRIGHT);
    int target = map(t, LIGHT_DARK, LIGHT_BRIGHT, BL_MIN_DUTY, BL_MAX_DUTY);
    autoBrightness = (uint8_t)((autoBrightness * 3 + target) / 4);
    if (prox > PROX_WAKE) lastActivityMs = millis();   // hand near = activity
    lv_label_set_text_fmt(lightLabel, "Light:%d Prox:%d BL:%d",
                          light, prox, autoBrightness);
  } else {
    lv_label_set_text(lightLabel, "LTR-553: FAIL");
  }
}

static void build_ui(void) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Battery");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  pctLabel = lv_label_create(scr);
  lv_label_set_text(pctLabel, "--%");
  lv_obj_set_style_text_font(pctLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(pctLabel, LV_ALIGN_TOP_MID, 0, 76);

  bar = lv_bar_create(scr);
  lv_obj_set_size(bar, 180, 26);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 122);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x303040), LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);

  voltLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(voltLabel, lv_color_hex(0xC0C0C0), LV_PART_MAIN);
  lv_obj_align(voltLabel, LV_ALIGN_TOP_MID, 0, 160);
  lv_label_set_text(voltLabel, "-.--- V");

  statusLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 188);
  lv_label_set_text(statusLabel, "...");

  detailLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(detailLabel, lv_color_hex(0x808088), LV_PART_MAIN);
  lv_obj_align(detailLabel, LV_ALIGN_TOP_MID, 0, 214);
  lv_label_set_text(detailLabel, "");

  // Low-power instruments.
  cpuLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(cpuLabel, lv_color_hex(0x40A0FF), LV_PART_MAIN);
  lv_obj_set_style_text_font(cpuLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(cpuLabel, LV_ALIGN_TOP_MID, 0, 270);
  lv_label_set_text(cpuLabel, "CPU: -- /s");

  wakeLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(wakeLabel, lv_color_hex(0x40A0FF), LV_PART_MAIN);
  lv_obj_align(wakeLabel, LV_ALIGN_TOP_MID, 0, 310);
  lv_label_set_text(wakeLabel, "Wakes: -- /s");

  // LTR-553 light + proximity readout (Stage 5c step 2).
  lightLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(lightLabel, lv_color_hex(0xE0A040), LV_PART_MAIN);
  lv_obj_align(lightLabel, LV_ALIGN_TOP_MID, 0, 350);
  lv_label_set_text(lightLabel, "Light: --");
}

// ---- Enter light sleep, waking on the touch line OR the 1 s timer -----------
static void enterLightSleep(void) {
  esp_sleep_enable_timer_wakeup(SLEEP_TIMER_US);
  gpio_wakeup_enable((gpio_num_t)TOUCH_IRQ, GPIO_INTR_LOW_LEVEL);  // touch pulses LOW
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start();          // ---- CPU halts here until a wake event ----

  gpio_wakeup_disable((gpio_num_t)TOUCH_IRQ);
  wakeCount++;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    lastActivityMs = millis();      // woken by touch: stay up to service it
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);

  // Backlight via PWM (LEDC): 8-bit duty (0-255) at 5 kHz, so we can DIM it.
  // Auto-brightness in loop() takes over once the light sensor reads.
  ledcAttach(TFT_BL, 5000, 8);
  setBacklight(autoBrightness);
  panel->begin();

  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  // Pull-up so the IRQ line idles firmly HIGH (only a real driven-LOW pulse
  // triggers the level wake — see Step 2's phantom-edge finding).
  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  pmuOk = PPM.init(Wire, I2C_SDA, I2C_SCL, SY6970_SLAVE_ADDRESS);
  if (pmuOk) { PPM.enableMeasure(); if (Serial) Serial.println("SY6970 online."); }
  else if (Serial) Serial.println("ERROR: SY6970 init failed!");

  // LTR-553 light + proximity sensor (same shared bus, addr 0x23).
  alsOk = als.begin(Wire, LTR553_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (alsOk) {
    als.setLightSensorGain(SensorLTR553::ALS_GAIN_1X);  // 1..64k lux range
    als.enableLightSensor();
    als.enableProximity();
    if (Serial) Serial.println("LTR-553 online.");
  } else if (Serial) Serial.println("ERROR: LTR-553 init failed!");

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
  lastActivityMs = millis();
}

void loop() {
  loopCount++;
  lv_timer_handler();

  static uint32_t lastUpd = 0;
  if (millis() - lastUpd >= 1000) {
    updateGauge();
    // If this second elapsed while idle (screen off), it's the sleeping rate —
    // stash it so we can show it once the screen wakes (the counter is otherwise
    // invisible exactly when it matters).
    if ((millis() - lastActivityMs) > BL_OFF_MS) idleCpu = loopCount;
    lv_label_set_text_fmt(cpuLabel,  "CPU:%u/s idle:%u", loopCount, idleCpu);
    lv_label_set_text_fmt(wakeLabel, "Wakes: %u /s", wakeCount);
    loopCount = 0;
    wakeCount = 0;
    lastUpd = millis();
  }

  // Backlight + sleep policy. After BL_OFF_MS with no activity the screen turns
  // off; only THEN do we light-sleep (duty 0 = steady low, so the PWM never
  // glitches from the sleep clock gating). Touch / proximity restore both.
  bool screenIdle = (millis() - lastActivityMs) > BL_OFF_MS;
  applyBacklight(screenIdle);
  if (!onUsbCached && screenIdle && lv_anim_count_running() == 0) {
    enterLightSleep();
  } else {
    delay(5);
  }
}
