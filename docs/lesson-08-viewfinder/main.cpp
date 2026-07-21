// ============================================================================
//  Stage 5d: Deep sleep — power (almost) everything down, reboot on wake.
//
//  Deep sleep is NOT light sleep. Light sleep (Stage 5b) halts the CPU but keeps
//  RAM + peripherals and RESUMES right after the call. Deep sleep powers the chip
//  down to the RTC domain (~tens of uA): RAM is lost, and on wake the chip REBOOTS
//  — setup() runs from scratch. Only RTC memory (RTC_DATA_ATTR) survives, and every
//  peripheral re-inits in setup(). It suits "off most of the time, wake rarely", so
//  we add it as a deliberate "Deep Sleep" BUTTON rather than replacing the
//  always-on gauge's light-sleep idle behaviour (5b/5c), which stays below.
//
//  Wake sources: the touch line (GPIO 21 — an RTC-capable pad — via EXT1 ANY_LOW;
//  the S3 dropped the classic EXT0) or a 20 s timer. On wake we show a boot counter
//  (proving RTC_DATA_ATTR persists while everything else rebooted) + the wake cause.
//  The backlight is driven low and GPIO-hold'd so it stays off through deep sleep.
//
//  (Stages 5b/5c — light sleep, auto-brightness, screen-off — are unchanged.)
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
#include <SPI.h>                 // SPIClass: the SD card shares the display's bus
#include <SD.h>                  // FAT filesystem on a microSD over SPI
#include "esp_sleep.h"            // esp_light_sleep_start, esp_deep_sleep_start, wakes
#include "driver/gpio.h"         // gpio_wakeup_enable, gpio_hold_en
#include "driver/rtc_io.h"       // rtc_gpio_pullup_en (deep-sleep pin config)
#include "esp_camera.h"          // bundled with the ESP32-S3 core; no lib_deps needed

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
#define BTN_WAKE  16   // physical user button: EXTERNAL pull-up, RTC-capable (Stage 5f)
#define SD_CS     14   // microSD chip-select — SHARES SCLK/MOSI/MISO with the display

// ---- Camera shield, DVP (Stage 7 probe) ----
// Pins from LilyGO's CameraShield example utilities.h. Only the three we need to
// WAKE the sensor are declared here; the 8 data lines matter once we capture.
// Note SIOD/SIOC are I2C_SDA/I2C_SCL — the sensor is just another device on the
// bus we already share with touch, the PMU and the ALS.
#define CAM_XCLK    11          // master clock we must generate FOR the sensor
#define CAM_PWDN    46          // active-HIGH power-down; LOW = sensor running
#define CAM_XCLK_HZ 20000000
#define CAM_RESET   -1          // not broken out on this shield: sensor self-resets
#define CAM_PCLK     2
#define CAM_VSYNC    7
#define CAM_HREF    15
// 8-bit parallel data bus. Note D5/D4/D3 land on GPIO 42/40/41 — the JTAG pins —
// and D0 on GPIO 45, a strapping pin. Fine at runtime, but it means a fitted
// camera is electrically present on pins the chip cares about during boot.
#define CAM_D7       4
#define CAM_D6      10
#define CAM_D5       3
#define CAM_D4       1
#define CAM_D3      42
#define CAM_D2      40
#define CAM_D1      41
#define CAM_D0      45

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

// ---- Stage 6: microSD on the display's SPI bus ----------------------------
// One bus, two devices, two chip-selects. This only works because both drivers
// cooperate: Arduino_ESP32SPI is built with is_shared_interface=true, so it calls
// spiTransaction() on every beginWrite() and RELEASES the bus on endWrite(), and
// SPIClass does the same around its transfers. The critical detail is that both
// must be on the SAME SPI HOST — on the S3, Arduino_ESP32SPI defaults to FSPI
// (which the core numbers 0 = the SPI2 bus). Two different hosts matrixed onto the
// same pins would simply fight.
SPIClass       sdSPI(FSPI);
bool           sdOk = false;
static char    sdInfo[40] = "SD none";
static char    camInfo[40] = "CAM ?";

// ---------------------------------------------------------------------------
//  Camera probe (Stage 7, step 1)
//  Goal: find out whether a camera shield is fitted and WHICH sensor it is,
//  without pulling in esp_camera or touching the 8 data lines.
// ---------------------------------------------------------------------------

// NOTE: an earlier hand-rolled camera probe lived here — a bare-metal I2C scan
// that toggled PWDN and generated XCLK with ledcAttach(). It was removed after the
// boot log proved it never worked THREE ways over: (1) ledcAttach(pin, 20 MHz, 2)
// silently FAILED — 20 MHz needs an LEDC divider of ~0, past the peripheral's
// limit, and I never checked the return, so the "clock" never ran; (2) even with a
// clock, SCCB is OmniVision's I2C variant and doesn't ACK the way Wire expects; and
// (3) LEDC is a PWM generator, not a clock source. Every "no camera" it printed was
// the instrument failing, not the hardware. esp_camera_init() (cameraDriverProbe,
// below) does all of this correctly and found the OV5640 first try. The full story
// is in the Stage 7 lesson; the code is gone because a probe that always lies is
// worse than none.

