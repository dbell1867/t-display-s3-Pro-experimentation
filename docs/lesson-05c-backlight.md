# Lesson 05c — Backlight power: PWM + LTR-553 auto-brightness

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Tackle the **biggest remaining power draw** — the
backlight. Drive it with **PWM** so it's dimmable, add the **LTR-553** ambient
light/proximity sensor for **auto-brightness**, and turn the **screen off when
idle** (waking on touch or proximity), combined with the Lesson 05b light sleep.

> Continues from [Lesson 05b](lesson-05b-low-power.md), which cut *CPU* power but
> noted the **backlight dominates** the budget and stays on. This is that next lever.

---

## Learning objectives

By the end of this lesson you can:

1. Drive an LED/backlight with **PWM (LEDC)** on ESP32 core 3.x and dim it.
2. Add a **third device** to a shared I²C bus and bring up the **LTR-553**.
3. Build **auto-brightness**: map a sensor to an actuator with **constrain + map**,
   **smoothing**, and a **floor**.
4. Implement a **screen-off idle timeout** with touch/proximity wake.
5. Reason about a subtle interaction: **PWM vs light-sleep clock gating**, and why
   we sleep only when the screen is off.
6. Solve an **observability** problem (the instrument is on the screen you turned off).

---

## Module 1 — PWM backlight (LEDC)

Until now the backlight was just **on**: `digitalWrite(TFT_BL, HIGH)`. To *dim* it
we drive the pin with **PWM** — a fast on/off square wave whose **duty cycle**
(fraction of time high) sets the average brightness. On ESP32 that's the **LEDC**
peripheral. Core 3.x has a simple pin-based API:

```cpp
ledcAttach(TFT_BL, 5000, 8);   // 5 kHz carrier, 8-bit duty (0..255)
ledcWrite(TFT_BL, 128);        // 50% -> half brightness
```

5 kHz is well above what the eye can see, so it looks like a steady dimmer, not a
flicker. 8-bit gives 256 brightness levels. (Core 2.x used a clunkier
channel-based API — `ledcSetup` + `ledcAttachPin`; core 3.x binds it all to the
pin.)

---

## Module 2 — A third chip on the I²C bus: LTR-553

The **LTR-553** is a combined **ambient-light** + **proximity** sensor at I²C
address **0x23** — on the *same* SDA=5/SCL=6 bus as touch (0x5A) and the PMU
(0x6A). Three independent chips, two wires: that's I²C doing its job. Driver is
**SensorLib** (already a dependency), class `SensorLTR553`:

```cpp
alsOk = als.begin(Wire, LTR553_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
als.setLightSensorGain(SensorLTR553::ALS_GAIN_1X);  // 1..64k lux range
als.enableLightSensor();
als.enableProximity();
...
int light = als.getLightSensor(0);   // channel 0 = broadband (our brightness proxy)
int prox  = als.getProximity();      // short-range: ~0 until a hand is close
```

We proved both halves first: `light` fell from ~15 to 0 when covered; `prox`
jumped from 0 to ~600 when a hand cupped over it. (The proximity is **short-range**
— it registers around ~3 cm, a "hover/cover" detector, not a long-distance
approach sensor.)

---

## Module 3 — Auto-brightness (sensor → actuator)

Map the light reading to a backlight duty, then **smooth** and **floor** it:

```cpp
int t      = constrain(light, LIGHT_DARK, LIGHT_BRIGHT);      // clamp to a sane window
int target = map(t, LIGHT_DARK, LIGHT_BRIGHT, BL_MIN_DUTY, BL_MAX_DUTY);
autoBrightness = (uint8_t)((autoBrightness * 3 + target) / 4); // EMA smoothing
```

Three ideas worth naming:

- **`constrain` + `map`** — the bread-and-butter of turning a raw sensor range
  into an actuator range. `constrain` first, so readings outside the window don't
  map to nonsense.
- **Smoothing (exponential moving average)** — `new = (old*3 + target)/4` eases
  the brightness toward the target instead of snapping on every noisy reading.
