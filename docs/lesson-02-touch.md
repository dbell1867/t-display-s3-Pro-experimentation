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

## Module 8 — Why fast drags leave gaps (a performance investigation)

A sharp observation kicked this off: **the faster you drag, the wider the space
between dots.** Slow drags draw a near-solid line; quick swipes leave a dotted
trail. That's a symptom worth chasing, because it teaches how to *measure* a
system before optimizing it.

### The mental model: we're sampling a continuous motion

Your finger moves *continuously*, but `loop()` only looks at it at discrete
moments — one snapshot per trip through the loop. Each dot is one snapshot. The
spacing between snapshots obeys a simple rule:

```
gap distance  =  finger speed  ×  time between samples
```

Notice **finger speed** is in there. The *time between samples* is roughly
constant, so a faster finger covers more ground between two snapshots → wider
gaps. Nothing is "lagging behind"; we're just taking too few pictures of a
fast-moving subject. That gives us two independent levers:

| Lever | What it does | Ceiling |
|---|---|---|
| **A. Sample more often** (shrink time between samples) | more dots, closer together | a hardware wall (below) |
| **B. Connect the samples** (draw a *line* between them) | continuous trail at any speed | basically none — it's cheap |

### Measure first: turn the display into an instrument

The honest way to size lever A is to measure our real sample rate. Serial was
being flaky, so we used a channel that *always* works on this board — **the
display itself**. We timestamped each accepted touch frame with `micros()` and,
every 30 frames, painted the average interval and implied rate in the corner:

```cpp
uint32_t now = micros();
if (lastSampleUs != 0) {
  uint32_t dt = now - lastSampleUs;
  if (dt < 200000) {                 // ignore gaps > 200 ms (a real finger-lift)
    dtSumUs += dt;
    if (++sampleCount >= 30) {
      float avgMs = (dtSumUs / 30.0f) / 1000.0f;
      drawStats(avgMs, 1000.0f / avgMs);   // cyan readout on the glass — no serial needed
      /* reset accumulators */
    }
  }
}
lastSampleUs = now;
```

> **Lesson inside the lesson:** when one debug channel fights you, use another.
> The screen is a perfectly good place to print numbers.

### A bug in the instrument — and what it revealed about the IRQ

The first version showed *nothing*: dots drew fine, but the readout never
appeared. The cause taught us something real about the hardware. We'd written:

```cpp
if (!pressed) lastSampleUs = 0;   // reset the timer when "not pressed"
```

But recall from Module 6 that `isPressed()` reads the **IRQ line (GPIO 21)**, and
that line only *pulses* when the controller has a fresh frame — it goes
**inactive between frames even while your finger is down**. So `pressed`
flickered false between every frame, wiping our timing anchor each time, so every
frame looked like "the first of a new drag" and the counter never advanced.

The fix was to time consecutive *accepted* frames and ignore only genuine lifts
(gaps > 200 ms), rather than trusting the flickering `pressed` flag. **Takeaway:
the touch IRQ is a brief pulse, not a steady "is-touched" level.** Hold that
thought.

### The numbers

With the instrument working, we swept the loop's `delay()` and read the rate off
the screen during a medium-speed drag:

| Loop `delay` | Sample rate | Interval |
|---|---|---|
| `delay(10)` (original) | **16 Hz** | ~62 ms |
| `delay(2)` | **40 Hz** | ~25 ms |
| `delay(0)` (spin) | **~77 Hz** | ~13 ms |

Two findings:

1. **The rate was flat across drag speeds** (15 / 16 / 16 Hz slow/medium/fast at
   `delay(10)`). That confirms the gaps are pure geometry — the loop samples at a
   fixed rate regardless of how fast you move. Lever B is the right fix.
2. **The rate kept climbing as we polled faster** — no clean plateau. So the
   original bottleneck was *our own loop cadence*, not a single hard controller
   wall. And the fact that faster polling caught *more* frames confirms the IRQ is
   a short pulse we were partly **missing** with a slow loop.

### Why lever A alone can't win

Convert rate into what the eye sees (~9 px/mm on this panel). For a **fast swipe**
(~300 mm/s):

| Rate | Gap between dot centres |
|---|---|
| 16 Hz | ~56 px (badly broken) |
| 40 Hz | ~22 px |
| 77 Hz | **~12 px — still a visible gap** with radius-5 dots |

So **even at the ceiling, a quick flick still gaps.** Polling faster helps
enormously, but geometry always catches up. That's the proof we need lever B.

### The tradeoff on cranking the delay to zero

`delay(0)` buys the best rate but the core **never sleeps** — it spins at 100%
between frames. Fine on USB power, bad on a battery. So "just poll flat out" isn't
free; it trades power for responsiveness.

And here's the payoff to a decision from Module 6: the reason faster polling
caught more frames is that we were *missing IRQ pulses*. A **true hardware
interrupt (design C)** — `attachInterrupt` on the IRQ edge — catches **every**
pulse regardless of loop timing, giving us the full frame rate **and** letting the
CPU sleep between touches. We deferred design C thinking it was only about power;
it turns out to also be the "right" way to get the sample rate. Another reason it
earns its own low-power lesson later.

### The decision