// Bring up (or bring BACK) the three drivers that share the I2C bus. Called once
// in setup(), and again after the camera probe — which tears the bus down and
// leaves it in a state Wire.begin() alone doesn't fully undo. Re-running each
// driver's begin() re-establishes both the bus AND that chip's register config.
// The touch IRQ pad dance stays in setup(): that's one-time pad recovery from
// deep sleep, not bus config.
static void initSharedBusSensors(void) {
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    if (Serial) Serial.println("ERROR: touch.begin() failed!");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  pmuOk = PPM.init(Wire, I2C_SDA, I2C_SCL, SY6970_SLAVE_ADDRESS);
  if (pmuOk) { PPM.enableMeasure(); if (Serial) Serial.println("SY6970 online."); }
  else if (Serial) Serial.println("ERROR: SY6970 init failed!");

  alsOk = als.begin(Wire, LTR553_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (alsOk) {
    als.setLightSensorGain(SensorLTR553::ALS_GAIN_1X);  // 1..64k lux range
    als.enableLightSensor();
    als.enableProximity();
    if (Serial) Serial.println("LTR-553 online.");
  } else if (Serial) Serial.println("ERROR: LTR-553 init failed!");
}

// Fill the pin map + XCLK that EVERY camera_config_t needs, whatever we then do
// with the camera (detect vs stream). Pixel format / frame size / buffers differ
// per use and are left to the caller. `ledc_timer/channel` dodge the backlight's
// LEDC (auto-allocated channel 0) so bringing the camera up can't reprogram the
// timer driving the screen.
static void cameraFillPins(camera_config_t &cfg) {
  cfg.pin_pwdn     = CAM_PWDN;   cfg.pin_reset    = CAM_RESET;
  cfg.pin_xclk     = CAM_XCLK;
  cfg.pin_sccb_sda = I2C_SDA;    cfg.pin_sccb_scl = I2C_SCL;
  cfg.pin_d7 = CAM_D7; cfg.pin_d6 = CAM_D6; cfg.pin_d5 = CAM_D5; cfg.pin_d4 = CAM_D4;
  cfg.pin_d3 = CAM_D3; cfg.pin_d2 = CAM_D2; cfg.pin_d1 = CAM_D1; cfg.pin_d0 = CAM_D0;
  cfg.pin_vsync = CAM_VSYNC; cfg.pin_href = CAM_HREF; cfg.pin_pclk = CAM_PCLK;
  cfg.xclk_freq_hz = CAM_XCLK_HZ;
  cfg.ledc_timer   = LEDC_TIMER_1;
  cfg.ledc_channel = LEDC_CHANNEL_2;
}

// esp_camera borrows I2C pins 5/6 for SCCB on PORT 1; Arduino Wire is PORT 0.
// After esp_camera_deinit() the pins stay muxed to the (now-dead) port 1, and
// Wire.begin() is idempotent — it early-returns and never re-does the pin mux. So
// tear Wire fully DOWN and back up to re-mux pins 5/6 to port 0, then re-establish
// the three sensor drivers' register config on top. Skipping this froze the gauge
// on stale reads (Stage 7). Any code that borrows the camera must call this after.
static void cameraRecoverBus(void) {
  Wire.end();
  delay(10);
  Wire.begin(I2C_SDA, I2C_SCL);
  initSharedBusSensors();
}

// ---------------------------------------------------------------------------
//  Camera probe: ask the REAL driver.
//
//  esp_camera_init() runs the correct power/clock/reset sequence, generates XCLK
//  through the proper clock peripheral (not an abused PWM channel), talks real
//  SCCB, and detects the sensor by PID — returning a SPECIFIC error code rather
//  than a boolean. When a purpose-built instrument exists, prefer it to one you
//  wrote yourself (see the removed hand-rolled probe's post-mortem above).
// ---------------------------------------------------------------------------
static void cameraDriverProbe(void) {
  camera_config_t cfg = {};
  cameraFillPins(cfg);

  // Smallest sensible capture: we only want DETECTION here, not a good picture,
  // so one small buffer keeps the allocation honest if PSRAM is the problem.
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size   = FRAMESIZE_QVGA;
  cfg.fb_count     = 1;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&cfg);

  if (err == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    uint16_t pid = s ? s->id.PID : 0;
    snprintf(camInfo, sizeof(camInfo), " CAM%04X", pid);
    if (Serial) Serial.printf("CAMERA OK: sensor PID 0x%04X\n", pid);
    esp_camera_deinit();
  } else {
    // 0x105 = ESP_ERR_NOT_FOUND: the sequence ran correctly and no sensor
    // answered. Any other code says the driver fell over somewhere earlier,
    // which is a different (and more interesting) failure.
    snprintf(camInfo, sizeof(camInfo), " CAMe%03X", (unsigned)(err & 0xFFF));
    if (Serial) Serial.printf("camera init failed: 0x%X (%s)\n", err, esp_err_to_name(err));
  }

  cameraRecoverBus();
}

// Write a file, read it straight back, and compare. Mounting only proves we can
// talk to the card; this proves the FILESYSTEM works in both directions.
// File is an object with a bool operator — `if (!f)` is the idiomatic check, and
// close() matters: data sits in a buffer until then (Python's `with open(...)`
// does this for you; in C++ you close it yourself).
static bool sdSelfTest(void) {
  File f = SD.open("/selftest.txt", FILE_WRITE);
  if (!f) return false;
  f.println("tdisplay");
  f.close();

  f = SD.open("/selftest.txt", FILE_READ);
  if (!f) return false;
  String back = f.readStringUntil('\n');
  f.close();
  return back.startsWith("tdisplay");
}

