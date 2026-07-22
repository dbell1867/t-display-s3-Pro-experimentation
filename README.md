# T-Display S3 Pro — Experimentation

A learning-focused embedded project: bringing up a **LilyGO T-Display S3 Pro**
(ESP32-S3) in **C++** with **PlatformIO**, one layer at a time — from serial and
display through touch, LVGL, power management, the camera, and finally WiFi and
BLE — with a written, project-based lesson for each stage.

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

🎉 **Full board tour complete — Stages 1–14.** A blank board taken all the way to
two wireless apps plus power characterisation. Every stage has a written lesson
and a `main.cpp` snapshot under `docs/`.

| Stage | Status | What it does |
|---|---|---|
| 1 — Hello serial | ✅ | prints over native-USB serial (compile → flash → read proven) |
| 2a — First light | ✅ | color bars + text on the ST7796 via Arduino_GFX |
| 2b — Touch | ✅ | CST226SE touch (SensorLib); calibrated; on-screen CLEAR button |
| 3 — Interactive app | ✅ | tap counter; reusable `Button` struct; edge-detected taps |
| 3b — Fading trail | ✅ | PSRAM `Arduino_Canvas` framebuffer; time-based fading trail |
| 4 — LVGL | ✅ | a real widget toolkit driving the UI |
| 5 — Battery gauge | ✅ | SY6970 PMU battery gauge (XPowersLib) |
| 5b — Light sleep | ✅ | light sleep + wake on touch |
| 5c — Backlight | ✅ | PWM backlight + LTR-553 auto-brightness |
| 5d — Deep sleep | ✅ | power down, reboot on touch/timer wake |
| 5e — Power meter | ✅ | measuring the current draw we'd assumed |
| 5f — Button wake | ✅ | choosing a wake source you can trust |
| 6 — microSD | ✅ | FAT filesystem over the shared SPI bus |
| 7 — Camera | ✅ | camera init and sensor probes |
| 8 — Viewfinder | ✅ | live camera preview on the glass |
| 9 — Capture still | ✅ | capture a frame to JPEG on the card |
| 10 — WiFi scan | ✅ | prove the radio with a network scan |
| 11 — WiFi AP | ✅ | board becomes its own access point |
| 12 — HTTP server | ✅ | `WebServer(80)` photo gallery at `192.168.4.1` |
| 13 — Bluetooth LE | ✅ | BLE Battery Service (`0x180F`) with notifications |
| 14 — Radio power | ✅ | measured cost: WiFi ≈ +45 mA, BLE ≈ +9 mA over baseline |

