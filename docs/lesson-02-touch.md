# Lesson 02 — Bringing Touch to Life

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Read finger taps from the **CST226SE** capacitive touch
controller, prove them over serial, draw where you touch, and **calibrate the
touch-to-screen orientation from real measurements** instead of guessing.

> Continues from [Lesson 01 — First Light](lesson-01-first-light.md). We keep the
> working display and add the *input* half of the picture.

---

## Learning objectives

By the end of this lesson you can:

1. Add a second library to a PlatformIO project and understand why we picked it.
2. Explain the **I²C bus** and how it differs from the SPI bus the display uses.
3. Initialize an I²C peripheral: reset pin, interrupt pin, address, SDA/SCL.
4. Contrast a **plain object** (`.`) with a **pointer object** (`->`) in C++.
5. Read multitouch points in `loop()` and draw to the exact touch location.
6. **Calibrate an orientation empirically** — derive swap/mirror from corner data.

---

## The hardware for this lesson

**CST226SE** capacitive touch controller, sitting on the shared **I²C** bus.

| Function | GPIO | Notes |
|---|---|---|
| I²C SDA | 5 | data line (shared with sensors/PMU) |
| I²C SCL | 6 | clock line |
| Touch RST | 13 | resets the touch chip |
| Sensor IRQ | 21 | chip pulses this when a touch happens |
| I²C address | `0x5A` | as `CST226SE_SLAVE_ADDRESS` in SensorLib |

---

## Design decision — which touch library?

The plan flagged two candidates. We resolved it by reading LilyGO's **actual**
`CapacitiveTouch` example in their T-Display-S3-Pro repo:

| Option | Verdict |
|---|---|
| **SensorLib** (`lewisxhe/SensorLib`), class `TouchDrvCSTXXX` | ✅ Chosen. It's what LilyGO's official example uses; auto-detects the CST226SE and exposes a clean `getPoint()` API. |
| TouchLib (`mmMicky/TouchLib`) | Also listed as a dependency, but the example's touch code is all SensorLib. |

> **Method reminder:** *research this board fresh.* The library question was
> settled by reading the vendor's real example, not by assuming. Note their
> example uses **TFT_eSPI** for graphics — but touch and display are independent
> halves, so we kept our **Arduino_GFX** display and only borrowed the touch code.

---

## Module 1 — Add the library

Libraries are per-project (like `requirements.txt`). In `platformio.ini`:

```ini
lib_deps =
	moononournation/GFX Library for Arduino
	lewisxhe/SensorLib          ; CST226SE touch driver (TouchDrvCSTXXX)
```

The next `pio run` downloads it automatically.

---

## Module 2 — SPI vs I²C (why touch is wired differently)

The display uses **SPI**: a fast bus with dedicated wires (clock, data-out,
data-in, chip-select) mostly for one device. The touch chip uses **I²C**: a
**two-wire** bus (`SDA` data + `SCL` clock) that **many chips share**, each
identified by an **address**. The CST226SE answers at `0x5A`.

That's why the touch setup looks different from the display: instead of naming
five dedicated pins, we start the shared bus (`SDA`, `SCL`) and address one chip.

---

## Module 3 — The code

### New pins + objects

```cpp
#include <Wire.h>              // the I2C library
#include <TouchDrvCSTXXX.hpp>  // SensorLib's driver

#define TOUCH_SDA  5
#define TOUCH_SCL  6
#define TOUCH_RST 13
#define TOUCH_IRQ 21

TouchDrvCSTXXX touch;          // a PLAIN object (note: no `new`, no `*`)
int16_t touchX[5], touchY[5];  // up to 5 simultaneous touch points
```

### Initialize it in `setup()`

```cpp
touch.setPins(TOUCH_RST, TOUCH_IRQ);
if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, TOUCH_SDA, TOUCH_SCL)) {
  Serial.println("ERROR: touch.begin() failed!");
}
touch.setMaxCoordinates(SCREEN_W, SCREEN_H);   // report coords in OUR screen size
touch.setHomeButtonCallback(onHomeButton);      // handle the home-button pad
```

### Read it in `loop()`

```cpp
uint8_t count = touch.getPoint(touchX, touchY, 5);   // returns #fingers (0 = none)
for (uint8_t i = 0; i < count; i++) {
  Serial.printf("touch %d: x=%d y=%d\n", i, touchX[i], touchY[i]);
  gfx->fillCircle(touchX[i], touchY[i], 5, RGB565_YELLOW);
}
```

### New C++ concepts in this module

- **Plain object vs pointer.** `TouchDrvCSTXXX touch;` creates the object
  *directly* (on the stack / as a global), so we call methods with a **dot**:
  `touch.begin()`. Compare the display: `Arduino_GFX *gfx = new ...;` created it
  with `new`, giving a **pointer**, so we used `gfx->begin()`. `->` was never
  magic — it's just "call a method *through a pointer*." Same idea as Python's dot
  either way; C++ only makes the pointer-vs-value distinction visible.