// Append one line per boot. FILE_APPEND (not FILE_WRITE, which truncates) is what
// makes this a log rather than a single-entry file — and it survives power loss,
// which is the whole point of storage over RTC memory.
static uint32_t sdLogBoot(uint32_t bootCount, const char *why) {
  File f = SD.open("/boots.csv", FILE_APPEND);
  if (f) { f.printf("%lu,%s\n", (unsigned long)bootCount, why); f.close(); }

  uint32_t lines = 0;                       // count them back = a read-side check
  f = SD.open("/boots.csv", FILE_READ);
  if (f) {
    while (f.available()) { if (f.read() == '\n') lines++; }
    f.close();
  }
  return lines;
}

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

// ---- Stage 5e: power bench -------------------------------------------------
// Normal firmware hides power states behind timers (and refuses to light-sleep on
// USB — exactly when a USB meter can see it). The bench holds ONE state still so
// the meter can settle, then advances on a tap. Each mode announces itself at full
// brightness for BENCH_ANNOUNCE_MS before going dark: the screen is both the
// instrument AND the biggest load, so it has to tell you the mode, then get out of
// the way. Read the meter AFTER it goes dark.
enum BenchMode : uint8_t {
  BENCH_OFF = 0,        // normal firmware behaviour (5b/5c/5d)
  BENCH_BL_FULL,        // A: busy-poll CPU + backlight 100%
  BENCH_BL_OFF,         // B: busy-poll CPU + backlight off  -> cost of the SCREEN
  BENCH_LIGHT_SLEEP,    // C: light sleep    + backlight off  -> cost of the CPU
  BENCH_DISP_SLEEP,     // D: light sleep + ST7796 SLPIN -> cost of the DISPLAY CONTROLLER
  BENCH_ALL_OFF,        // E: D + touch asleep + ALS off  -> cost of the I2C PERIPHERALS
  BENCH_DEEP_SLEEP,     // F: E + deep sleep -> the true FIRMWARE FLOOR (rest is hardware)
  BENCH_COUNT
};
#define BENCH_ANNOUNCE_MS 4000

static uint8_t   benchMode      = BENCH_OFF;
static uint32_t  benchEnteredMs = 0;
static bool      benchTouchPrev = false;
static bool      benchBtnPrevHigh = true;  // GPIO 16 edge detect (advances the bench)
static bool      dispAsleep     = false;   // ST7796 currently in SLPIN (bench mode D)
static bool      periphOff      = false;   // touch + ALS shut down (bench modes E/F)

// Shut down the I2C peripherals. One-way: the CST226SE may need a power cycle to
// come back, which is fine because every mode that calls this ends in a reboot.
static void peripheralsOff(void);
static lv_obj_t *benchLabel     = NULL;

static const char *benchName(uint8_t m) {
  switch (m) {
    case BENCH_BL_FULL:     return "A: poll + BL 100%";
    case BENCH_BL_OFF:      return "B: poll + BL off";
    case BENCH_LIGHT_SLEEP: return "C: light sleep";
    case BENCH_DISP_SLEEP:  return "D: +display off";
    case BENCH_ALL_OFF:     return "E: +touch/ALS off";
    case BENCH_DEEP_SLEEP:  return "F: deep sleep 20s";
    default:                return "";
  }
}

// Instruments + power-state, sampled once a second.
static uint32_t loopCount      = 0;
static uint32_t wakeCount      = 0;
static uint32_t idleCpu        = 0;    // loops/sec captured while idle (screen off)
static uint32_t lastActivityMs = 0;    // millis() of the last real touch
static bool     onUsbCached    = true; // updated in updateGauge(); gates sleeping

// Deep sleep (Stage 5d).
RTC_DATA_ATTR uint32_t bootCount = 0;  // in RTC memory: survives deep sleep, not power-on
static const char *wakeReason  = "power-on";
static bool        wantDeepSleep = false;   // set by the Deep Sleep button
static bool        wantViewfinder = false;  // set by the Cam button (Stage 8)
static lv_obj_t   *bootLabel   = NULL;
static void deepsleep_event_cb(lv_event_t *e);   // defined below build_ui()
static void bench_event_cb(lv_event_t *e);       // ditto (Stage 5e)
static void cam_event_cb(lv_event_t *e);         // ditto (Stage 8)
static void enterDeepSleep(bool touchWake);      // benchStep() calls it before it's defined
static void runViewfinder(void);                 // loop() calls it before it's defined

// ---- Stage 5f: the physical button (GPIO 16) ----
// PROBED before designing around it (Stage 5f step 1, scaffolding since removed):
// idles HIGH, active-low, and — the fact that mattered — it has an EXTERNAL
// pull-up. Verified by driving an internal PULLDOWN and seeing who won: still HIGH
// => external. That's why it beats the touch line as a deep-sleep wake source:
// internal pull-ups live in RTC_PERIPH, a domain deep sleep switches off, so they
// force esp_sleep_pd_config(..., ON) and burn current purely to hold a line high.
// (The board has only TWO buttons, GPIO 0 = BOOT and GPIO 16; GPIO 12 has a
// pull-up but nothing attached, despite the vendor docs listing three.)

