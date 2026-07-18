# Lesson 05d — Deep sleep: power down and reboot on wake

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** The deepest power state. Power the chip down to the RTC
domain (~tens of µA), keep just enough state in **RTC memory** to survive, and
**wake by rebooting** — on a touch (RTC-GPIO) or a timer.

> Continues from [Lesson 05b](lesson-05b-low-power.md) (light sleep) and
> [05c](lesson-05c-backlight.md) (backlight). Deep sleep is the bottom of the
> power ladder we've been climbing.

---

## Learning objectives

By the end of this lesson you can:

1. Explain **deep sleep vs light sleep** — reboot vs resume, and what survives.
2. Keep state across deep sleep with **`RTC_DATA_ATTR`** (RTC memory).
3. Configure deep-sleep **wake sources**: **EXT1** on an RTC GPIO, and a timer.
4. Detect **why** the chip woke (`esp_sleep_get_wakeup_cause()`).
5. Hold a **non-RTC pin's level** through deep sleep (`gpio_hold_en`).
6. Avoid the **instant-rewake** trap (wait for the wake line to release).

---

## Module 1 — Deep sleep vs light sleep

|                | **Light sleep** (05b) | **Deep sleep** (this lesson) |
|----------------|-----------------------|------------------------------|
| CPU / RAM      | halted, **retained**  | **powered down, RAM lost**   |
| On wake        | *resumes* after the call | **reboots** — `setup()` re-runs |
| Surviving state| everything            | only **RTC memory**          |
| Current        | ~mA → hundreds of µA  | **~tens of µA**              |
| Wake latency   | ~ms                   | full boot (~1 s here)        |

The headline: deep sleep is not "pause", it's **"power off, then reboot on an
event."** So it fits a device that's off most of the time and wakes rarely — not
our always-on gauge. We added it as a deliberate **"Deep Sleep" button** and left
the light-sleep idle behaviour (05b/05c) alone.

---

## Module 2 — State that survives: RTC memory

Since RAM is wiped, any state you need after wake must live in **RTC memory** —
declared with `RTC_DATA_ATTR`:

```cpp
RTC_DATA_ATTR uint32_t bootCount = 0;   // survives deep sleep; reset only on power-on/flash
```

We increment it every boot and show it. Across deep-sleep cycles it climbed
1 → 2 → 3 while everything else restarted from scratch — the visible proof that
RTC memory persists and normal globals don't. (RTC memory is small — a few KB —
and is for exactly this: a little carry-over state, not your whole program.)

---

## Module 3 — Waking up: sources + cause

Deep sleep can only be woken by the **RTC domain**, so wake GPIOs must be
**RTC-capable** (on the ESP32-S3, GPIO 0–21 — our touch IRQ on GPIO 21 qualifies).
We enable two sources:

```cpp
rtc_gpio_pullup_en((gpio_num_t)TOUCH_IRQ);                 // idle HIGH in the RTC domain
rtc_gpio_pulldown_dis((gpio_num_t)TOUCH_IRQ);
esp_sleep_enable_ext1_wakeup(1ULL << TOUCH_IRQ, ESP_EXT1_WAKEUP_ANY_LOW);  // touch pulls LOW
esp_sleep_enable_timer_wakeup(20ULL * 1000000);            // ...or after 20 s
esp_deep_sleep_start();                                    // never returns
```

- **EXT1** is the S3's multi-GPIO RTC wake (the classic ESP32's **EXT0** was
  dropped on the S3). `ANY_LOW` wakes when any listed pin goes low — our touch
  line pulses low, so a touch wakes it. We enable an **RTC pull-up** so the line
  idles high in the RTC domain (the main-domain `INPUT_PULLUP` doesn't apply once
  the chip is powered down).
- `esp_deep_sleep_start()` **never returns** — the next thing that runs is the
  reset vector, i.e. `setup()`.

On the next boot, ask **why** we woke:

```cpp
switch (esp_sleep_get_wakeup_cause()) {
  case ESP_SLEEP_WAKEUP_EXT1:  wakeReason = "touch";    break;
  case ESP_SLEEP_WAKEUP_TIMER: wakeReason = "timer";    break;
  default:                     wakeReason = "power-on"; break;   // fresh boot / flash
}
```

