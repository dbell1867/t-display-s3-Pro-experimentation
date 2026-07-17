# Lesson 05 — A battery gauge with the SY6970 PMU

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Read the board's **SY6970** power-management chip over
I²C and turn it into a live **battery gauge** in LVGL — the instrument we'll use
to *see* power consumption in the upcoming low-power lessons.

> Continues from [Lesson 04](lesson-04-lvgl.md). We reuse the whole LVGL stack
> (flush + read callbacks, aligned draw buffer) and add one new I²C peripheral.

---

## Learning objectives

By the end of this lesson you can:

1. Explain what a **power-path / charger IC** is and how it differs from a
   **fuel gauge**.
2. Put a **second device on an existing I²C bus** and address it independently.
3. Bring up the SY6970 with **XPowersLib** and read its ADC.
4. Estimate battery **state-of-charge from voltage** — and know why that's only
   an approximation.
5. Build a small **LVGL gauge** (bar + labels) and update it on a timer.

---

## Module 1 — What the SY6970 is (and isn't)

The SY6970 is a **battery charger + power-path IC**. It sits between USB (VBUS),
the LiPo battery, and the system rail. It decides whether to charge the battery,
run the system from USB, or run from the battery — and it has an on-chip **ADC**
that measures VBUS voltage, battery voltage, system voltage and charge current,
plus charge status.

What it is **not** is a **fuel gauge**. A true fuel gauge (e.g. a MAX17048)
*coulomb-counts* — integrates current over time — to know real state-of-charge.
The SY6970 only gives us **voltage**, so any "percentage" we show is *derived*
from voltage. That distinction drives the honest caveat in Module 4.

### It shares the I²C bus

The SY6970 is at I²C address **0x6A**, on the **same** SDA=5 / SCL=6 bus as the
CST226SE touch controller (**0x5A**). I²C is a bus — multiple devices, each with
its own address, share two wires. So we don't add any pins; we just create a
second driver on the same `Wire`. (Bringing up two independent chips on one bus
and having both work is itself the proof that I²C addressing does its job.)

---

## Module 2 — XPowersLib, and a header gotcha

The driver is **XPowersLib** (`lewisxhe/XPowersLib`). Add it to `lib_deps`:

```ini
lib_deps =
    ...
    lewisxhe/XPowersLib         ; SY6970 PMU / charger driver
```

The examples declare the object as `XPowersPPM PPM;` — but that **failed to
compile** for us: *`'XPowersPPM' was not declared`*. Reading the header
(`XPowersLib.h`) explains why:

```cpp
#if   defined(XPOWERS_CHIP_AXP2101)
  ...
#elif defined(XPOWERS_CHIP_SY6970)
  #include "PowersSY6970.tpp"
  typedef PowersSY6970 XPowersPPM;     // <-- the alias only exists in this branch
#else
  // includes every driver but defines NO XPowersPPM alias
#endif
```

The convenient `XPowersPPM` alias only exists if you pre-`#define` a chip macro
(`XPOWERS_CHIP_SY6970`). Without it you land in the `#else` branch — all drivers
included, but no alias. **Fix (the clearer one):** name the concrete class
directly, no macro needed:

```cpp
PowersSY6970 PPM;
```

> **Gotcha:** vendor examples often assume a `-D CHIP` build flag you didn't
> copy. When an "obvious" type is undefined, read the library header — the name
> is usually gated behind a macro. Using the concrete class sidesteps it and
> documents which chip you mean.

---

## Module 3 — Bringing it up

Init on the shared bus, then **turn the ADC on** (`enableMeasure`) or every
getter reads zero:

```cpp
pmuOk = PPM.init(Wire, I2C_SDA, I2C_SCL, SY6970_SLAVE_ADDRESS);  // 0x6A
if (pmuOk) PPM.enableMeasure();
```

Then the reads (all straightforward):

```cpp
uint16_t vbat = PPM.getBattVoltage();     // mV
uint16_t vbus = PPM.getVbusVoltage();     // mV
uint16_t ichg = PPM.getChargeCurrent();   // mA
bool     vin  = PPM.isVbusIn();
bool     chg  = PPM.isCharging();
const char *s = PPM.getChargeStatusString();
```

