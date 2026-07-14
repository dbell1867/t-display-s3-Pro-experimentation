# Lesson 01 — From Zero to First Light

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Go from a bare, freshly-connected board to your own C++
code running on it and drawing to the screen — understanding *every* step, not
just copy-pasting.

> Audience note: written for someone comfortable with **Python / CircuitPython**
> who is learning **C++** and embedded development. C++ concepts are introduced
> by translating from Python where possible.

---

## Learning objectives

By the end of this lesson you can:

1. Explain the language and UX options for an ESP32 board and why we chose C++ + Arduino_GFX.
2. Give your Linux user permission to talk to a USB serial device.
3. Install and use **PlatformIO** to build and flash firmware.
4. Read the ESP32-S3 serial console from the command line.
5. Understand the Arduino program model (`setup()` / `loop()`), pointers, and `->`.
6. Drive an SPI TFT display: pins, panel offset, color format, and text.
7. Read a compiler error and fix it, including a library/framework version mismatch.

---

## The hardware

**LilyGO T-Display S3 Pro**

- **MCU:** ESP32-S3R8 — dual-core LX7, WiFi + BLE, **16 MB flash, 8 MB PSRAM**
- **Display:** 2.33" IPS TFT, **222 × 480**, driver chip **ST7796** (SPI bus)
- **Touch:** capacitive **CST226SE** (I²C) — *added in a later lesson*
- **Connection:** USB-C using the ESP32-S3's **native USB** (no separate serial chip)

### Pin map (from LilyGO's `utilities.h`)

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| SPI SCLK | 18 | TFT CS | 39 |
| SPI MOSI | 17 | TFT DC | 9 |
| SPI MISO | 8  | TFT RST | 47 |
| I²C SDA | 5 | TFT backlight | 48 |
| I²C SCL | 6 | Touch RST | 13 |

---

## Design decisions (made before any code)

### Language

| Option | Verdict |
|---|---|
| **C++ / Arduino** | ✅ Chosen. Best library + docs support for this board; the whole ESP32 ecosystem speaks it. Steeper than Python but the friction is educational. |
| C / ESP-IDF | More control, more boilerplate. Overkill to start. |
| MicroPython / CircuitPython | Familiar, fast to iterate, but heavier and driver support for this exact board is fiddly. Kept as a fallback. |
| Rust (esp-rs) | Modern and safe, but smaller ecosystem for this display. |

**Why C++ despite being new to it:** embedded/Arduino C++ is a *friendlier subset*
of C++ (mostly functions, simple objects, library calls — not heavy templates or
inheritance), and coming from Python you already know how to *program* — you're
only learning new syntax and mechanics.

### Graphics / UX library