- **Fixed-size arrays.** `int16_t touchX[5];` reserves room for exactly 5 values
  up front — C++ wants the size known at compile time, unlike a Python list that
  grows on demand. `int16_t` = a 16-bit signed integer.
- **A callback function.** `onHomeButton` is a plain function we hand to the
  driver; it calls *us back* when the home pad is pressed. Same concept as passing
  a function as an argument in Python.

---

## Module 4 — Calibration by observation (the real lesson)

We didn't trust the dots blindly. We **printed raw coordinates over serial** and
touched known corners to learn how the chip's coordinate system maps to the
screen. Three corners are enough to pin down the full 2D transform:

| Physical corner | Reported (x, y) |
|---|---|
| Top-left | `66, 95` |
| Top-right | `159, 79` |
| Bottom-left | `66, 361` |

**Reading the map:**

- **Left → right:** `x` rose `66 → 159`, `y` ~unchanged ⇒ touch-X = screen-X, same direction.
- **Top → bottom:** `y` rose `95 → 361`, `x` ~unchanged ⇒ touch-Y = screen-Y, same direction.

That's an **identity mapping**: for *this* board in portrait (`rotation 0`), the
CST226SE already reports coordinates aligned with the display. So we need
**no `setSwapXY()` and no `setMirrorXY()`**. Confirmed by eye: dots land directly
under the fingertip and follow it everywhere.

> **Why this matters:** SensorLib exposes `setSwapXY(bool)` and
> `setMirrorXY(bool x, bool y)` precisely because different boards/rotations need
> different transforms. LilyGO's own example runs *landscape* (`rotation 1`) and
> therefore *does* set `setSwapXY(true)` + `setMirrorXY(false, true)`. Our
> portrait orientation needs neither — a concrete reminder that these settings are
> **orientation-specific**, and the honest way to find them is to measure.

### How to derive the transform yourself

1. Set `setMaxCoordinates(width, height)` to your screen size; set **no**
   swap/mirror yet.
2. Print raw `(x, y)`; touch top-left, top-right, bottom-left.
3. If moving **right** doesn't raise `x` (or raises the *other* axis) → you need a
   swap and/or mirror. Pick the settings that make right = +x and down = +y.

---

## Module 5 — Reading serial while touching (workflow note)

Same monitoring gotcha as Lesson 01 (`pio device monitor` needs a real TTY). We
captured touches by reading the port with the `read-serial.py` pyserial helper.

Two practical wrinkles we hit:

- **The DTR pulse resets the board.** The helper toggles DTR to catch the boot
  header, which *reboots* the board each time you start a capture. **Hold the
  corner through the reset** so your touch is still down when `loop()` starts.
- **Holding spams the log.** At ~100 polls/sec a 10-second hold prints hundreds of
  identical lines. Summarize with:
  ```bash
  ... read-serial.py /dev/ttyACM0 8 | grep 'touch 0' | sort | uniq -c | sort -rn
  ```
  which collapses a hold into one counted line per unique coordinate.

---

## Module 6 — Polling vs interrupt (and why GPIO 21 exists)

Our first working `loop()` **pure-polled**: it called `getPoint()` every ~10 ms
no matter what. The catch is that `getPoint()` is an **I²C transaction** — it
drives the bus and talks to the chip at `0x5A` — so an *idle* screen still ran
~100 pointless I²C conversations per second, each answered with "no touch."

The touch chip exposes a dedicated **IRQ pin on GPIO 21** ("interrupt request").
It's a doorbell: the chip drives that wire the instant a touch happens. Reading a
GPIO level is a *single CPU instruction* — no bus traffic. That one wire enables
three possible designs:

| Design | What it does | Idle cost | Latency | Complexity |
|---|---|---|---|---|
| **A. Pure polling** | I²C `getPoint()` every loop | High (I²C 100×/s) | ≤ one loop | Trivial |
| **B. IRQ-gated polling** | read the cheap IRQ pin first; I²C only when it's asserted | Low (a pin read) | ≤ one loop | Trivial+ |
| **C. True interrupt** | hardware jumps to an ISR on the pin edge; ISR sets a flag; `loop()` does the read | Lowest (CPU can sleep) | Near-instant | Real (ISR, `volatile`, races) |

### Why design C's ISR can't just read the touch

The "proper" event-driven form attaches an ISR to the pin
(`attachInterrupt(TOUCH_IRQ, isr, FALLING)`). But an **ISR must be tiny**: while
it runs, the system is paused, and — critically — **you cannot do I²C inside an
ISR** (I²C itself needs interrupts/time to clock bytes). So the pattern is: the
ISR only sets a `volatile bool` flag; `loop()` sees the flag and does the actual
`getPoint()` when it's safe. (`volatile` = "this can change behind your back in
the ISR, don't optimize away re-reads"; `IRAM_ATTR` puts the ISR in fast RAM.)