We proved comms first with a raw text dump (step 1) before making it pretty —
seeing `VBUS 5100 mV, Battery 4124 mV, Charging: yes` confirmed the chip answers
and the ADC is live.

---

## Module 4 — Voltage → percentage (and why it lies while charging)

A single-cell LiPo's discharge curve is **non-linear** — nearly flat through the
middle of its range — so a naïve `(v - 3300) / (4200 - 3300)` map is poor. We use
a small table of `(mV, %)` points and linearly interpolate between them:

```cpp
static const struct { uint16_t mv; uint8_t pct; } pts[] = {
  {3300,0},{3600,10},{3700,25},{3750,40},{3800,55},
  {3850,68},{3900,78},{4000,90},{4100,97},{4200,100},
};
```

**The demonstration that makes the caveat concrete:** on USB the gauge read
**97%** at 4.124 V. Unplugging USB — *nothing about the battery's charge changed*
— it dropped to **90%**. Why? While charging, the charge current props the
terminal voltage *up*, so voltage over-reports charge. Remove the current and the
voltage sags to its true resting value, which maps to a lower (honest) %.

> **Lesson:** a voltage-based gauge reads optimistically on the charger and
> accurately on battery under light load. That's fine for our purpose — the
> low-power lessons run **on battery**, precisely where it's most trustworthy.

---

## Module 5 — The LVGL gauge

Pure reuse of Lesson 04's stack. New widget: `lv_bar`.

```cpp
bar = lv_bar_create(scr);
lv_bar_set_range(bar, 0, 100);
lv_obj_set_style_bg_color(bar, lv_color_hex(0x303040), LV_PART_MAIN);       // track
...
lv_bar_set_value(bar, pct, LV_ANIM_ON);                                     // animates
lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);       // fill
```

Note the two **parts** of a bar: `LV_PART_MAIN` (the track) and
`LV_PART_INDICATOR` (the filled portion) are styled independently — a general
LVGL pattern (knobs, scrollbars, etc. are parts too). The fill colour is chosen
by level (green ≥50%, amber ≥20%, else red).

We refresh once a second with a simple `millis()` throttle in `loop()`:

```cpp
static uint32_t lastUpd = 0;
if (millis() - lastUpd >= 1000) { updateGauge(); lastUpd = millis(); }
```

(A more idiomatic LVGL approach is `lv_timer_create(cb, 1000, NULL)` — worth
trying as an exercise.)

---

## What you built and learned

- ✅ Understand **charger/PMU vs fuel gauge**, and voltage vs true state-of-charge
- ✅ Added a **second I²C device** on the existing bus (0x6A alongside touch's 0x5A)
- ✅ Brought up the SY6970 with **XPowersLib** — including the `XPowersPPM`
      alias/macro gotcha (used the concrete `PowersSY6970` class)
- ✅ Enabled the ADC (`enableMeasure`) and read voltage/current/charge state
- ✅ Estimated **SoC from voltage** with a LiPo curve, and *proved* on hardware
      why it over-reads while charging (97% → 90% on unplug)
- ✅ Built an **LVGL bar gauge** and learned widget **parts** (track vs indicator)

## Command cheat-sheet

```bash
pio run -d ~/Work/Micro/tdisplay -t upload    # -d points pio at the project dir
```

## Glossary

- **PMU / power-path IC** — manages USB/battery/system power routing + charging.
- **Fuel gauge** — a chip that coulomb-counts for true state-of-charge (the
  SY6970 is *not* one).
- **VBUS** — the USB 5 V input rail.
- **State-of-charge (SoC)** — how full the battery is; only approximated here.
- **`enableMeasure()`** — turns on the SY6970's ADC so the getters return live data.
- **`lv_bar` / parts** — an LVGL progress bar; `MAIN` = track, `INDICATOR` = fill.

## Next lesson

**Stage 5b / low-power lesson.** With the gauge in hand, convert the touch input
to a **true hardware interrupt** (`attachInterrupt` on GPIO 21 → `volatile` flag)
so the CPU can **light-sleep** and wake on touch, and watch the battery current
drop on this very gauge. That's the "design C" we deferred back in Lesson 02.
