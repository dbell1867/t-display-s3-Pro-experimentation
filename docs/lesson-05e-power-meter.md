# Lesson 05e — The power meter: measuring what we assumed

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Put an **inline USB power meter** on everything Stages
5b–5d optimised, and find out how much of it mattered. Answer: less than we
thought, and the biggest single win was a command we'd never sent.

> Continues from [05b](lesson-05b-low-power.md) (light sleep),
> [05c](lesson-05c-backlight.md) (backlight) and [05d](lesson-05d-deep-sleep.md)
> (deep sleep). Those lessons measured **loops/sec**. This one measures **milliamps**.

---

## Learning objectives

By the end of this lesson you can:

1. Explain what an inline USB meter **actually measures** — and why a charging
   battery makes every reading meaningless.
2. Build a **power bench**: hold one power state still so a slow meter can settle.
3. Measure by **differences**, not absolutes.
4. Put the **display controller** to sleep, not just the backlight.
5. Recognise when the remaining draw is **not reachable from firmware**.
6. Distinguish a **hypothesis** from a **measurement** when debugging.

---

## Module 1 — What the meter is really measuring

```
USB 5V ──[ meter ]──> VBUS ──> SY6970 ──┬──> 3.3V rail ──> ESP32 + display
                                        └──> battery charge current
```

Three consequences, all of which will bite you:

1. **Charge current is included.** Our first attempt read `I 200mA` of charging —
   roughly 10× everything we were trying to measure. Every reading was noise until
   the battery was full (`I` = 0) or disconnected. *You cannot measure a load
   through a power path that has another load on it.*
2. **The meter reads 5 V; the chip runs at 3.3 V.** Don't compare the two. Compare
   bench readings to each other.
3. **Cheap meters average over ~0.5–1 s.** Our 1 s light-sleep wake cycle shows as
   a blend, not a peak. That's fine — the average is what drains a battery.

Even at "Charged" the SY6970 runs periodic **top-off cycles**, so the meter jumps
occasionally for no visible reason. Disconnecting the battery is the cleanest bench.

> **Why dev boards have a current-sense jumper.** Serious low-power boards put a
> jumper on the 3.3 V rail specifically to get *downstream* of the PMU. Now you know
> what it's for.

---

## Module 2 — The power bench

Normal firmware hides power states behind timers, and (from 5b) *refuses to
light-sleep on USB* — exactly when a USB meter can see it. So measurement needs a
mode that **holds one state still**:

```cpp
enum BenchMode : uint8_t {
  BENCH_OFF = 0,
  BENCH_BL_FULL,      // A: busy-poll CPU + backlight 100%
  BENCH_BL_OFF,       // B: busy-poll CPU + backlight off
  BENCH_LIGHT_SLEEP,  // C: light sleep    + backlight off
  BENCH_DISP_SLEEP,   // D: light sleep    + ST7796 SLPIN
  BENCH_DEEP_SLEEP,   // E: deep sleep, TIMER wake only
  BENCH_COUNT
};
```

Two design details that make it usable:

- **Announce, then go dark.** Each mode shows its name at full brightness for 4 s
  before settling into the state being measured. The screen is both the instrument
  *and* the biggest load — it has to tell you the mode, then get out of the way.
  (Same lesson as 05c's `idleCpu` capture: the instrument shares fate with the
  screen you're turning off.)
- **Any touch advances.** In modes B–E the screen is dark, so there's no visible
  button. We poll the controller directly rather than going through LVGL.

Mode E arms **only the timer** — no EXT1 — so nothing can wake it early and you get
a clean 20 s window. That was deliberate: it let measurement proceed while a
touch-wake bug was still unsolved. *Don't let a broken diagnostic block data
collection.*

---

## Module 3 — The numbers (and the four times the apparatus lied)

The first run looked clean and produced a confident, **wrong** conclusion. Getting
to trustworthy numbers took four corrections — and every one of them was the
measurement setup, not the board.

### The final, reproducible figures

Powered from a **dumb charger** (no USB host), **charging disabled** for the run:

| State | Meter | Δ = cost of… |
|---|---|---|
| A: poll + BL 100% | 94–120 mA ⚠ | |
| B: poll + BL off | 74 mA | backlight ≈ 18–20 mA ⚠ |
| C: light sleep | 41 mA | **busy CPU 33 mA** |
| D: + display off | 30 mA | **ST7796 11 mA** |
| E: + touch/ALS off | 29 mA | touch + ALS ≈ 1 mA |
| **F: deep sleep, all off** | **27 mA** | rest of chip ≈ 2 mA |

**≈71% of the draw is reachable from firmware.** The floor is **27 mA** — a
plausible figure for regulator quiescent, the status LED, panel bias and sleeping
I²C parts. No mystery requiring a schematic.

⚠ **The high-current rows are not reproducible.** Mode A measured 94 and then
120 mA on *identical* settings. Likely mechanism (hypothesis, untested): the SY6970
is a **power-path** chip with an input current limit, and near that limit the
**battery supplements** the load. The meter reads *input* current, which equals the
load only while the battery is neither supplementing nor absorbing. Below ~40 mA
every reading reproduced exactly; above ~90 mA they wander. So: **trust the
sleep-state numbers, treat the backlight as ≈18–20 mA, and don't quote A.**

### The four artifacts

1. **Battery charge current** (~35 mA, variable). The meter sits *upstream of the
   PMU*, so charging is included, and even a "full" battery top-off cycles at
   unpredictable moments. Detected because `A − B` — a fixed difference between two
   pinned backlight duties — came out 18, then 6, then 20. Fixed with
   `PPM.disableCharge()` for the duration of the bench.
2. **The USB host link** (~26–35 mA). The S3's USB PHY stays powered and the
   enumerated link stays alive, *including through deep sleep*. Present in every
   early reading, and it looked exactly like an immovable hardware floor. Fixed by
   powering from a dumb charger. **This was larger than the backlight.**
3. **The instrument's own I²C polling** (31 mA) — mode E kept calling
   `touch.getPoint()` on a controller it had just put to sleep. See Module 5.
4. **A defeated sleep** (also mode E) — the level-triggered wake fired instantly on
   a line that was already low, so the CPU never actually slept.

### What the first run got wrong

The original conclusion — *"the backlight isn't dominant, deep sleep saves 2 mA,
and 53% of the draw is unreachable from firmware"* — was an artifact. That 53%
"unreachable floor" was mostly a USB host and a charging battery. Corrected: the
**busy CPU is the largest firmware-controllable load** (33 mA), the backlight is
second (~18–20), the display controller third (11), and the real floor is 27 mA.

One conclusion did survive: **loops/sec cannot see a load that isn't the CPU.**
Stages 5b–5d measured the right thing for what they controlled, but had no way to
see the display controller, the charger, or the USB link.

### On the RESET floor (a measurement that misleads)

An early reading held RESET down (66 mA, on the PC) and I called it "the number no
firmware change can ever beat." **Wrong.** Holding RESET stops the *ESP32*; every
other chip keeps whatever state firmware last left it in — the ST7796 was still
scanning. It isn't a hardware floor, it's "CPU halted, peripherals running", and
mode D (CPU alive, display asleep) beats it. **A command sent before sleeping can
beat halting the processor.**


---

## Module 4 — The win: sleep the display, not just the light

The backlight is a **PWM pin**. The display controller is a **separate chip that
needs an actual command** (`SLPIN`, 0x10). We'd been doing one and not the other
since 05c — leaving a third of the screen's cost on the table.

```cpp
static void applyScreenPower(bool idle) {
  if (idle) {
    setBacklight(0);                       // dark FIRST, so SLPIN's teardown isn't seen
    if (!dispAsleep) { panel->displayOff(); dispAsleep = true; }
  } else {
    if (dispAsleep) {
      panel->displayOn();
      dispAsleep = false;
      lv_obj_invalidate(lv_screen_active());     // SLPIN discarded the frame buffer
      lv_refr_now(lv_display_get_default());     // redraw BEFORE lighting up
    }
    setBacklight(autoBrightness);
  }
}
```

**Order matters in both directions**: go dark before `SLPIN`; redraw before the
backlight returns, or you light up on garbage.

---

## Module 4b — One confusion, three bugs

Shipping `SLPIN` exposed something that had been wrong since Stage 1: **we had been
treating "the backlight" and "the display" as the same thing.** They aren't. The
backlight is an LED on a PWM pin. The display is a separate chip with its own power
state and its own memory. That single conflation produced three different symptoms:

| Symptom | Cause |
|---|---|
| 9 mA wasted while "off" | backlight off, controller still scanning |
| White noise on **wake** | lit up before the repaint finished |
| White noise on **power-on** | lit up before `panel->begin()` had even run |

The wake bug is instructive because my first fix was subtly wrong. I ordered it so
the repaint would finish before the backlight rose — but that's a **race**, and it
depends on redraw time (~40 ms for a full 222×480 frame over non-DMA SPI). `SLPOUT`
restores the panel showing whatever is in the controller's **GRAM**, which after a
sleep cycle is random. The fix isn't to outrun the garbage, it's to **erase it**:

```cpp
panel->displayOn();                          // SLPOUT + 120 ms settle (driver does the wait)
panel->fillScreen(RGB565_BLACK);             // wipe the random GRAM — still dark
lv_obj_invalidate(lv_screen_active());
lv_refr_now(lv_display_get_default());       // repaint the real UI — still dark
setBacklight(autoBrightness);                // only NOW illuminate
```

> **When correctness depends on A finishing before B, prefer eliminating the bad
> state over sequencing around it.** Ordering is a race; erasing is a guarantee.

The power-on bug was the same mistake, one lesson earlier. `setup()` did:

```cpp
setBacklight(autoBrightness);   // light ON
panel->begin();                 // ...then initialise the panel. Noise, for ~1 s.
```

Fixed by holding the backlight at 0 through the whole bring-up, clearing to black
right after `panel->begin()`, and raising the light only after `lv_refr_now()` puts
a real first frame on the panel.

**The rule, now applied in all three places: never illuminate a buffer you haven't
drawn.**

One honest tradeoff: the screen is now *black* for the ~1 s boot rather than showing
noise. Blank reads as broken; noise at least reads as alive. Real products cover
this with a **splash screen** drawn immediately after `fillScreen`, before the slow
peripheral init — worth adding if the dead second bothers you.

---

## Module 5 — Two bugs, and how they were finally found

**A layout bug that hid the evidence.** The new Bench button was placed at
`BOTTOM_MID -60`, landing exactly on `bootLabel` at y=378 — the wake-reason readout
we needed. LVGL's `lv_obj_align` places widgets at absolute offsets with **no
collision detection**: nothing warns you, the button just draws on top and the
loser is invisible. Hand-tuning y-offsets across eight widgets is a sign the screen
has outgrown manual alignment; LVGL's flex/grid containers would have caught it.

**The deep-sleep wake bug — solved, but not by guessing.** Tapping Sleep woke
instantly with `woke: touch`. Three hypotheses, three failures:

1. *Touch controller heartbeat.* Rewrote the guard to watch the **pin** rather than
   `getPoint()` (EXT1 watches the line, not fingers) and drain the controller.
   Result: `IRQ settled in 500 ms` — the function's own minimum. **No chatter at
   all.** Theory dead.
2. *RTC pad never initialised.* `rtc_gpio_pullup_en()` has no effect until
   `rtc_gpio_init()` switches the pad to its RTC function. Added. **Still woke.**
3. *RTC_PERIPH powered down + floating touch RESET.* Internal pull-ups live in a
   domain deep sleep switches off (`esp_sleep_pd_config(..., ESP_PD_OPTION_ON)`),
   and a floating `TOUCH_RST` lets the CST226SE reset and assert IRQ. Added both.
   **Still woke.**

All three are correct, necessary fixes for any working deep-sleep-on-touch, and all
are in the code. None was *the* bug.

**What actually found it was a power measurement.** Mode E (touch controller
asleep) read 60 mA — *above* mode D, which has strictly less turned off. That's the
signature of a CPU that never sleeps. Our light-sleep wake is level-triggered on the
same line:

```cpp
gpio_wakeup_enable((gpio_num_t)TOUCH_IRQ, GPIO_INTR_LOW_LEVEL);
```

Sleeping on the timer alone dropped E to **29 mA**, below D — proving the line sits
**LOW** whenever the controller isn't being serviced.

**Root cause:** the CST226SE asserts IRQ when it has a report and **holds it low
until someone reads it**. While we're awake we poll constantly, draining every
report, so the line looks perfectly quiet — which is why the guard in attempt 1
honestly measured 500 ms of calm. The instant we stop polling in order to sleep, the
next event asserts IRQ with nobody to clear it, the line latches low, and `ANY_LOW`
fires immediately.

Every hypothesis assumed something was *pulling* the line down. It was simply never
being *released*. The same mechanism retroactively explains 5b's "residual wake
creep" and mode E in one stroke.

> **The lesson:** after three hypothesis-driven misses, the answer came from an
> unrelated measurement that didn't fit. Pattern-matching against known gotchas is
> fast but has no stopping condition; a number that contradicts the model is worth
> more than another plausible theory. An "impossible" reading (E above D) is a gift.

**Consequence for the design:** a touch controller is a poor deep-sleep wake source,
because it holds its interrupt asserted precisely when nobody is listening. A
physical button is a clean line with nothing holding it — which is the next stage.

---

## Module 6 — Practical hazards

- **Screen auto-off fights measurement.** 05c blanks the screen after 5 s, making
  it impossible to *watch* a slow value like charge current tapering. Fix: gate it
  on `!onUsbCached`, the same rule that already gated CPU sleep. Power policy that
  ignores the power source is just an annoyance — phones dim on battery, stay lit
  while charging.
- **Every sleep mode makes the board harder to flash.** A sleeping CPU isn't
  servicing USB CDC: the port stays enumerated but goes silent, and upload fails
  with `OSError: [Errno 71] Protocol error`. Recovery is **RESET**, or **BOOT +
  RESET** for the ROM bootloader. A "flash window" at the top of `setup()` — a
  couple of seconds before any sleeping is allowed — prevents it.
- **Measure on a dumb charger, not your PC.** The USB host link costs more than the
  backlight on this board, and it hides in every reading taken over a dev cable.
- **Disable charging for the run** (`PPM.disableCharge()`), and re-enable on exit.
  A "full" battery still top-off cycles, and it lands mid-reading.
- **Sanity-check with a difference that shouldn't move.** `A − B` is two pinned
  backlight duties; if it isn't the same every run, something uncontrolled is
  moving and no other number in the set can be trusted yet.

---

## What you built and learned

- ✅ Understood what an inline USB meter measures — and why **charge current** ruins it
- ✅ Built a **power bench** that holds states still for a slow instrument
- ✅ Measured by **differences**; put real mA on Stages 5b–5d
- ✅ Found and removed **four measurement artifacts** — charge current, the USB host
  link, the instrument's own I²C polling, and a defeated sleep
- ✅ Established the real budget: **busy CPU 33 mA > backlight ~18–20 > ST7796 11**,
  floor **27 mA**, ≈71% reachable from firmware
- ✅ Shipped **`SLPIN`** into the idle path — 11 mA we'd been leaving on the table
- ✅ Fixed white noise on **wake** and on **power-on** — the same conflation, twice
- ✅ Learned to **erase a bad state rather than sequence around it** (guarantee > race)
- ✅ **Solved the deep-sleep wake bug** — via a power reading that contradicted the model
- ✅ Learned that **a measurement you haven't controlled is a hypothesis with a
  number attached**

## Command cheat-sheet

```bash
pio run -d ~/Work/Micro/tdisplay -t upload
# Upload fails with "Errno 71 Protocol error"? The board is asleep and not
# servicing USB. Tap RESET, or hold BOOT + tap RESET for the ROM bootloader.
```

## Glossary

- **Inline power meter** — a meter in series with the USB supply; reads everything downstream.
- **Power bench** — a mode that holds one power state still long enough to measure.
- **`SLPIN` (0x10) / `SLPOUT` (0x11)** — ST7796 sleep-in/out; each needs ~120 ms to settle.
- **GRAM** — the display controller's own frame memory; random after a sleep cycle or power-on.
- **`ESP_PD_DOMAIN_RTC_PERIPH`** — the power domain holding RTC pad pull-ups.
- **`rtc_gpio_init`** — switches a pad from its digital function to its RTC function.
- **Bisecting** — narrowing a bug by controlled elimination rather than by guessing causes.
- **Power path / supplement mode** — a PMU that lets the battery help supply the load;
  input current then no longer equals the load.
- **Measurement artifact** — a reading caused by the apparatus rather than the subject.

## Next lesson

**Button wake** (GPIO 0/12/16). Module 5 showed a touch controller is a poor
deep-sleep wake source — it holds its interrupt asserted exactly when nobody is
listening. A physical button is a clean line with nothing holding it, it's
RTC-capable on the S3, and it doubles as the first of the board's remaining
peripherals. After that: the SD card (SPI CS 14), or an LVGL flex/grid refactor.
