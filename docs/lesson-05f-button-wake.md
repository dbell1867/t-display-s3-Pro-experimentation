# Lesson 05f — Button wake: choosing a wake source you can trust

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Replace the deep-sleep touch wake — which never worked —
with a **physical button**, after *measuring* the button rather than assuming how it
behaves. Short stage, but it closes a thread running through three lessons.

> Continues from [05e](lesson-05e-power-meter.md), which finally explained *why* the
> touch wake failed.

---

## Learning objectives

By the end of this lesson you can:

1. Explain why a **touch controller is a poor deep-sleep wake source**.
2. **Probe** an input before designing around it — pull-ups, idle level, bounce.
3. Test for an **external pull-up** with an internal pull-down.
4. Configure **EXT1 wake on a button**, and know why an external pull-up lets you
   drop `ESP_PD_DOMAIN_RTC_PERIPH`.
5. Recognise when you've hit your **instrument's resolution limit** and stop.

---

## Module 1 — Why the touch line could never work

From 05e's post-mortem: the CST226SE asserts its IRQ when it has a report and
**holds it LOW until someone reads it**. While the firmware is awake it polls
constantly and drains every report, so the line looks perfectly quiet. The instant
we stop polling *in order to sleep*, the next event latches the line low, and an
`ANY_LOW` wake fires immediately.

> **A touch controller asserts precisely when nobody is listening.** That makes it
> structurally unsuitable as a deep-sleep wake source — not a bug to be fixed with
> better pin configuration.

Three earlier attempts all assumed something was *pulling* the line down. It was
never being *released*. No amount of ESP-IDF knowledge would have found that,
because the fault was a property of the **peripheral**, not of the sleep API.

---

## Module 2 — Probe before you design

We had been burned enough to measure first. Temporary scaffolding read all three
documented button pins and displayed, live on screen:

- **idle level** — all three read HIGH, so they're active-low.
- **press counts** — GPIO 0 and 16 counted; **GPIO 12 never did**.
- **external pull-up?** — the interesting test:

```cpp
// Drive an internal PULLDOWN and see who wins.
// Still HIGH => a strong EXTERNAL pull-up. LOW => nothing external.
pinMode(pin, INPUT_PULLDOWN);
delay(5);
bool external = (digitalRead(pin) == HIGH);
```

**Result: `0:ext 12:ext 16:ext`** — all three externally pulled up.

Two findings, one of which corrected our board notes:

1. **The board has only TWO buttons** (GPIO 0 = BOOT, GPIO 16 = user), not the three
   the vendor docs imply. GPIO 12 has a pull-up but nothing attached. *The reference
   was wrong; the measurement was right.*
2. **The external pull-up is the prize.** Internal pull-ups live in `RTC_PERIPH` — a
   power domain deep sleep switches **off**. Relying on one forces
   `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)`, burning current
   all night purely to hold a line high. An external resistor does it for free.

### A probe that measured the wrong thing

The same scaffolding reported **zero bounce** on every press — which is not
credible for mechanical contacts. The flaw: the falling edge was detected in the
main `loop()` (~5 ms cadence), and only *then* did a tight 50 ms sampling window
start. Contact bounce lasts 1–5 ms; it was over before we arrived. **We measured
the wrong interval and got a clean-looking answer.**

We chose not to fix it, for a reason worth stating: **for a wake source, bounce is
harmless.** Extra edges just mean the chip wakes on the first and the rest land
while it's already awake. Bounce only matters for *counting* presses. Knowing which
measurements you don't need is as useful as taking them.

---

## Module 3 — Wiring up the wake

```cpp
#define BTN_WAKE 16     // external pull-up, RTC-capable (GPIO 0-21 on the S3)

// Don't arm EXT1 while the button is still held, or ANY_LOW fires immediately.
// Unlike the touch line, this one genuinely idles high, so a simple wait works.
uint32_t t0 = millis();
while (digitalRead(BTN_WAKE) == LOW && (millis() - t0) < 5000) delay(10);
delay(50);                                  // let release bounce settle

esp_sleep_enable_ext1_wakeup(1ULL << BTN_WAKE, ESP_EXT1_WAKEUP_ANY_LOW);
esp_sleep_enable_timer_wakeup(20ULL * 1000000);
esp_deep_sleep_start();                     // never returns
```