- **Arduino_GFX** ✅ — a drawing library (think CircuitPython's `displayio`): text,
  shapes, images, color. It's what LilyGO's own examples use for this panel, so it
  already knows the ST7796's quirks. **Chosen for first light.**
- **TFT_eSPI** — popular and fast, but needs fiddly manual config for this panel.
- **LVGL** — a full *widget* toolkit (buttons, sliders, animations) that sits *on
  top of* a display driver. The destination for real UX — tackled after the raw
  display is proven.

**Method:** prove the display with a simple drawing library first; add the heavy
UI framework only once the hardware is known-good. Debug one layer at a time.

---

## Module 1 — Give your user permission to talk to the board

On Linux, hardware appears as files under `/dev/`. Our board is `/dev/ttyACM0`:

```
crw-rw---- 1 root uucp ... /dev/ttyACM0
```

Read that as: owner `root`, **group `uucp`**, permissions `rw-rw----` (owner and
group can read/write, others get nothing). To use the port without `sudo`, add
yourself to the `uucp` group:

```bash
sudo usermod -aG uucp $USER
```

- `usermod` — modify a user account
- `-a` — **append** (don't replace your existing groups — omitting this is dangerous)
- `-G uucp` — the group to add
- `$USER` — your username

> **Distro note:** on Arch the serial group is `uucp`; on Debian/Ubuntu it's
> `dialout`. Read the actual group from `ls -l /dev/ttyACM0`.

**Gotcha:** group membership is only picked up by **new login sessions**. After
running the command you must **log out and back in** (or reboot). Verify with:

```bash
getent group uucp   # shows your account is a member (updates immediately)
id -nG              # shows what your CURRENT session sees (needs re-login)
```

---

## Module 2 — Install the toolchain (PlatformIO)

**PlatformIO** is a build system + package manager for embedded work. Per project
it downloads the right compiler, the framework (Arduino-for-ESP32), and any
libraries, then builds and flashes — all driven by one `platformio.ini` file.

Install into an isolated environment (doesn't touch system Python):

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py
```

Then put `pio` on your `PATH` by adding this to `~/.bashrc`:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

Verify: `pio --version`.

---

## Module 3 — Stage 1: "Hello, serial"

**Idea:** before touching the display (where many things can break at once), prove
the whole pipeline — *compile C++ → flash → read output* — with a trivial program.

### `platformio.ini`

```ini
[env:tdisplay-s3-pro]
platform  = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
board     = esp32-s3-devkitm-1
framework = arduino

board_build.mcu        = esp32s3
board_build.flash_size = 16MB
board_upload.flash_size = 16MB

build_flags =
	-DARDUINO_USB_MODE=1        ; use the chip's built-in USB
	-DARDUINO_USB_CDC_ON_BOOT=1 ; expose Serial over that USB at boot

monitor_speed = 115200
```

> The two `build_flags` are the ESP32-S3-specific magic: they route `Serial` onto
> the **native USB** port, so the same USB-C cable that powers the board carries
> its console. Without them you'd see nothing.

### `src/main.cpp`

```cpp
#include <Arduino.h>   // like a Python import; pastes in declarations at compile time

void setup() {          // runs ONCE at boot (top-level script setup)
  Serial.begin(115200); // open USB serial at 115200 baud
  delay(2000);          // let the USB port re-enumerate
  Serial.println("=== T-Display S3 Pro is alive! ===");
}

int counter = 0;        // explicitly typed integer (C++ makes types explicit)

void loop() {           // runs FOREVER, over and over (the framework provides the while-True)
  counter++;
  Serial.printf("tick %d  (uptime %lu ms)\n", counter, millis());
  delay(1000);
}
```

### Build, flash, monitor

```bash
pio run              # compile only (first run downloads the toolchain — minutes)
pio run -t upload    # build + flash to the board
pio device monitor   # watch serial (115200); Ctrl-C to quit
```

**Success looks like** boot ROM chatter, then your `=== ... alive! ===` line, then
`tick 1`, `tick 2`, ... incrementing once per second.

> **Monitoring gotcha:** `pio device monitor` needs a real interactive terminal.
> In a script/non-TTY context it fails with `termios: Inappropriate ioctl for
> device`. From code you can instead read the port with `pyserial`:
> ```python
> import serial, time
> ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
> ser.dtr = False; time.sleep(0.1); ser.dtr = True   # pulse DTR to reset the board
> # ... read ser.readline() in a loop ...
> ```

---

## Module 4 — Stage 2: First light (the display)

### Add the library

In `platformio.ini` (libraries are per-project, like `requirements.txt`):

```ini
lib_deps =
	moononournation/GFX Library for Arduino
```

### The display code (`src/main.cpp`)

```cpp
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Pin map
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_MISO  8
#define TFT_CS   39
#define TFT_DC    9
#define TFT_RST  47
#define TFT_BL   48

// Two layers: a "data bus" (how to push bytes over SPI) and a "driver"
// (the ST7796's command language + geometry).
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

// Args: bus, reset, rotation, IPS?, width, height, col-offset, row-offset.
// 222 width + 49 column offset are THIS panel's specific quirk.
Arduino_GFX *gfx = new Arduino_ST7796(bus, TFT_RST, 0, true, 222, 480, 49, 0);

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TFT_BL, OUTPUT);        // backlight ON — without this the panel is dark
  digitalWrite(TFT_BL, HIGH);     //   even when drawing succeeds

  if (!gfx->begin()) {            // begin() returns false if it can't reach the panel
    Serial.println("ERROR: gfx->begin() failed!");
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0,   0, 222, 120, RGB565_RED);
  gfx->fillRect(0, 120, 222, 120, RGB565_GREEN);
  gfx->fillRect(0, 240, 222, 120, RGB565_BLUE);
  gfx->fillRect(0, 360, 222, 120, RGB565_WHITE);

  gfx->setTextColor(RGB565_WHITE); // one color = transparent text
  gfx->setTextSize(3);
  gfx->setCursor(12, 20);
  gfx->println("Hello!");
}

void loop() { delay(1000); }
```

### New C++ concepts in this module

- **Pointers & `new`.** `Arduino_GFX *gfx = new Arduino_ST7796(...)` creates an
  object and stores a **pointer** to it (the `*`). In Python every variable is
  already a reference; C++ makes that explicit and lets you choose.
- **`->` vs `.`** Call a method *through a pointer* with `gfx->begin()`. If `gfx`
  were a plain object (not a pointer) you'd write `gfx.begin()`. Same idea as
  Python's dot.
- **`#define NAME value`** — a compile-time text substitution (a named constant).

### Display concepts

- **The `49` column offset** — the panel's visible pixels don't start at the
  driver's column 0. Wrong offset → shifted/garbled image. We copied LilyGO's
  proven value rather than guessing.
- **RGB565** — 16-bit color: 5 bits red, 6 green, 5 blue.
- **`setTextColor(fg)`** draws only glyph pixels (transparent). **`setTextColor(fg, bg)`**
  fills a solid box behind the text (useful for cleanly overwriting changing values).

---

## Module 5 — Debugging we actually hit (the real lessons)

### Bug 1 — library needs a newer framework

```
fatal error: esp32-hal-periman.h: No such file or directory
```

**Cause:** the latest Arduino_GFX (1.6.6) requires **ESP32 Arduino core 3.x**, but
the default `platform = espressif32` installed the legacy **core 2.0.17** (shown as
`framework-arduinoespressif32 @ 3.20017...`, where `3.20017` = core `2.0.17`). The
library and framework were out of sync — the embedded version of "this package
needs a newer Python."

**Fix:** switch to the community **pioarduino** platform, which ships the modern
core 3.x (the official platform stalled on 2.0.x):

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
```

**Takeaway:** a library and its underlying framework must be version-compatible.
When a header is "missing," suspect a version mismatch before anything else.

### Bug 2 — renamed color constants

```
error: 'RED' was not declared in this scope; did you mean 'RER'?
```

**Cause:** Arduino_GFX 1.6 renamed its color macros to avoid clashing with other
libraries — plain `RED`/`BLACK` became `RGB565_RED`/`RGB565_BLACK`.

**Takeaway:** libraries rename things across major versions. The compiler's "did
you mean...?" and the library headers are your friends.

### Fix 3 — the black text boxes

Two-argument `setTextColor(fg, bg)` fills a background box. Using one argument
makes the text transparent. A one-line change — and a demonstration that the
edit → upload loop is now fast.

---

## What you built and learned

- ✅ Toolchain installed; board flashing + serial working
- ✅ Modern ESP32 core 3.x foundation (pioarduino)
- ✅ Display fully working — color, shapes, text — with real understanding
- ✅ The C++ **edit → compile → flash → observe** loop
- ✅ Pointers, `->` vs `.`, `#define`
- ✅ Reading and fixing real compiler errors, incl. a version mismatch

## Command cheat-sheet

```bash
pio run                     # build only
pio run -t upload           # build + flash
pio device monitor          # serial console (Ctrl-C to exit)
pio run -t clean            # wipe build outputs
getent group uucp           # confirm serial-group membership
```

## Glossary

- **PlatformIO** — embedded build system + package/toolchain manager.
- **Framework / core** — the Arduino API layer for the ESP32 (`setup`/`loop`, `Serial`, ...).
- **Platform** — the PlatformIO package that bundles a chip family's toolchain + framework.
- **SPI** — fast serial bus used to talk to the display.
- **I²C** — two-wire bus used by the touch controller and sensors.
- **DC pin** — "data/command": tells the display whether bytes are a command or pixel data.
- **Backlight** — the LED lighting the panel; separate from drawing pixels.
- **RGB565** — 16-bit color packing (5 red / 6 green / 5 blue).
- **Panel offset** — pixel gap between the driver's origin and the visible glass.

## Next lesson

**Stage 2b — Touch:** bring the CST226SE capacitive touch to life and react to
taps, completing the input+output picture. Then decide whether to adopt **LVGL**
for a real widget-based UI.