**The firmware in `src/main.cpp` right now** is the full **Stage 14** app: touch
**Cam** for the live viewfinder (tap GPIO 16 to capture a JPEG to SD, hold to
exit); GPIO 12 **tap** raises a WiFi hotspot serving the photo gallery at
`http://192.168.4.1/`; GPIO 12 **hold** starts the BLE Battery Service; GPIO 16
opens the radio power bench. To run an earlier stage instead, flash its
environment — see [Flashing a specific lesson](#flashing-a-specific-lesson) below.

See [`docs/PLAN.md`](docs/PLAN.md) for the full roadmap and design decisions.

---

## Repository layout

```
platformio.ini              PlatformIO config: shared base + one env per lesson
src/main.cpp                current firmware (full Stage 14 app)
docs/
  PLAN.md                   resumable roadmap + design decisions
  lesson-NN-*.md            the written lesson for each stage (01 … 14)
  lesson-NN-*/main.cpp      code snapshot for each stage (flash it with `-e`)
```

---

## Toolchain

- **[PlatformIO](https://platformio.org/)** — build system + package manager.
- **Modern ESP32 Arduino core 3.x** via the community
  [**pioarduino**](https://github.com/pioarduino/platform-espressif32) platform
  (the official platform stalled on the legacy core 2.0.x, which is too old for
  Arduino_GFX / SensorLib).
- Libraries (auto-installed on first build): `moononournation/GFX Library for
  Arduino`, `lewisxhe/SensorLib`, `lvgl/lvgl` (Stage 4+), and
  `lewisxhe/XPowersLib` (Stage 5+). The camera, SD, Wi-Fi, HTTP and BLE stages
  use drivers bundled inside the ESP32 core, so they need no extra libraries.

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

### Flashing a specific lesson

`src/main.cpp` is the *current* code. Each past stage is preserved under
`docs/lesson-NN-*/main.cpp`, and `platformio.ini` defines a matching
**environment** (`-e`) for every one. An environment builds only that lesson's
folder and pulls in only the libraries that lesson uses, so the build stays as
lean as it was when the stage was written.

```bash
pio run -e lesson-03b-fading-trail -t upload             # build + flash a lesson
pio run -e lesson-03b-fading-trail -t upload -t monitor  # + open the serial monitor
pio run -t upload                                        # your live src/ (the default)
```

List every available environment with `pio project config` (or see the
`[env:lesson-...]` sections in `platformio.ini`). The first build of any
environment is slow — each keeps its own `.pio/build/<env>/` cache, so the ESP32
core compiles once per lesson, then rebuilds incrementally.

---

## Lessons

Written for someone comfortable with **Python / CircuitPython** who is learning
**C++** — new C++ ideas are introduced by translating from Python.

Each lesson has a matching `main.cpp` snapshot you can flash with
`pio run -e lesson-NN-... -t upload`.

**Display & touch**
- **[01 — From Zero to First Light](docs/lesson-01-first-light.md):** permissions,
  PlatformIO, serial, and driving the display; pointers & `->`, RGB565, panel
  offset, and a real framework/library version-mismatch bug.
- **[02 — Bringing Touch to Life](docs/lesson-02-touch.md):** I²C vs SPI, the
  CST226SE, calibrating orientation from measured data, polling vs interrupt,
  hit-testing a button, and a native-USB `Serial` freeze.
- **[03 — Your First Interactive App](docs/lesson-03-interactive.md):** a tap
  counter; a reusable `Button` struct and crisp edge-detected taps.
- **[03b — A Fading Trail](docs/lesson-03b-fading-trail.md):** an `Arduino_Canvas`
  framebuffer in PSRAM, and the real cost of a full-frame flush.
- **[04 — LVGL](docs/lesson-04-lvgl.md):** moving up to a real widget toolkit.

**Power & sensors**
- **[05 — A Battery Gauge](docs/lesson-05-battery.md):** the SY6970 PMU via XPowersLib.
- **[05b — Light Sleep](docs/lesson-05b-low-power.md):** light sleep + wake on touch.
- **[05c — Backlight](docs/lesson-05c-backlight.md):** PWM + LTR-553 auto-brightness.
- **[05d — Deep Sleep](docs/lesson-05d-deep-sleep.md):** power down and reboot on wake.
- **[05e — The Power Meter](docs/lesson-05e-power-meter.md):** measuring what we assumed.
- **[05f — Button Wake](docs/lesson-05f-button-wake.md):** choosing a wake source you can trust.

**Storage, camera & radios**
- **[06 — microSD](docs/lesson-06-sdcard.md):** sharing a bus, and a diagnostic that lied.
- **[07 — The Camera](docs/lesson-07-camera.md):** camera init and four probes.
- **[08 — Live Viewfinder](docs/lesson-08-viewfinder.md):** camera pixels onto the glass.
- **[09 — Capture a Still](docs/lesson-09-capture-sd.md):** three buses, one JPEG on the card.
- **[10 — WiFi Scan](docs/lesson-10-wifi-scan.md):** proving the radio with a scan.
- **[11 — WiFi AP](docs/lesson-11-wifi-ap.md):** the board becomes an access point.
- **[12 — HTTP Server](docs/lesson-12-http-server.md):** the capstone photo gallery.
- **[13 — Bluetooth LE](docs/lesson-13-bluetooth-le.md):** the other half of the radio.
- **[14 — Radio Power](docs/lesson-14-radio-power.md):** measuring WiFi vs BLE power.

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
