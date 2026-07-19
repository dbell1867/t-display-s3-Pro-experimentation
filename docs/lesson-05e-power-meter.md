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

## Module 3 — The numbers

Battery full (`I` = 0), meter inline on USB:

| State | Meter | Δ = cost of… | Share |
|---|---|---|---|
| A: poll + BL 100% | 129 mA | | |
| B: poll + BL off | 111 mA | **backlight** = 18 mA | 14% |
| C: light sleep | 80 mA | **busy CPU** = 31 mA | 24% |
| D: + display off | 71 mA | **ST7796 scanning** = 9 mA | 7% |
| E: deep sleep¹ | 78 mA | rest of chip ≈ 2 mA | 1.5% |
| RESET held | 66 mA | ESP32 running at all ≈ 12 mA | |
| **unexplained floor** | **~69 mA** | | **53%** |

¹ E measured with the display still awake, hence above D.

### What this overturned

- **The backlight does not dominate.** Received wisdom on ESP32 boards is "the
  screen is everything." Here a busy CPU (31 mA) costs nearly twice the backlight
  (18 mA). The prediction going in was the opposite.
- **Deep sleep saves 2 mA over light sleep** on this board — and is *12 mA worse
  than simply holding the chip in RESET*. Deep sleep isn't free: the RTC domain,
  the RTC timer, and the pull-up domain we keep powered for touch-wake
  (`ESP_PD_DOMAIN_RTC_PERIPH`) all cost. We bought wake-on-touch, and 12 mA is its
  price. A real tradeoff, visible only because we measured.
- **Over half the draw is not reachable from our code.** Stages 5b–5d were honest
  about what they controlled, but they optimised ~47% of the problem while 53% sat
  unexamined. **Loops/sec cannot see a load that isn't the CPU.**

### On the RESET floor

Holding RESET stops the **ESP32** — but every other chip keeps whatever state
firmware last left it in. The ST7796 was initialised and still scanning. So 66 mA
is *not* a hardware floor; it's "CPU halted, peripherals still running." Firmware
can beat it: mode D (71 mA) has the CPU alive and the display asleep, which is why
D and RESET are so close. **A command sent before sleeping can beat halting the
processor.**

The remaining ~69 mA is too much for a power LED and regulator quiescent draw.
Something on this board isn't understood yet. Saying so is better than inventing
an explanation — the honest next step is the schematic and a meter on the 3.3 V
rail, not more guessing.

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

## Module 5 — Two bugs, and how they were found

**A layout bug that hid the evidence.** The new Bench button was placed at
`BOTTOM_MID -60`, landing exactly on `bootLabel` at y=378 — the wake-reason readout
we needed. LVGL's `lv_obj_align` places widgets at absolute offsets with **no
collision detection**: nothing warns you, the button just draws on top and the
loser is invisible. Hand-tuning y-offsets across eight widgets is a sign the screen
has outgrown manual alignment; LVGL's flex/grid containers would have caught it.

**A deep-sleep wake bug, still open.** Tapping Sleep woke instantly with
`woke: touch`. Three hypotheses, three failures:

1. *Touch controller heartbeat.* Fixed the guard to watch the **pin** rather than
   `getPoint()` (EXT1 watches the line, not fingers) and drain the controller.
   Result: `IRQ settled in 500 ms` — the minimum possible. **No chatter at all.**
   Theory dead.
2. *RTC pad never initialised.* `rtc_gpio_pullup_en()` has no effect until
   `rtc_gpio_init()` switches the pad to its RTC function. Added it. **Still woke.**
3. *RTC_PERIPH powered down + floating touch RESET.* Internal pull-ups live in a
   domain deep sleep switches off (`esp_sleep_pd_config(..., ESP_PD_OPTION_ON)`),
   and a floating `TOUCH_RST` lets the CST226SE reset itself — a just-reset touch
   controller asserts IRQ. Added both. **Still woke.**

All three are correct, necessary fixes that any working deep-sleep-on-touch needs.
None of them was *the* bug. The lesson is the process failure: after three
hypothesis-driven attempts, the right move was to stop pattern-matching and start
**bisecting** — arm EXT1 with no pull-up configured at all, and separately arm it on
an unconnected pin, to prove whether the wake comes from the pin's electrical state
or the wake source's configuration. **Unresolved — see PLAN.md.**

Timer-only deep sleep (mode E) works correctly.

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

---

## What you built and learned

- ✅ Understood what an inline USB meter measures — and why **charge current** ruins it
- ✅ Built a **power bench** that holds states still for a slow instrument
- ✅ Measured by **differences**; put real mA on Stages 5b–5d
- ✅ Found the backlight is **not** dominant, and deep sleep buys **2 mA** here
- ✅ Shipped **`SLPIN`** into the idle path — 9 mA, the biggest single win of the day
- ✅ Fixed white noise on **wake** and on **power-on** — the same conflation, twice
- ✅ Learned to **erase a bad state rather than sequence around it** (guarantee > race)
- ✅ Learned that **53% of this board's draw is not reachable from firmware**
- ✅ Learned when to stop hypothesising and start **bisecting**

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

## Next lesson

Two open threads: the **unexplained ~69 mA** (schematic, LED, a meter on the 3.3 V
rail) and the **deep-sleep touch-wake bug** (bisect the wake source). Or leave power
behind for the physical **buttons** (GPIO 0/12/16), the **SD card** (SPI CS 14), or
an LVGL refactor onto flex/grid containers — which the layout collision above argues
for on its own.