- **A floor (`BL_MIN_DUTY`)** — never dim to fully off *while in use*; the screen
  must stay readable. (Off is reserved for the idle timeout, Module 4.)

The values (`LIGHT_DARK/BRIGHT`, floor) are hand-tuned to the room and are the
obvious knobs to adjust.

---

## Module 4 — Screen-off idle timeout (+ the PWM/sleep interaction)

The big saving: after `BL_OFF_MS` with no activity, drive the backlight to **0**
(off). Touch — *or* proximity — counts as activity and restores it:

```cpp
if (prox > PROX_WAKE) lastActivityMs = millis();          // hand near = activity
...
bool screenIdle = (millis() - lastActivityMs) > BL_OFF_MS;
applyBacklight(screenIdle);                               // 0 when idle, else autoBrightness
if (!onUsbCached && screenIdle && lv_anim_count_running() == 0) enterLightSleep();
else                                                            delay(5);
```

**The subtle part — sleep only when the screen is OFF.** A PWM backlight depends on
a peripheral clock that **gets gated during light sleep**. If we slept with the
backlight at, say, 50% duty, the PWM would freeze mid-cycle each nap and the
brightness would glitch. So we only light-sleep once the duty is **0** (a steady
low level — no PWM to glitch). Happily that's also when the biggest win lands:
**screen dark and CPU asleep together.** (Full brightness, duty 255 = steady high,
would also be glitch-free — it's only the *in-between* PWM levels that can't
survive the sleep.)

---

## Module 5 — When your instrument is on the screen you turned off

A neat problem surfaced: we wanted to *see* the CPU drop to ~1/s, but that only
happens once the screen is off — so the on-screen `CPU: /s` counter is invisible
exactly when it matters. The instrument shares the fate of the thing it measures.

Fix: **capture** the idle rate while asleep and display it on the next wake. During
each idle second the 1 s update still runs (on the sleep timer wake), so
`loopCount` for that second *is* the sleeping rate:

```cpp
if ((millis() - lastActivityMs) > BL_OFF_MS) idleCpu = loopCount;   // stash it
lv_label_set_text_fmt(cpuLabel, "CPU:%u/s idle:%u", loopCount, idleCpu);
```

Result on hardware, after waking a timed-out screen:
- **On battery:** `idle:1` — the CPU slept while the screen was dark.
- **On USB:** `idle:178` — we never sleep on USB (the battery-gate), made visible.

---

## What you built and learned

- ✅ **PWM backlight** with LEDC (`ledcAttach`/`ledcWrite`) — dimmable, flicker-free
- ✅ Brought up the **LTR-553** (3rd chip on the shared I²C bus); light + proximity
- ✅ **Auto-brightness** — `constrain`/`map`, EMA smoothing, a readability floor
- ✅ **Screen-off idle timeout** with **touch and proximity wake**
- ✅ Understood **PWM vs light-sleep clock gating** → sleep only when the screen is off
- ✅ Solved an **observability** problem by capturing the idle metric for later display

## Command cheat-sheet

```bash
pio run -d ~/Work/Micro/tdisplay -t upload
```

## Glossary

- **PWM / duty cycle** — fast on/off; the high fraction sets average brightness.
- **LEDC** — the ESP32 LED-control PWM peripheral.
- **`constrain` / `map`** — clamp a value to a range / rescale one range to another.
- **EMA (exponential moving average)** — cheap smoothing: `new = (old*k + x)/(k+1)`.
- **Ambient light sensor (ALS)** — measures scene brightness (broadband channel).
- **Proximity sensor** — IR reflection; detects a nearby object (here short-range).
- **Idle timeout / screen-off** — blank the display after inactivity to save power.

## Next lesson

Options: **deep sleep** for the lowest floor (needs RTC-GPIO / ext wake, and the
peripherals re-init on wake), or other onboard hardware — physical buttons
(GPIO 0/12/16), SD card (SPI CS 14) — or an inline **USB power meter** to put real
milliamp numbers on all of this.