### What we chose, and why

We adopted **design B** — because SensorLib hands it to us for free. Its
`isPressed()` method, *when an IRQ pin was passed to `setPins()`*, is literally:

```cpp
// from TouchDrvInterface::isPressed() — SensorLib
if (irqPin != -1)
    return digitalRead(irqPin) == irqTriggerLevel;  // cheap pin read
return getTouchPoints().hasPoints();                // else: fall back to I2C
```

So our `loop()` gates the expensive read behind the cheap one, with **no new C++
concepts**:

```cpp
void loop() {
  if (touch.isPressed()) {                 // cheap: reads GPIO 21
    uint8_t count = touch.getPoint(touchX, touchY, 5);   // I2C: only when touched
    for (uint8_t i = 0; i < count; i++) { /* print + draw */ }
  }
  delay(10);
}
```

A nice bonus: `isPressed()` knows the *polarity* (`irqTriggerLevel`), so we never
had to figure out whether "pressed" means the pin goes high or low.

**Verified on hardware:** taps still register and draw correctly, but a held
touch now prints only a few frames instead of the hundreds pure-polling produced
— visible proof we stopped reading the bus when nothing's happening.

### The decision, recorded

- **Now (design B):** best effort-to-benefit ratio; removes idle bus spam with
  zero added complexity. This is what the firmware ships.
- **Deferred (design C):** true interrupt + CPU sleep. The payoff is **power** —
  it lets the CPU sleep and wake only on a real touch — so we'll tackle it as part
  of a dedicated **low-power lesson** once the battery/PMU (SY6970) is in play.
  Until then, on USB power, the extra ISR complexity isn't worth it.

---

## Module 7 — Making it fun, and fixing a nasty freeze

The bare dots-on-touch demo had two problems that made it *clunky*: it froze
after ~15 touches (with no serial monitor attached), and there was no way to
clear the screen. Fixing both turned into one of the most instructive parts of
the whole lesson.

### The freeze — a debugging story worth its own section

**Symptom:** after ~15 touches the whole thing froze — but *only* when no serial
monitor was attached. With a monitor open it ran forever.

**The clue that cracked it:** clearing the screen didn't restore responsiveness.
If the slowdown were caused by the *number of dots drawn*, wiping them would
speed it back up. It didn't — so the cause wasn't on the screen at all. It was
something that **accumulates over time regardless of what's drawn**: the
**serial output buffer**.

**What's actually happening.** Every touch, we call `Serial.printf(...)`. On the
ESP32-S3's **native USB**, if the TX buffer fills and nothing is *draining* it,
`Serial.print` **blocks** — the whole `loop()` waits. When a monitor is attached
it constantly drains the buffer, so you never notice. Standalone, the buffer
fills and the loop stalls.

**Fix attempt 1 — `if (Serial)` (necessary, but NOT sufficient).** The idea:
only print when a host is listening. But here's the native-USB gotcha —
`if (Serial)` is true whenever the port is **USB-connected**, and the OS asserts
that on plug-in *even when no app is reading the port*. So `Serial` still read
"true," we still printed, bytes still piled up unread, and each write stalled a
little longer → the **growing lag** we saw. The guard is still worth keeping (it
skips the string-formatting work when truly disconnected), but it doesn't solve
the stall.

**Fix attempt 2 — make writes non-blocking (the real fix):**

```cpp
Serial.setTxTimeoutMs(0);   // in setup(), right after Serial.begin()
```

This tells the USB CDC driver to **drop** output when the buffer is full instead
of **waiting** for room. The loop can never stall on serial again — whether or
not anyone is listening. Verified: smooth and constant past 50+ touches.

> **The tradeoff, stated honestly:** with a 0 ms timeout, debug lines are
> *best-effort* — if you attach a monitor and touch very fast, some coordinate
> lines get dropped. That's the correct bargain for a UI: **never let debug
> output block the app.** (Alternative if you must not drop lines: only print
> when truly connected, throttle the rate, or check `Serial.availableForWrite()`
> before printing.)

**Takeaway:** on native-USB ESP32-S3/C3, treat `Serial` as a device that might
never be drained. Guard *and* make it non-blocking. This is a footgun on *every*
native-USB board, not just this one.

### The Clear button — your first taste of hit-testing

We chose a clean on-screen **CLEAR** button (a blue bar across the bottom) over a
physical button or auto-expiring dots — it needs no extra hardware and teaches
the idea every GUI is built on: **hit-testing.** A "button" is just a rectangle
we drew; a touch "presses" it when its coordinates fall inside that rectangle.

