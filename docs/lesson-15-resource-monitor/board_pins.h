// ============================================================================
//  board_pins.h — every pin and tuning constant for the LilyGO T-Display S3 Pro
// ----------------------------------------------------------------------------
//  Stage 15 refactor: these lived as #defines at the top of main.cpp. Moving
//  them here is the first step in breaking that 1500-line file into modules.
//
//  WHY constexpr AND NOT #define
//  -----------------------------
//  A #define is a *preprocessor* trick: the compiler never sees the name, only
//  the substituted text. That means it has no type, no scope, and no presence
//  in the debugger — and if it collides with a name in a library header, you
//  get an error pointing at code you didn't write.
//
//  `constexpr` is a real C++ variable that happens to be known at compile time.
//  It costs exactly the same at runtime (zero — it's folded into the code), but
//  it is TYPED, it is SCOPED, and the debugger knows its name. In modern C++
//  this is the default choice; #define is reserved for things constexpr can't
//  do (conditional compilation, stringising).
//
//  Coming from Python: think of #define as a blind find-and-replace on your
//  source before it compiles, versus a proper module-level constant.
//
//  `#pragma once` is the include guard: it tells the compiler to process this
//  file only once per translation unit, even if several headers include it.
// ============================================================================

#pragma once

#include <stdint.h>

// ---- Display pins (SPI) ----
constexpr int TFT_SCLK = 18;
constexpr int TFT_MOSI = 17;
constexpr int TFT_MISO =  8;
constexpr int TFT_CS   = 39;
constexpr int TFT_DC   =  9;
constexpr int TFT_RST  = 47;
constexpr int TFT_BL   = 48;

// ---- Touch + PMU share this I2C bus ----
constexpr int I2C_SDA   =  5;
constexpr int I2C_SCL   =  6;
constexpr int TOUCH_RST = 13;
constexpr int TOUCH_IRQ = 21;
constexpr int BTN_WAKE  = 16;  // physical user button: EXTERNAL pull-up, RTC-capable (Stage 5f)
constexpr int BTN_WIFI  = 12;  // the OTHER half of the 16 rocker (re-found 2026-07-21); triggers WiFi
constexpr int SD_CS     = 14;  // microSD chip-select — SHARES SCLK/MOSI/MISO with the display

// ---- Camera shield, DVP (Stage 7 probe) ----
// Pins from LilyGO's CameraShield example utilities.h. Note SIOD/SIOC are
// I2C_SDA/I2C_SCL — the sensor is just another device on the bus we already
// share with touch, the PMU and the ALS.
constexpr int      CAM_XCLK    = 11;         // master clock we must generate FOR the sensor
constexpr int      CAM_PWDN    = 46;         // active-HIGH power-down; LOW = sensor running
constexpr uint32_t CAM_XCLK_HZ = 20000000;
constexpr int      CAM_RESET   = -1;         // not broken out on this shield: sensor self-resets
constexpr int      CAM_PCLK    =  2;
constexpr int      CAM_VSYNC   =  7;
constexpr int      CAM_HREF    = 15;
// 8-bit parallel data bus. Note D5/D4/D3 land on GPIO 42/40/41 — the JTAG pins —
// and D0 on GPIO 45, a strapping pin. Fine at runtime, but it means a fitted
// camera is electrically present on pins the chip cares about during boot.
constexpr int CAM_D7 =  4;
constexpr int CAM_D6 = 10;
constexpr int CAM_D5 =  3;
constexpr int CAM_D4 =  1;
constexpr int CAM_D3 = 42;
constexpr int CAM_D2 = 40;
constexpr int CAM_D1 = 41;
constexpr int CAM_D0 = 45;

// ---- Panel geometry ----
constexpr int SCREEN_W = 222;
constexpr int SCREEN_H = 480;

// ---- Sleep / backlight tuning ----
constexpr uint64_t SLEEP_TIMER_US = 1000000ULL;  // wake every 1 s (refresh gauge) while asleep
constexpr uint32_t BL_OFF_MS      = 5000;        // screen off after this long with no activity
constexpr int      BL_MIN_DUTY    = 24;          // backlight floor while active (~10%, readable)
constexpr int      BL_MAX_DUTY    = 255;         // full brightness
constexpr int      LIGHT_DARK     = 1;           // LTR-553 CH0 count mapped to BL_MIN_DUTY
constexpr int      LIGHT_BRIGHT   = 40;          // ...and this count mapped to BL_MAX_DUTY
constexpr int      PROX_WAKE      = 30;          // proximity above this counts as activity
                                                 // (LTR-553 PS is short-range: ~0 until a hand
                                                 // is close/over it, then hundreds)