- **Fix (planned): interpolation (lever B)** — draw a line from the previous
  point to the current one, starting a fresh stroke on each new touch. Makes the
  trail continuous at any speed, cheaply. Tracked in `docs/PLAN.md` as Stage 2c.
- **Sample rate:** a smaller `delay` is a real, easy win (16 → 40 Hz), so we'll
  pick a balanced value rather than the power-hungry `delay(0)`. The *full* answer
  (design C) waits for the low-power lesson.

> **Method takeaway:** we *measured before optimizing*, built an instrument when
> the obvious one failed, found a bug that taught us how the hardware really
> behaves, and proved which lever actually solves the problem. That sequence —
> observe → measure → diagnose → choose the fix — is the whole game.

---

## Module 9 — Implementing the smooth trail (Stage 2c)

With the diagnosis from Module 8 in hand, the fix was **interpolation**: connect
consecutive touch reports instead of drawing isolated dots.

### The interpolation helper

```cpp
#define DOT_RADIUS 3   // brush thickness

// Draw a continuous stroke from (x0,y0) to (x1,y1) by placing a dot every ~2 px
// along the straight line between them.
void drawStroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  int16_t dx = x1 - x0, dy = y1 - y0;
  int16_t span  = max(abs(dx), abs(dy));   // pixels the finger jumped
  int16_t steps = span / 2; if (steps < 1) steps = 1;
  for (int16_t i = 0; i <= steps; i++) {
    int16_t x = x0 + (int32_t)dx * i / steps;   // fraction i/steps along the line
    int16_t y = y0 + (int32_t)dy * i / steps;
    gfx->fillCircle(x, y, DOT_RADIUS, RGB565_YELLOW);
  }
}
```

That's **linear interpolation**: the point a fraction `i/steps` of the way from
the start to the end is `start + (end − start) × i/steps`, done separately for x
and y. We synthesize the samples the hardware never gave us.

> **A real C++ gotcha:** `dx * i` can exceed a 16-bit range (a 480-px jump × 240
> steps ≈ 115 000, but an `int16_t` maxes at 32 767). The cast `(int32_t)dx * i`
> promotes the whole calculation to 32-bit so it can't overflow. In Python
> integers grow without limit; in C++ you must *choose* a type wide enough.

### The same IRQ bug, a second time

The first attempt drew *only isolated dots* — no connecting line. The cause was
**the exact pulse behaviour from Module 8**, biting again. This code looked
reasonable:

```cpp
if (touch.isPressed()) { /* ... draw ... haveStroke = true; */ }
else haveStroke = false;      // "finger lifted → start a new stroke"
```

But `isPressed()` reads the IRQ, which goes **inactive between frames even while
touching** — so `pressed` flickered false constantly, the `else` wiped
`haveStroke` between *every* frame, and each accepted frame took the "first point,
draw a single dot" branch. Never interpolated. **Lesson learned twice: don't use
the flickering `pressed` flag to decide "same touch or new touch."**

### The fix: time the gap, don't trust the flag

We decide continuation by **elapsed time between accepted frames** instead:

```cpp
#define STROKE_TIMEOUT_MS 200

uint32_t now = millis();
bool sameSession = (now - lastTouchMs) < STROKE_TIMEOUT_MS;   // same finger-down?
// ... use sameSession to continue-or-restart the stroke ...
lastTouchMs = now;
```

During a drag, frames arrive ~25 ms apart (< 200 ms) → same session → interpolate.
Lift your finger and the frames stop; the next touch is > 200 ms later → new
session → fresh stroke, no line dragged across the screen. As a bonus, the same
`sameSession` flag makes the **CLEAR button fire exactly once per press** (only on
a *new* press that lands inside it), so we no longer need a separate edge flag.

### The result

- Smooth continuous strokes at **any** finger speed — the gaps are gone.
- `delay(2)` (~40 Hz) keeps it responsive without spinning the CPU flat out.
- The measurement scaffolding (on-screen Hz readout, timing globals) is removed;
  `src/main.cpp` is clean and snapshotted to `docs/lesson-02-touch/main.cpp`.

> **Meta-lesson:** the same hardware fact — *the touch IRQ is a pulse, not a
> level* — caused two different bugs (the measurement instrument, then the stroke
> logic). Understanding the hardware once paid off twice. And the robust pattern
> for "is this the same continuous gesture?" is a **timeout**, not a level flag.

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
- ✅ **Measured the touch sample rate** (display-as-instrument) and learned why
     fast drags gap — pointing to interpolation as the real fix (Stage 2c)
- ✅ **Implemented a smooth trail with linear interpolation**, using a *timeout*
     (not the flickering IRQ flag) to track a continuous stroke

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
- **Sample rate** — how many times per second we read the input; sets the *time between samples* and therefore the dot spacing (`gap = speed × interval`).
- **Interpolation** — filling the space *between* samples with geometry (e.g. drawing a line between two touch points) so the result looks continuous regardless of sample rate.

## Next lesson

**Stage 3 — a small interactive app:** draw an on-screen button, hit-test touches
against its bounds, and react (change color / count taps). That combines this
lesson's input with Lesson 01's output into real event-driven behavior — the last
step before adopting **LVGL** for a full widget UI.
