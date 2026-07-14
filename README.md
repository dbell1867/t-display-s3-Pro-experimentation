# T-Display S3 Pro — Experimentation

A learning-focused embedded project: bringing up a **LilyGO T-Display S3 Pro**
(ESP32-S3) in **C++** with **PlatformIO**, one layer at a time — serial, then
display, then touch — with a written, project-based lesson for each stage.

The goal is understanding, not just working code: every step is explained, and
the real bugs hit along the way are documented as part of the material.

---

## The board

**LilyGO T-Display S3 Pro**

- **MCU:** ESP32-S3R8 — dual-core LX7, Wi-Fi + BLE, **16 MB flash, 8 MB PSRAM**
- **Display:** 2.33" IPS TFT, **222 × 480**, **ST7796** driver (SPI)
- **Touch:** capacitive **CST226SE** (I²C, address `0x5A`)
- **USB:** native USB-C (no separate serial chip)

### Pin map (from LilyGO's `utilities.h`)

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| SPI SCLK | 18 | TFT CS | 39 |
| SPI MOSI | 17 | TFT DC | 9 |
| SPI MISO | 8  | TFT RST | 47 |
| I²C SDA  | 5  | TFT backlight | 48 |
| I²C SCL  | 6  | Touch RST | 13 |
| Touch IRQ | 21 | | |

---

## Current state

| Stage | Status | What it does |
|---|---|---|
| 1 — Hello serial | ✅ | prints over native-USB serial (compile → flash → read proven) |
| 2a — First light | ✅ | color bars + text on the ST7796 via Arduino_GFX |
| 2b — Touch | ✅ | CST226SE touch (SensorLib); calibrated; on-screen CLEAR button |

**The firmware in `src/main.cpp` right now** is the Stage 2b touch demo: touch the
screen to draw yellow dots, tap the blue **CLEAR** bar to wipe them.

See [`docs/PLAN.md`](docs/PLAN.md) for the full roadmap (interactive app → PSRAM
framebuffer → LVGL → onboard sensors/PMU).

---

## Repository layout

```
platformio.ini              PlatformIO config (board, core, libs, USB flags)
src/main.cpp                current firmware (Stage 2b touch demo)
docs/
  PLAN.md                   resumable roadmap + design decisions
  lesson-01-first-light.md  zero → serial → display
  lesson-02-touch.md        touch, calibration, polling-vs-interrupt, the freeze bug
  lesson-NN-*/main.cpp      code snapshot for each lesson
```

---

## Toolchain

- **[PlatformIO](https://platformio.org/)** — build system + package manager.
- **Modern ESP32 Arduino core 3.x** via the community
  [**pioarduino**](https://github.com/pioarduino/platform-espressif32) platform
  (the official platform stalled on the legacy core 2.0.x, which is too old for
  Arduino_GFX / SensorLib).
- Libraries (auto-installed on first build): `moononournation/GFX Library for
  Arduino`, `lewisxhe/SensorLib`.

Everything is pinned in [`platformio.ini`](platformio.ini); no manual library
installs needed.

---

## Build & flash

```bash
pio run                 # compile only (first run downloads the toolchain — minutes)
pio run -t upload       # build + flash over USB
pio device monitor      # serial console @ 115200 (Ctrl-C to quit)
```

**Linux serial permissions:** the USB port is group-owned (`uucp` on Arch,
`dialout` on Debian/Ubuntu). Add yourself once and re-login:

```bash
sudo usermod -aG uucp $USER      # or dialout; check `ls -l /dev/ttyACM0`
```

---

## Lessons

Written for someone comfortable with **Python / CircuitPython** who is learning
**C++** — new C++ ideas are introduced by translating from Python.

- **[Lesson 01 — From Zero to First Light](docs/lesson-01-first-light.md):**
  permissions, PlatformIO, serial, and driving the display; pointers & `->`,
  RGB565, panel offset, and a real framework/library version-mismatch bug.
- **[Lesson 02 — Bringing Touch to Life](docs/lesson-02-touch.md):** I²C vs SPI,
  the CST226SE, calibrating touch orientation from measured data, polling vs
  interrupt, hit-testing a button, and diagnosing a native-USB `Serial` freeze.

---

## Notable gotchas learned (all detailed in the lessons)

- Arduino_GFX 1.6+ needs **core 3.x** (`esp32-hal-periman.h` errors otherwise) →
  use the pioarduino platform.
- Arduino_GFX 1.6 renamed colors to `RGB565_*` (e.g. `RGB565_RED`).
- The ST7796 panel needs a **49 px column offset** and the backlight pin driven HIGH.
- Touch orientation is **measured, not assumed** (this board = identity in portrait).
- **Native-USB `Serial.print` blocks** when nothing drains it — froze the loop
  after ~15 touches. Fixed with `Serial.setTxTimeoutMs(0)` (non-blocking writes).

---

## Acknowledgements

- [LilyGO](https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro) — board, schematics,
  and example code.
- [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) and
  [SensorLib](https://github.com/lewisxhe/SensorLib) — the display and touch drivers.
