# Lesson 05b — Low power: light sleep + wake on touch

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Stop the CPU busy-polling forever. Make it **light-sleep**
when idle and **wake on touch**, and *see* the effect with an on-screen activity
instrument. This is the "design C" we deferred all the way back in Lesson 02.

> Continues from [Lesson 05](lesson-05-battery.md) — we reuse that battery gauge
> as a coarse power instrument, and add the CPU-activity counter that actually
> makes the win visible.

---

## Learning objectives

By the end of this lesson you can:

1. Explain **polling vs interrupts**, and why interrupts enable sleep.
2. Write a correct **ISR** (`IRAM_ATTR`, `volatile`, no I²C/heavy work inside).
3. Recognise and fix **phantom interrupts** from a floating pin (pull-ups).
4. Put an ESP32 into **light sleep** and configure **wake sources** (GPIO level +
   timer).
5. Design **defensively** around a noisy wake source (wake → check → re-sleep).
6. Reason honestly about **what you measured** (CPU activity ≠ milliamps).

---

## Module 1 — Measure first: the activity instrument

We can't improve what we can't see, and the on-board SY6970 is a *coarse* power
instrument (it reads voltage + charge current, not live CPU milliamps). So the
primary instrument lives on the screen: a **loops-per-second counter**.

```cpp
void loop() {
  loopCount++;                 // one lap of loop()
  lv_timer_handler();
  if (millis() - lastUpd >= 1000) {
    lv_label_set_text_fmt(cpuLabel, "CPU: %u /s", loopCount);
    loopCount = 0; lastUpd = millis();
  }
  delay(5);
}
```

**Baseline:** ~**178 /s**, and — critically — it stays that high *even when you're
not touching the screen*. The CPU does 178 laps a second doing nothing useful:
`lv_timer_handler()` then a `delay(5)` that isn't real sleep. That constant number
is the waste we're about to remove.

---

## Module 2 — Polling vs interrupts

Until now we **polled**: every lap we asked the touch chip "anything?" An
**interrupt** inverts that — the touch chip's IRQ line (GPIO 21) tells *us* the
instant something happens, so the CPU needn't keep asking. That inversion is what
makes sleeping possible: a sleeping CPU can't poll, but a hardware line can still
wake it.

### ISR rules (the new embedded content)

An **Interrupt Service Routine** is a function the *hardware* calls, interrupting
`loop()` mid-lap. It must be tiny:

```cpp
static volatile uint32_t irqCount = 0;     // ISR writes, loop() reads
static void IRAM_ATTR touchISR(void) {     // IRAM_ATTR: lives in fast RAM
  irqCount++;                              // no I2C, no Serial, no allocation
}
...
attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), touchISR, FALLING);
```

- **`IRAM_ATTR`** — put the ISR in internal RAM so it can run even while flash is
  busy (an ISR that lived in flash could deadlock).
- **`volatile`** — the ISR changes the variable "behind the compiler's back", so
  it must re-read it from memory, not cache a stale copy in a register.
- **No I²C / `Serial` / heavy work** — those block or allocate; an ISR must
  return in microseconds. That's *why* the actual touch coordinates are still read
  by polling in `loop()`; the interrupt only says "something happened".

We displayed an "IRQ events" counter and watched it climb on touch — proving the
line is a trustworthy wake source *before* depending on it.

---

## Module 3 — Phantom interrupts (a floating-pin trap)

The IRQ counter also crept up **when untouched**. First suspect: a **floating
pin**. Configured as plain `INPUT`, GPIO 21 isn't strongly driven between the
controller's pulses, so nearby signals couple in tiny wiggles that cross the logic
threshold — each stray dip is a phantom FALLING edge. Fix: give the pin a defined
idle level.

```cpp
pinMode(TOUCH_IRQ, INPUT_PULLUP);   // idles firmly HIGH; only a real driven-LOW pulse triggers
```

That killed most of the creep — but a **small residual creep remained**. That's
the honest lesson: the CST226SE also emits its *own* periodic IRQ pulses (partly
provoked by our 30 Hz polling). **A real wake source is never perfectly quiet.**
So we don't chase zero — we design defensively (Module 5).

> **Gotcha:** never leave an interrupt pin floating. And even with a pull-up,
> expect *some* spurious events from a real sensor — handle them, don't assume
> them away.

---

## Module 4 — Light sleep + wake sources

Light sleep halts the CPU and gates most clocks; RAM and peripherals keep state,
and execution **resumes right after** the call. We wake on two sources:

```cpp
static void enterLightSleep(void) {
  esp_sleep_enable_timer_wakeup(1000000ULL);                     // 1 s: refresh the gauge
  gpio_wakeup_enable((gpio_num_t)TOUCH_IRQ, GPIO_INTR_LOW_LEVEL);// touch pulses LOW
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start();          // ---- CPU halts here until a wake event ----

  gpio_wakeup_disable((gpio_num_t)TOUCH_IRQ);
  wakeCount++;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO)
    lastActivityMs = millis();      // woken by touch: stay up to service it
}
```