We measured `woke: touch` and `woke: timer` — both sources confirmed.

---

## Module 4 — Two gotchas that make it actually work

**1. Hold a non-RTC pin through sleep.** The backlight is GPIO 48, which is *not*
an RTC pad — once the chip powers down, its level floats (the screen could glow).
So we drive it low and **latch** it:

```cpp
ledcWrite(TFT_BL, 0);
gpio_hold_en((gpio_num_t)TFT_BL);   // freeze this pad's current level
gpio_deep_sleep_hold_en();          // ...and keep holds active through deep sleep
```

On the next boot we **release** the holds before re-driving the pin:

```cpp
gpio_deep_sleep_hold_dis();
gpio_hold_dis((gpio_num_t)TFT_BL);
```

**2. Don't instantly re-wake.** You tap "Deep Sleep" *with your finger on the
screen* — so the touch line is already LOW, and an `ANY_LOW` wake would fire the
instant we sleep. Wait for the finger to lift first:

```cpp
uint32_t up = millis();
while (millis() - up < 400) {                 // 400 ms of continuous no-touch
  if (touch.getPoint(x, y, 1) > 0) up = millis();
  delay(20);
}
```

---

## Module 5 — Re-initialising on wake

Because deep sleep reboots, `setup()` runs fresh every wake and **re-initialises
every peripheral** — display, touch, PMU, light sensor, LVGL. There's no
short-cut; the ~1 s you see before the gauge redraws *is* that boot. (Contrast
light sleep, which resumed mid-`loop()` with all of that still live.) This is the
cost side of deep sleep's tiny current: you pay a full boot on every wake, so it
only wins when wakes are infrequent.

---

## Module 6 — What we could measure, honestly

Deep sleep current is ~tens of µA — but the on-board SY6970 is far too coarse to
show that, so we don't pretend to. The **on-device proof** is behavioural:

- the screen goes fully dark and *stays* dark (backlight held),
- waking takes a visible **reboot** (not an instant resume),
- the **boot counter** climbs while all other state resets,
- the **wake cause** reports touch vs timer correctly.

For real numbers you'd put an **inline USB power meter** (or a bench supply's
current readout) on it — the natural companion tool for any serious low-power work.

---

## What you built and learned

- ✅ Understand **deep vs light sleep** (reboot vs resume; what survives; current)
- ✅ Persisted state through deep sleep with **`RTC_DATA_ATTR`** (proven by a boot counter)
- ✅ Configured **EXT1 (RTC-GPIO) + timer** wake, and read the **wake cause**
- ✅ **Held a non-RTC pin** (backlight) low through sleep, released it on boot
- ✅ Avoided the **instant-rewake** trap by waiting for the wake line to release
- ✅ Completed the power ladder: **busy-poll → light sleep (~mA) → deep sleep (~µA)**

## Command cheat-sheet

```bash
pio run -d ~/Work/Micro/tdisplay -t upload
# If the board is asleep when you flash: the RESET button (or unplug/replug) gets
# it back; the 20 s timer wake also reboots it on its own.
```

## Glossary

- **Deep sleep** — chip powered to the RTC domain only; wakes by rebooting.
- **RTC domain / RTC memory** — the always-on island; `RTC_DATA_ATTR` state lives here.
- **EXT1** — deep-sleep wake from one or more RTC GPIOs (S3 has no EXT0).
- **RTC GPIO** — a pad wired to the RTC domain (GPIO 0–21 on the S3) — can wake deep sleep.
- **`gpio_hold_en` / deep-sleep hold** — latch a pad's level so it survives sleep.
- **Wake cause** — `esp_sleep_get_wakeup_cause()`: why the chip left sleep.

## Next lesson

The power arc is complete. From here: an **inline USB power meter** to put real
milliamps on all of it, or other onboard hardware — the physical **buttons**
(GPIO 0/12/16) and **SD card** (SPI CS 14) — or circle back to the UI with a
richer LVGL screen.