static uint32_t lv_tick_cb(void) { return millis(); }

// Set backlight brightness 0 (off) .. 255 (full) via the LEDC PWM channel.
static void setBacklight(uint8_t duty) { ledcWrite(TFT_BL, duty); }

// Auto-brightness level (smoothed from ambient light); applied by applyBacklight.
static uint8_t autoBrightness = 180;

// Screen power: backlight AND the display controller itself.
//
// Stage 5e measured what we'd been missing: turning the backlight off saves 18 mA,
// but the ST7796 keeps scanning a panel nobody can see for another 9 mA on top. The
// backlight is a PWM pin; the controller needs an actual command (SLPIN). Blanking
// one without the other leaves a third of the screen's cost on the table.
//
// Order matters both ways: go dark BEFORE SLPIN so its teardown isn't visible, and
// redraw BEFORE lighting up on the way back (SLPIN discards the frame buffer, so
// without the invalidate+refresh you'd light up on garbage).
static void applyScreenPower(bool idle) {
  if (idle) {
    setBacklight(0);
    if (!dispAsleep) { panel->displayOff(); dispAsleep = true; }
  } else {
    if (dispAsleep) {
      panel->displayOn();               // SLPOUT (+120 ms settle, done by the driver)
      dispAsleep = false;
      // SLPOUT resumes scanning whatever is in the controller's GRAM — which after a
      // sleep cycle is random, and showed as white noise. Wipe it to black while the
      // backlight is still 0, THEN let LVGL repaint, THEN light up. Relying on the
      // redraw to outrun the backlight isn't enough: the garbage has to not exist.
      panel->fillScreen(RGB565_BLACK);
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(lv_display_get_default());
    }
    setBacklight(autoBrightness);
  }
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
  // A BLINKING red charge LED means the SY6970 is in a FAULT/retry loop, not
  // charging normally — and a charger that keeps retrying draws current in bursts
  // the meter reports as a raised average. Ask the chip which fault it is.
  uint8_t fault = PPM.getFaultStatus();
  if (fault) {
    lv_label_set_text_fmt(detailLabel, "FAULT %02X %s%s%s%s%s", fault,
                          PPM.isWatchdogFault() ? "WD "    : "",
                          PPM.isBoostFault()    ? "BOOST " : "",
                          PPM.isChargeFault()   ? "CHG "   : "",
                          PPM.isBatteryFault()  ? "BAT "   : "",
                          PPM.isNTCFault()      ? "NTC "   : "");
    lv_obj_set_style_text_color(detailLabel, lv_color_hex(0xE04040), LV_PART_MAIN);
  } else {
    lv_label_set_text_fmt(detailLabel, "VBUS %u mV   I %u mA  %s",
                          PPM.getVbusVoltage(), PPM.getChargeCurrent(),
                          PPM.getNTCStatusString());
    lv_obj_set_style_text_color(detailLabel, lv_color_hex(0x808088), LV_PART_MAIN);
  }

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
  lv_obj_align(cpuLabel, LV_ALIGN_TOP_MID, 0, 282);
  lv_label_set_text(cpuLabel, "CPU: -- /s");

  wakeLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(wakeLabel, lv_color_hex(0x40A0FF), LV_PART_MAIN);
  lv_obj_align(wakeLabel, LV_ALIGN_TOP_MID, 0, 320);
  lv_label_set_text(wakeLabel, "Wakes: -- /s");

  // LTR-553 light + proximity readout (Stage 5c step 2).
  lightLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(lightLabel, lv_color_hex(0xE0A040), LV_PART_MAIN);
  lv_obj_align(lightLabel, LV_ALIGN_TOP_MID, 0, 348);
  lv_label_set_text(lightLabel, "Light: --");

  // Boot counter + wake cause (Stage 5d): proves RTC memory survived deep sleep.
  bootLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(bootLabel, lv_color_hex(0x9090A0), LV_PART_MAIN);
  lv_obj_align(bootLabel, LV_ALIGN_TOP_MID, 0, 372);
  lv_label_set_text(bootLabel, "");

  // Power-bench mode readout (Stage 5e): big, because you read it, then it goes dark.
  benchLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(benchLabel, lv_color_hex(0x40FFC0), LV_PART_MAIN);
  lv_obj_set_style_text_font(benchLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(benchLabel, LV_ALIGN_TOP_MID, 0, 396);
  lv_label_set_text(benchLabel, "");

  // THREE action buttons share ONE row (stacking them buries bootLabel behind the
  // top one). On a 222 px-wide screen that's ~68 px each at x = -74 / 0 / +74, so
  // the captions have to be short. Still hand-aligned by absolute offset — this
  // third button is exactly the pressure that argues for LVGL flex/grid.
  lv_obj_t *camBtn = lv_button_create(scr);
  lv_obj_set_size(camBtn, 68, 44);
  lv_obj_align(camBtn, LV_ALIGN_BOTTOM_MID, -74, -6);
  lv_obj_set_style_bg_color(camBtn, lv_color_hex(0x704028), LV_PART_MAIN);
  lv_obj_add_event_cb(camBtn, cam_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *camLbl = lv_label_create(camBtn);
  lv_label_set_text(camLbl, "Cam");
  lv_obj_center(camLbl);

  lv_obj_t *pbBtn = lv_button_create(scr);
  lv_obj_set_size(pbBtn, 68, 44);
  lv_obj_align(pbBtn, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_bg_color(pbBtn, lv_color_hex(0x206048), LV_PART_MAIN);
  lv_obj_add_event_cb(pbBtn, bench_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *pbLbl = lv_label_create(pbBtn);
  lv_label_set_text(pbLbl, "Bench");
  lv_obj_center(pbLbl);

  // Deep Sleep trigger button.
  lv_obj_t *dsBtn = lv_button_create(scr);
  lv_obj_set_size(dsBtn, 68, 44);
  lv_obj_align(dsBtn, LV_ALIGN_BOTTOM_MID, 74, -6);
  lv_obj_set_style_bg_color(dsBtn, lv_color_hex(0x304080), LV_PART_MAIN);
  lv_obj_add_event_cb(dsBtn, deepsleep_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *dsLbl = lv_label_create(dsBtn);
  lv_label_set_text(dsLbl, "Sleep");
  lv_obj_center(dsLbl);
}

// ---- Enter light sleep, waking on the touch line OR the 1 s timer -----------
static void enterLightSleep(bool touchWake) {
  esp_sleep_enable_timer_wakeup(SLEEP_TIMER_US);
  if (touchWake) {
    gpio_wakeup_enable((gpio_num_t)TOUCH_IRQ, GPIO_INTR_LOW_LEVEL);  // touch pulses LOW
    esp_sleep_enable_gpio_wakeup();
  }

  esp_light_sleep_start();          // ---- CPU halts here until a wake event ----

  if (touchWake) gpio_wakeup_disable((gpio_num_t)TOUCH_IRQ);
  wakeCount++;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    lastActivityMs = millis();      // woken by touch: stay up to service it
  }
}

// The Deep Sleep button just raises a flag; loop() acts on it (don't sleep from
// inside an LVGL event callback).
static void deepsleep_event_cb(lv_event_t *e) { wantDeepSleep = true; }

// The Cam button also just raises a flag; loop() runs the (blocking) viewfinder —
// not from inside an LVGL event callback.
static void cam_event_cb(lv_event_t *e) { wantViewfinder = true; }

// ---- Stage 5e: power bench --------------------------------------------------
// Entering the bench from the button (mode A). Later modes are reached by tapping.
static void peripheralsOff(void) {
  touch.sleep();                      // CST226SE standby (needs a power cycle to return)
  if (alsOk) {
    als.disableLightSensor();
    als.disableProximity();
  }
}

static void bench_event_cb(lv_event_t *e) {
  // Stop charging for the duration of the bench. A "full" battery still does
  // periodic top-off cycles, and each one adds tens of mA to the meter at a moment
  // you can't predict — which is why A-B (a fixed backlight difference that should
  // be identical every run) came out as 18 mA once and 6 mA the next time.
  // Pinning this makes the differences reproducible. Restored on exit / on reboot.
  if (pmuOk) PPM.disableCharge();


  benchMode      = BENCH_BL_FULL;
  benchEnteredMs = millis();
  lv_label_set_text(benchLabel, benchName(benchMode));
}

// Advance the bench on a fresh touch. We poll the controller DIRECTLY rather than
// going through LVGL, because in modes B and C the screen is dark — there is no
// visible button to press, so any touch anywhere advances. Rising-edge only
// (press, not hold), with a guard so the tap that entered a mode can't skip it.
static void benchAdvanceOnTouch(void) {
  // The physical button (GPIO 16) advances the bench in EVERY mode. Modes E/F used
  // to auto-advance on a timer because the touch controller was asleep — but both
  // are dark, so you couldn't tell WHICH state the meter was showing. A button works
  // in all of them and puts the transition under your control.
  bool btnHigh    = (digitalRead(BTN_WAKE) == HIGH);
  bool btnPressed = (benchBtnPrevHigh && !btnHigh);
  benchBtnPrevHigh = btnHigh;

  // Touch only when the controller is still awake: once asleep it won't answer, and
  // every getPoint() becomes an I2C NACK/timeout that burns CPU instead of sleeping.
  // That cost 31 mA and made mode E read HIGHER than mode D — the instrument
  // measuring itself.
  bool pressed = false;
  if (!periphOff) {
    int16_t x[1], y[1];
    pressed = touch.getPoint(x, y, 1) > 0;
  }

  if ((btnPressed || (pressed && !benchTouchPrev)) &&
      (millis() - benchEnteredMs) > 500) {
    benchMode      = (benchMode + 1) % BENCH_COUNT;   // wraps E -> off
    benchEnteredMs = millis();
    if (dispAsleep) {                 // leaving mode D: SLPOUT, or the panel stays dead
      panel->displayOn();
      dispAsleep = false;
      lv_obj_invalidate(lv_screen_active());   // SLPIN loses the image; force a full redraw
    }
    lv_label_set_text(benchLabel, benchName(benchMode));
    if (benchMode == BENCH_OFF) {
      lastActivityMs = millis();          // hand control back awake
      if (pmuOk) PPM.enableCharge();      // bench over: let the battery charge again
    }
  }
  benchTouchPrev = pressed;
}

// One bench iteration. Returns true if the bench handled this loop() pass.
static bool benchStep(void) {
  if (benchMode == BENCH_OFF) return false;

  lv_timer_handler();              // keep rendering (the announcement needs it)
  benchAdvanceOnTouch();
  if (benchMode == BENCH_OFF) return true;   // just exited; normal loop resumes next pass

  // Announce the mode at full brightness, THEN settle into the state being measured.
  if ((millis() - benchEnteredMs) < BENCH_ANNOUNCE_MS) {
    setBacklight(BL_MAX_DUTY);
    delay(5);
    return true;
  }

  switch (benchMode) {
    case BENCH_BL_FULL:                       // screen on, CPU busy: worst case
      setBacklight(BL_MAX_DUTY); delay(5); break;
    case BENCH_BL_OFF:                        // A minus B = what the SCREEN costs
      setBacklight(0);           delay(5); break;
    case BENCH_LIGHT_SLEEP:                   // B minus C = what the busy CPU costs
      setBacklight(0);
      enterLightSleep(true);                      // note: ignores the !onUsbCached gate
      break;
    case BENCH_DISP_SLEEP:                    // C minus D = cost of the ST7796 itself
      setBacklight(0);
      if (!dispAsleep) {                      // SLPIN once, not every loop
        panel->displayOff();                  // ST7796 SLPIN: stop scanning the panel
        dispAsleep = true;
      }
      enterLightSleep(true);
      break;
    case BENCH_ALL_OFF:                       // D minus E = cost of touch + ALS
      setBacklight(0);
      if (!dispAsleep) { panel->displayOff(); dispAsleep = true; }
      if (!periphOff)  { peripheralsOff();    periphOff  = true; }
      // Touch is asleep now, so a tap can't advance us — auto-advance instead, or
      // the bench would be stuck here until a manual reset.
      if ((millis() - benchEnteredMs) > 25000) {
        benchMode      = BENCH_DEEP_SLEEP;
        benchEnteredMs = millis() - BENCH_ANNOUNCE_MS;  // skip announce: screen is off
        break;
      }
      enterLightSleep(false);
      break;
    case BENCH_DEEP_SLEEP:                    // E minus F = the CPU's last contribution
      setBacklight(0);                        // ...and F itself = the firmware floor
      if (!dispAsleep) { panel->displayOff(); dispAsleep = true; }
      if (!periphOff)  { peripheralsOff();    periphOff  = true; }
      enterDeepSleep(false);                  // timer only; never returns (reboots)
      break;
  }
  return true;
}

// Power down to the RTC domain. This NEVER returns — the chip reboots on wake.
// touchWake=false arms ONLY the timer: a control experiment that isolates whether
// the touch line is what's waking us, and the reliable way to hold deep sleep still
// long enough for a USB meter to read it.
static void enterDeepSleep(bool touchWake) {
  // Farewell message, forced to the panel now (LVGL renders in loop, which we're
  // about to leave for good).
  lv_label_set_text(statusLabel, "Deep sleep - press button");
  lv_refr_now(lv_display_get_default());

  // Don't arm EXT1 until the line it watches is actually idle-HIGH — otherwise we
  // wake the instant we sleep. If it never settles, ABORT and say so on screen:
  // a visible "couldn't sleep" beats a silent reboot loop.
  // Don't arm EXT1 while the button is still held, or ANY_LOW fires immediately.
  // Unlike the touch line, this one genuinely idles high, so a simple wait works.
  if (touchWake) {
    uint32_t t0 = millis();
    while (digitalRead(BTN_WAKE) == LOW && (millis() - t0) < 5000) delay(10);
    delay(50);                                   // let any release bounce settle
  }

  // Backlight off and HELD low: GPIO 48 isn't an RTC pad, so without a hold its
  // level would float once the chip powers down (screen might glow).
  ledcWrite(TFT_BL, 0);
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_deep_sleep_hold_en();

  // Wake on the touch line going LOW (RTC pull-up keeps it HIGH while idle) or a
  // 20 s timer. EXT1 is the S3's multi-GPIO RTC wake source.
  // Wake on the PHYSICAL BUTTON (GPIO 16), not the touch line.
  //
  // Why the change: the CST226SE asserts IRQ and holds it LOW until someone reads
  // the pending report. While awake we poll constantly and drain it, so the line
  // looks quiet — but the instant we stop polling in order to sleep, the next event
  // latches it low and ANY_LOW fires immediately. A touch controller is structurally
  // a bad deep-sleep wake source: it asserts precisely when nobody is listening.
  //
  // GPIO 16 has a measured EXTERNAL pull-up, which is the real prize: internal
  // pull-ups live in RTC_PERIPH, a domain deep sleep switches off, so relying on
  // them forces esp_sleep_pd_config(..., ON) and burns current all night just to
  // hold a line high. An external resistor does it for free.
  gpio_hold_en((gpio_num_t)TOUCH_RST);   // keep the touch controller quiet/reset-stable

  if (touchWake) {
    esp_sleep_enable_ext1_wakeup(1ULL << BTN_WAKE, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_sleep_enable_timer_wakeup(20ULL * 1000000);

  esp_deep_sleep_start();          // ---- never returns; chip reboots on wake ----
}

// ---- Stage 8: live viewfinder ----------------------------------------------
// Capture RGB565 frames into PSRAM and push them straight to the ST7796 — no
// LVGL, no decode. QCIF (176x144) fits the 222-wide panel with NO scaling,
// cropping or rotation, so this is "first light" for the camera: get pixels on
// the glass, THEN refine size/orientation once we can see them.
//
// The camera OWNS the shared I2C pins while it streams (SCCB is on 5/6), so touch
// and the gauge are frozen for the duration. That's why we EXIT on the GPIO 16
// button — the one input that survives everything the firmware takes over (Stage
// 5f). Enter via the touch "Cam" button; leave via the physical button.
//
// This blocks the main loop until you exit. That's fine: we've taken over the
// screen, and esp_camera_fb_get() blocks on a semaphore (it yields to the RTOS),
// so nothing starves.
static void runViewfinder(void) {
  camera_config_t cfg = {};
  cameraFillPins(cfg);
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size   = FRAMESIZE_QCIF;     // 176x144: fits 222 wide, no scaling
  cfg.fb_count     = 2;                  // double-buffer so capture overlaps draw
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_LATEST; // always the freshest frame; drop stale ones

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    if (Serial) Serial.printf("viewfinder: init failed 0x%X (%s)\n",
                              err, esp_err_to_name(err));
    cameraRecoverBus();                  // hand the I2C bus back even on failure
    return;
  }

  // Orient the image in HARDWARE — the sensor flips during readout, so it costs no
  // CPU per frame (unlike rotating the buffer ourselves). "Inverted" = upside down
  // = a vertical flip. If the picture then reads left-right MIRRORED, the module is
  // mounted a full 180° and set_hmirror(s,1) is the other half.
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    // s->set_hmirror(s, 1);   // uncomment if the image is also mirrored left-right
  }

  setBacklight(BL_MAX_DUTY);
  panel->fillScreen(RGB565_BLACK);
  const int fw = 176, fh = 144;
  const int ox = (SCREEN_W - fw) / 2;    // 23  — centered horizontally
  const int oy = (SCREEN_H - fh) / 2;    // 168 — centered vertically

  // Seed the edge detector with the button's CURRENT level, so a still-held button
  // from entering can't instantly trip the exit.
  bool prevHigh = (digitalRead(BTN_WAKE) == HIGH);
  uint32_t frames = 0, t0 = millis();

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      // esp_camera's RGB565 is BIG-ENDIAN relative to the panel, so the "Be"
      // (big-endian) variant is the correct push — confirmed by eye after the
      // native-endian draw16bitRGBBitmap gave wrong colours.
      panel->draw16bitBeRGBBitmap(ox, oy, (uint16_t *)fb->buf, fb->width, fb->height);
      esp_camera_fb_return(fb);          // recycle the buffer or the queue starves
      frames++;
    }
    bool high = (digitalRead(BTN_WAKE) == HIGH);
    if (prevHigh && !high) break;        // GPIO 16 pressed: leave the viewfinder
    prevHigh = high;
  }

  uint32_t elapsed = millis() - t0;
  if (Serial && elapsed > 0)
    Serial.printf("viewfinder: %lu frames / %lu ms = ~%lu fps\n",
                  (unsigned long)frames, (unsigned long)elapsed,
                  (unsigned long)(frames * 1000 / elapsed));

  // Wait for the exit button to be released so it doesn't bleed into other button
  // handlers on the way out.
  while (digitalRead(BTN_WAKE) == LOW) delay(10);

  esp_camera_deinit();
  cameraRecoverBus();

  // Repaint the LVGL gauge screen we clobbered with camera frames.
  panel->fillScreen(RGB565_BLACK);
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(lv_display_get_default());
  lastActivityMs = millis();
}

// ---- Stage 6: mount the microSD ------------------------------------------
// Order matters: this runs AFTER panel->begin(). Initialising SPIClass first left
// the screen BLACK — panel->begin() can't drive a bus another driver already owns.
// These two share the bus fine at RUN time (both bracket their transfers), but at
// INIT time the display must go first.
//
// Debugging note: the card originally failed with cardType()==0, which I read as
// "no comms, electrical". WRONG — cardType() reads the MOUNTED card struct, so it
// returns 0 for ANY begin() failure, including a filesystem it can't read. The card
// simply needed reformatting (FAT32; cards >32GB ship as exFAT, which this library
// cannot mount). t= could never distinguish the two cases.
static void initSD(void) {
  // What is MISO doing before anyone drives it? A pull-up that still reads LOW means
  // something is holding the line down — which would explain a card that never answers.
  pinMode(TFT_MISO, INPUT_PULLUP);
  delay(2);
  int misoIdle = digitalRead(TFT_MISO);

  sdSPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // FASTEST FIRST. The original ladder ran slow->fast and stopped at the first
  // success, which finds the SLOWEST working speed — useless. 400 kHz is ~50 KB/s.
  static const uint32_t sdSpeeds[] = {20000000, 10000000, 4000000, 1000000, 400000};
  uint32_t sdHz = 0;
  for (uint8_t i = 0; i < 5 && !sdOk; i++) {
    if (SD.begin(SD_CS, sdSPI, sdSpeeds[i])) { sdOk = true; sdHz = sdSpeeds[i]; }
    else SD.end();
    delay(20);
  }

  if (sdOk) {
    uint8_t type = SD.cardType();
    const char *t = (type == CARD_MMC) ? "MMC" : (type == CARD_SD)  ? "SD"
                  : (type == CARD_SDHC) ? "SDHC" : "?";
    // The boot line fits ~28 chars at font 14 on a 222 px screen, and the camera
    // token pushed it past that — the tail was silently CLIPPED, which cost two
    // rounds of misread results. The mount speed was a one-time finding (lesson
    // 06); it doesn't need to ride on the status line forever.
    snprintf(sdInfo, sizeof(sdInfo), "SD%lluG",
             SD.cardSize() / (1024ULL * 1024ULL * 1024ULL));
    (void)sdHz;
  } else {
    snprintf(sdInfo, sizeof(sdInfo), "SDfail miso=%d", misoIdle);
  }

}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  // Native-USB CDC: the port re-enumerates on reset, so setup()'s one-shot logs
  // fly past before a monitor reattaches. Wait up to 1.5 s for a host to connect
  // so we CATCH the boot output — but time out, so a headless (battery) boot only
  // pauses briefly instead of hanging forever on `while (!Serial)`.
  uint32_t serialWait = millis();
  while (!Serial && (millis() - serialWait) < 1500) delay(10);
  delay(300);

  // Deep-sleep bookkeeping (Stage 5d): release any pin holds left from a previous
  // deep sleep, bump the RTC-memory boot counter, and record WHY we booted.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)TFT_BL);
  gpio_hold_dis((gpio_num_t)TOUCH_RST);        // released before touch.begin() drives it
  rtc_gpio_hold_dis((gpio_num_t)TOUCH_IRQ);    // ...and before we reclaim the IRQ pad
  bootCount++;
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:  wakeReason = "button"; break;
    case ESP_SLEEP_WAKEUP_TIMER: wakeReason = "timer"; break;
    default:                     wakeReason = "power-on"; break;
  }

  // Backlight via PWM (LEDC): 8-bit duty (0-255) at 5 kHz, so we can DIM it.
  // Auto-brightness in loop() takes over once the light sensor reads.
  // Backlight stays OFF through the whole bring-up: the panel's GRAM is random at
  // power-on, so lighting up before it's initialised and cleared shows white noise.
  // Raised at the very end of setup(), once the first frame is actually rendered.
  ledcAttach(TFT_BL, 5000, 8);
  setBacklight(0);
  panel->begin();
  panel->fillScreen(RGB565_BLACK);

  initSharedBusSensors();   // touch + PMU + ALS on the shared I2C bus

  // Pull-up so the IRQ line idles firmly HIGH (only a real driven-LOW pulse
  // triggers the level wake — see Step 2's phantom-edge finding).
  // Hand the pad back to the digital domain: enterDeepSleep() switches it to its RTC
  // function, and that survives the reboot. Without this, digitalRead(TOUCH_IRQ) and
  // the light-sleep GPIO wake would both be reading a pad we no longer control.
  rtc_gpio_deinit((gpio_num_t)TOUCH_IRQ);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  pinMode(BTN_WAKE, INPUT_PULLUP);   // external pull-up exists; this is belt-and-braces

  initSD();   // after the display: it must own the bus first (see initSD's notes)

  // Camera shield probe (Stage 7). Runs after the shared-bus sensors are up, and
  // hands the I2C bus back to them afterwards (it borrows pins 5/6 for SCCB).
  cameraDriverProbe();   // the driver that actually knows SCCB (and how to clock it)

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
  // Exercise the filesystem now that everything else is up.
  if (sdOk) {
    bool st = sdSelfTest();
    uint32_t boots = sdLogBoot(bootCount, wakeReason);
    char tail[16];
    snprintf(tail, sizeof(tail), "%s L%lu", st ? "" : " RWFAIL", (unsigned long)boots);
    strncat(sdInfo, tail, sizeof(sdInfo) - strlen(sdInfo) - 1);
  }
  // "%.3s" clips the wake reason to btn/tim/pow on screen only — the SD boot log
  // still records the full word. The camera token must survive to its last
  // character: a truncated PID reads as a different sensor.
  lv_label_set_text_fmt(bootLabel, "#%u %.3s %s%s", bootCount, wakeReason, sdInfo, camInfo);
  lastActivityMs = millis();

  // First real frame on the panel, THEN the light. Same rule as the wake path:
  // never illuminate a buffer you haven't drawn.
  lv_refr_now(lv_display_get_default());
  setBacklight(autoBrightness);
}

void loop() {
  loopCount++;

  // Power bench (Stage 5e) takes over the loop entirely when active: it holds one
  // power state still so a USB meter can settle, instead of the timer-driven
  // behaviour below.
  if (benchStep()) return;

  lv_timer_handler();

  if (wantViewfinder) { wantViewfinder = false; runViewfinder(); return; }  // Cam button
  if (wantDeepSleep) enterDeepSleep(true);  // Sleep button — touch wake armed

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
  // On USB the screen stays lit: power isn't scarce, and a screen that blanks
  // every 5 s makes it impossible to WATCH a slow value (charge current tapering,
  // Stage 5e). Same reasoning that already gates CPU sleep on !onUsbCached — so
  // the auto-off demo still runs, just on battery where it earns its keep.
  bool screenIdle = !onUsbCached && (millis() - lastActivityMs) > BL_OFF_MS;
  applyScreenPower(screenIdle);
  if (!onUsbCached && screenIdle && lv_anim_count_running() == 0) {
    enterLightSleep(true);
  } else {
    delay(5);
  }
}