**Level, not edge.** Light-sleep GPIO wake is **level-triggered**
(`GPIO_INTR_LOW_LEVEL`), a *different* mechanism from Step 2's edge
`attachInterrupt`. Because they'd fight over the pin's interrupt type, we drop the
edge ISR here — Step 2 had already done its job (proving the line responds). The
1 s timer wake keeps the battery gauge refreshing while otherwise asleep.

(`millis()` stays correct across light sleep — the IDF compensates for the sleep
duration — so our 1 s gauge cadence still works.)

---

## Module 5 — Design decisions that made it clean

**Sleep only on battery.** We gate sleeping behind "not on USB"
(`!PPM.isVbusIn()`, cached in the 1 s gauge read):

```cpp
bool idle = (millis() - lastActivityMs) > IDLE_SLEEP_MS;   // 400 ms
if (!onUsbCached && idle && lv_anim_count_running() == 0) enterLightSleep();
else                                                        delay(5);
```

Two payoffs: (1) it avoids native-USB re-enumeration and auto-flash headaches
while you're plugged in developing, and (2) it mirrors real devices, which sleep
aggressively only on battery. Bonus: it makes a **crisp live demo** — unplug USB
and the counter craters; replug and it returns.

**Don't sleep mid-interaction or mid-animation.** The `IDLE_SLEEP_MS` guard keeps
us awake ~400 ms after the last touch (so a drag stays smooth), and
`lv_anim_count_running() == 0` avoids napping through a bar animation.

**Defensive wake.** If a wake wasn't a real touch, we simply loop and sleep again;
`lastActivityMs` is only bumped on an actual touch (in the read callback / on a
GPIO wake). Residual pulses cost a lap, not a stall.

---

## Module 6 — Results (and what they honestly mean)

Measured on hardware, on battery, idle:

| Phase                 | CPU /s | Wakes /s |
|-----------------------|:------:|:--------:|
| On USB (baseline)     |  ~178  |    0     |
| On battery, idle      |  **1** |  **1**   |
| On battery, touching  |  ~177  |    0     |

- **178 → 1 /s**: the CPU sleeps ~99.4% of the time; the one lap/sec is the timer
  refreshing the gauge.
- **Wakes 1 /s** (timer only): the Step-2 "idle creep" is gone — it was largely
  *our own polling* provoking the controller, which stops while we sleep.
- **Touch → 177 /s, instant**: the level wake fires the moment the line goes low;
  responsiveness is unchanged.

**The honest caveat.** `loops/sec` is **CPU activity, not milliamps.** Light sleep
cuts the *CPU's* current (tens of mA → ~1–2 mA), but the **backlight and display
stay fully on**, and on this board the backlight *dominates* the power budget. So
battery life improves for real, but not by 178×. The next big levers are **dimming
/ killing the backlight when idle** (where the LTR-553 light sensor or an idle
timeout comes in) and **deep sleep** for an even lower floor. True instantaneous
current still wants an **inline USB power meter** — the on-screen counter proves
the *CPU* is resting, which was the goal.

---

## What you built and learned

- ✅ An on-screen **activity instrument** (loops/sec) and read a baseline first
- ✅ A correct **ISR** — `IRAM_ATTR`, `volatile`, no I²C — and why each matters
- ✅ Diagnosed **phantom interrupts** (floating pin → `INPUT_PULLUP`)
- ✅ **Light sleep** with **GPIO level + timer** wake sources
- ✅ **Defensive** low-power design (sleep on battery only; wake → check → re-sleep)
- ✅ Cut CPU loop activity **178 → 1 /s** with **instant touch wake**, and can say
      honestly what that does and doesn't prove about power

## Command cheat-sheet

```bash
pio run -d ~/Work/Micro/tdisplay -t upload    # -d points pio at the project dir
```

## Glossary

- **Polling** — repeatedly asking a device for status.
- **Interrupt / ISR** — hardware calls a small function when an event occurs.
- **`IRAM_ATTR`** — place code in internal RAM so it can run with flash busy.
- **`volatile`** — variable may change outside normal flow; don't cache it.
- **Floating pin / phantom interrupt** — an undriven input picks up noise → false edges.
- **Light sleep** — CPU halted, RAM/peripherals retained, resumes after the call.
- **Wake source** — what can end sleep (here: GPIO low level, or a timer).
- **Level vs edge trigger** — wake on a held level vs on a transition.

## Next lesson

Options from here: **dim/kill the backlight when idle** (the biggest remaining
power lever — pairs with the **LTR-553** light/proximity sensor for auto-brightness
and proximity-wake), or push to **deep sleep** for the lowest floor. Or move on to
other onboard peripherals (buttons, SD card).