Note what's **gone** compared with the touch version: no `rtc_gpio_init()`, no
`rtc_gpio_pullup_en()`, no `rtc_gpio_hold_en()`, and no
`esp_sleep_pd_config(RTC_PERIPH, ON)`. The external resistor removes the entire
class of problem those calls existed to work around.

Worked **first try**: `Boot #12  woke: button`.

| | Touch IRQ (GPIO 21) | Button (GPIO 16) |
|---|---|---|
| Idle state | LOW whenever a report is unread | genuinely high |
| Pull-up | internal, in `RTC_PERIPH` (powered down) | **external resistor** |
| Held by anything? | yes — the controller, until read | nothing |
| Attempts to make it work | 4, all failed | 1 |

---

## Module 4 — The button fixed a second problem

Bench modes E and F are both dark, and F used to arrive on a silent timer — so you
couldn't tell which state the meter was showing. The button, which works regardless
of what the software has shut down, now advances the bench in **every** mode:

```
Bench (tap) → A →[btn]→ B →[btn]→ C →[btn]→ D →[btn]→ E →[btn]→ F
```

> A physical input that survives everything the firmware turns off is exactly what a
> power bench needs — and it's the one thing touch could never be. When a new
> primitive immediately solves an unrelated problem, that's usually a sign it was
> the right primitive.

---

## Module 5 — The final bench, and the resolution limit

Charger supply, charging disabled:

| State | mA | Δ |
|---|---|---|
| A: poll + BL 100% | 97.5 ⚠ | |
| B: poll + BL off | 71 | backlight ≈ 20 ⚠ |
| C: light sleep | 38.5 | **busy CPU 32.5** |
| D: + display off | 28.3 | **ST7796 10.2** |
| E: + touch/ALS off | 27.5 | touch + ALS 0.8 |
| F: deep sleep | 25 | deep vs light 2.5 |

Four differences reproduced within **0.8 mA** of the previous session — the bench is
trustworthy.

**But we could not demonstrate the `RTC_PERIPH` saving.** F went 27 → 25, which
looks like a win — except *every* mode dropped ~2 mA, including code paths we never
touched (B 74→71, C 41→38.5, D 30→28.3). A uniform offset between runs, cause
unknown (supply, battery state, temperature). The saving is real in principle and
invisible in practice.

> **We have hit the resolution limit.** Differences ≥10 mA are solid; ~2 mA is
> indistinguishable from run-to-run drift. Anything smaller needs a better
> instrument, not better firmware. Knowing where your measurement stops being
> meaningful is part of measuring.

`A − B` has now read 18, 6, 20 and 26.5 mA across four runs — A is the only mode
near the current where the SY6970 starts supplementing from the battery. **The
backlight is "roughly 20 mA, ± quite a lot"**, and that's as precise as this setup
gets.

---

## What you built and learned

- ✅ Understood why a **touch controller can't be a deep-sleep wake source**
- ✅ **Probed** an input before designing around it — and corrected the board notes
- ✅ Tested for an **external pull-up** with an internal pull-down
- ✅ Configured **EXT1 button wake**; dropped four RTC calls and a whole power domain
- ✅ Watched a good primitive **solve a second, unrelated problem**
- ✅ Recognised a probe that **measured the wrong interval** — and decided not to care
- ✅ Recognised the **resolution limit** of the instrument, and stopped

## Glossary

- **External pull-up** — a physical resistor; unlike the internal one it doesn't live
  in a power domain that deep sleep switches off.
- **`INPUT_PULLDOWN` test** — drive a pin low internally; if it still reads HIGH,
  something external is pulling it up.
- **Contact bounce** — mechanical chatter for 1–5 ms after a press. Harmless for a
  wake source; matters when counting.
- **Resolution limit** — the point below which run-to-run drift exceeds the effect
  you're trying to measure.

## Next lesson

Power is finished. Remaining onboard hardware: the **SD card** (SPI CS 14), or a
**button-driven UI** now that we have a real button. The gauge screen also hand-aligns
~9 widgets by absolute offset and has produced one invisible-overlap bug — an
argument for an **LVGL flex/grid refactor**.