```cpp
#define BTN_X 0
#define BTN_Y 430
#define BTN_W 222
#define BTN_H 50

// Is the touch inside the button's rectangle?
bool insideClearButton(int16_t x, int16_t y) {
  return x >= BTN_X && x < BTN_X + BTN_W &&
         y >= BTN_Y && y < BTN_Y + BTN_H;
}
```

In `loop()`, each touch either presses the button or draws a dot:

```cpp
if (insideClearButton(x, y)) {
  if (!wasPressed) {            // edge-triggered: fire once per tap
    drawClearButton(true);      // flash for feedback
    delay(60);
    clearScreen();              // wipe, then repaint title + button
  }
} else {
  gfx->fillCircle(x, y, 5, RGB565_YELLOW);   // continuous: dragging draws a trail
}
```

### New C++ / embedded concepts in this module

- **Helper functions.** We split the UI into small functions (`drawTitle`,
  `drawClearButton`, `clearScreen`, `insideClearButton`) — same modularity habit
  as Python, just with explicit return types (`void`, `bool`).
- **The ternary `?:`** — `pressed ? A : B` is shorthand for an if/else that
  yields a value. Python's `A if pressed else B`.
- **Edge detection with a state variable.** `wasPressed` remembers last loop's
  state so the button fires **once per tap** (on the press *edge*) instead of
  repeatedly while the finger rests. Dots, by contrast, we *do* want continuous —
  which is why dragging leaves a trail. Recognizing "edge vs level" is a core
  input-handling skill.
- **`clearScreen()` repaints the fixed UI.** Because we draw straight to the panel
  (immediate mode), "clear" means fill black then redraw the title and button —
  there's no saved scene to restore. (This limitation is exactly what the planned
  framebuffer upgrade removes.)

### Why this isn't the *whole* answer — and what's deferred

An on-screen button is functional but blunt. The delightful version — dots that
**fade or expire** into a living trail — needs a **retained framebuffer** (a
saved copy of the scene we can redraw every frame). We deliberately deferred that
to a dedicated **framebuffer stage** (`Arduino_Canvas` in the 8 MB PSRAM), which
also sets up LVGL. See `docs/PLAN.md`.

---

## What you built and learned

- ✅ Added SensorLib; brought up the CST226SE over I²C
- ✅ Understand SPI (display) vs I²C (touch) and how each is initialized
- ✅ Plain object + `.` vs pointer + `->`; fixed-size arrays; a callback
- ✅ Multitouch read loop drawing at the exact touch point
- ✅ **Derived an orientation transform from measured corner data** (identity here)
- ✅ Evaluated **polling vs interrupt** and adopted IRQ-gated polling (design B)
- ✅ Built an on-screen **Clear button** with **hit-testing** + edge detection
- ✅ Diagnosed & fixed a **native-USB serial freeze** (non-blocking `Serial`)

## Command cheat-sheet

```bash
pio run                       # build only
pio run -t upload             # build + flash
# capture + summarize touches (avoids the monitor TTY issue):
~/.platformio/penv/bin/python \
  ~/.claude/skills/esp32-board-bringup/references/read-serial.py /dev/ttyACM0 8 \
  | grep 'touch 0' | sort | uniq -c | sort -rn
```

## Glossary

- **I²C** — two-wire shared bus (SDA data + SCL clock); each chip has an address.
- **Address (`0x5A`)** — which chip on the I²C bus we're talking to.
- **IRQ / interrupt pin** — a line the chip pulses to say "something happened."
- **Callback** — a function you hand to a library for it to call back on an event.
- **Swap / mirror** — coordinate transforms mapping raw touch axes to screen axes.
- **Identity mapping** — the case where no transform is needed (raw = screen).
- **Polling** — repeatedly asking a device "anything new?" on a schedule.
- **Interrupt** — the device signals the CPU on an event, instead of being asked.
- **ISR** — Interrupt Service Routine; the small function that runs on the event.
- **`volatile`** — marks a variable that can change outside normal code flow (e.g. in an ISR), so the compiler always re-reads it.
- **Hit-testing** — checking whether a touch's coordinates fall inside a shape's bounds (how buttons work).
- **Edge vs level** — reacting to a *change* of state (press edge) vs the ongoing state (finger held); buttons want edges, drawing wants level.
- **Immediate vs retained mode** — drawing straight to the panel (no saved scene) vs keeping a framebuffer you redraw; retained mode makes clear/fade/animation easy.
- **Non-blocking I/O** — an output that *drops* when it can't proceed instead of *waiting*, so it can't stall the program (`Serial.setTxTimeoutMs(0)`).

## Next lesson

**Stage 3 — a small interactive app:** draw an on-screen button, hit-test touches
against its bounds, and react (change color / count taps). That combines this
lesson's input with Lesson 01's output into real event-driven behavior — the last
step before adopting **LVGL** for a full widget UI.
