# Project Plan — T-Display S3 Pro (C++ learning)

A resumable roadmap. Each stage lists its **goal**, **approach**, **key facts**
already researched, **steps**, and **done-when** criteria. Check items off as you go.

> **Resume here:** read `docs/lesson-01-first-light.md` and `docs/lesson-02-touch.md`
> for what's already done and why. The reusable workflow lives in the
> `esp32-board-bringup` skill. Current position: **Stage 5e complete — inline USB
> power meter. A "Bench" button walks five held power states (A poll+BL100 / B
> poll+BLoff / C light sleep / D +display off / E deep sleep timer-only) so a slow
> meter can settle. Real numbers at last: 129 / 111 / 80 / 71 / 78 mA, RESET floor
> 66 mA. Findings: the backlight is NOT dominant (18 mA) — a busy CPU costs 31 mA;
> deep sleep saves only 2 mA over light sleep and is 12 mA WORSE than holding RESET
> (the price of wake-on-touch); and ~69 mA (53%) is not reachable from firmware and
> remains UNEXPLAINED. Shipped win: ST7796 SLPIN via panel->displayOff() in the idle
> path (9 mA) — we'd only ever turned off the backlight. Lesson 05e written.
> OPEN BUGS: (1) deep-sleep touch wake fires instantly (`woke: touch`) — three
> hypotheses tried and all failed; next step is to BISECT, not guess. (2) the
> unexplained 69 mA. Next up (pick by interest): those two, or buttons / SD card /
> LVGL flex-grid refactor.**

---

## ✅ Done
- [x] **Setup** — serial permissions (`uucp`), PlatformIO installed, modern ESP32
      core 3.x (pioarduino).
- [x] **Stage 1 — Hello serial** — compile → flash → read loop proven.
- [x] **Stage 2a — First light** — Arduino_GFX driving the ST7796; color bars + text.
- [x] **Stage 2b — Touch (CST226SE)** — SensorLib `TouchDrvCSTXXX`; dots track the
      finger; orientation calibrated to identity (no swap/mirror in portrait).
      Input uses **IRQ-gated polling (design B)** via `touch.isPressed()` on GPIO 21
      — see decision below.
- [x] **Stage 2b polish** — on-screen CLEAR button (hit-testing + edge detection);
      fixed a native-USB `Serial` freeze/lag (see bug note below).
- [x] Lesson docs 01 + 02 written; `esp32-board-bringup` skill created & updated.
- [x] **Stage 2c — Smooth touch trail** — measured the sample rate (Module 8:
      `delay(10)`→16 Hz, `delay(2)`→40 Hz, `delay(0)`→~77 Hz), then fixed the
      speed-dependent gaps with **linear interpolation** (`drawStroke` connects
      consecutive touch reports). Uses a **timeout** to track a continuous stroke
      because the IRQ pulses (Module 9); settled on `delay(2)` (~40 Hz).
- [x] **Stage 3 — Interactive Tap Counter** — reusable `Button` struct + generic
      `inside()`/`drawButton()`; live `counter` state; correct **one-per-tap edge
      detection** (debounced press/release, using the controller's **touch-up
      event** — a `count == 0` frame — for crisp release, timeout as fallback);
      pressed-while-held feedback via a `Button *activeBtn`. Lesson 03 written +
      snapshot in `docs/lesson-03-interactive/`.
- [x] **Stage 3b — Framebuffer fading trail** — enabled **octal PSRAM**
      (`memory_type = qio_opi` + `BOARD_HAS_PSRAM`); `Arduino_Canvas` framebuffer
      in PSRAM; time-based fading trail (ring buffer + per-point timestamps).
      **Performance investigation:** full-frame `flush()` ≈ 40 ms (~22 fps); the
      SPI path is CPU-polled (no DMA) and caps ~40 MHz, so raising the clock did
      nothing. Fixed under-sampling (poll `getPoint()` directly) + size-popping
      (constant radius). Fast-motion stutter is the accepted full-frame ceiling.
      Lesson 03b + snapshot in `docs/lesson-03b-fading-trail/`.

### Bug fixed (2026-07-14) — native-USB serial freeze/lag
`Serial.print` on the ESP32-S3's native USB **blocks** when the TX buffer fills
and no host drains it → the loop froze after ~15 touches with no monitor attached.
`if (Serial)` guards weren't enough (the port reads "connected" even when no app
is reading). Real fix: **`Serial.setTxTimeoutMs(0)`** makes writes drop instead of
wait. Reusable on any native-USB ESP32-S3/C3 board. (Now in the skill playbook.)

### Design decision (2026-07-14) — touch input: polling vs interrupt
Evaluated three designs (see `docs/lesson-02-touch.md` Module 6):
**A** pure polling (I²C every loop), **B** IRQ-gated polling (cheap GPIO-21 read
gates the I²C read, via `isPressed()`), **C** true hardware interrupt + ISR flag.
**Chose B now** — removes idle-bus spam with zero added complexity. **Deferred C**
to a future **low-power lesson**: its payoff is letting the CPU *sleep* and wake on
touch, which only matters once running on battery (SY6970 PMU, Stage 5).

### Investigation (2026-07-16) — touch sample rate vs dot spacing
Observation: faster drags leave wider gaps between dots. Measured the sample rate
by turning the **display into an instrument** (on-screen Hz readout — serial was
flaky). Results, sweeping the loop `delay`: **`delay(10)` → 16 Hz, `delay(2)` →
40 Hz, `delay(0)` → ~77 Hz.** Rate is flat across drag speeds (gaps are pure
geometry: `gap = speed × interval`) and keeps climbing as we poll faster (our loop
cadence was the bottleneck, not a single controller wall — the IRQ is a brief
pulse we were partly missing). Even at 77 Hz a fast swipe still gaps, so polling
alone can't win. Full write-up in `docs/lesson-02-touch.md` Module 8. Fix =
Stage 2c below. (`delay(0)` also spins the CPU at 100% — a power cost; the true
fix for rate *and* power is design C, deferred to the low-power lesson.)

---

## ✅ Stage 2c — Smooth the trail (sample rate + interpolation)   [DONE]
**Goal:** eliminate the gaps a fast drag leaves, so the trail looks continuous at
any finger speed.

**Approach (two levers, see Module 8):**
- **Interpolation (the real fix):** track the previous touch point and
  `gfx->drawLine(prevX, prevY, x, y, …)` instead of an isolated `fillCircle`.
  Start a **fresh stroke** on each new touch (reset `prevX/prevY` when a touch
  begins) so we don't draw a line across the screen from the last tap's end.
  Consider a thicker stroke (draw a small filled circle at each end, or parallel
  lines) for a nicer pen.
- **Sample rate:** replace `delay(10)` with a balanced value (~`delay(2)`, ≈40 Hz)
  — a real, cheap responsiveness win — rather than the power-hungry `delay(0)`.

**Also:** remove the Module 8 measurement scaffolding (on-screen Hz readout,
timing globals, `VERBOSE_TOUCH`) and re-snapshot `docs/lesson-02-touch/main.cpp`.

**Done when:** a fast swipe draws a continuous line (no visible dot gaps), and the
firmware is back to a clean, shippable state.

> **Deferred within this theme:** the *true* answer for both sample rate and power
> is **design C** (hardware interrupt on GPIO 21 — catches every IRQ pulse, lets
> the CPU sleep). It stays in the low-power lesson (Stage 5); Stage 2c gets the
> smooth result now with interpolation + a modest delay.

---

## ✅ Stage 3 — Small interactive app (Tap Counter)   [DONE]
**Goal:** combine display + touch into event-driven behavior.

**Built:** a Tap Counter — reusable `Button` struct, generic `inside()` /
`drawButton()`, a live `counter`, and correct **one-per-tap** edge detection.
Getting the edge detection right was the meat (see lesson 03 Module 3): the IRQ
pulses so `isPressed()` can't be trusted directly → debounced press/release state
→ used the controller's **touch-up event** (`count == 0` frame) for crisp release,
with a timeout fallback. Pressed-while-held feedback via `Button *activeBtn`.

**Done:** taps register one-for-one (fast tapping included), holds are solid,
buttons highlight while pressed. Lesson `docs/lesson-03-interactive.md` +
snapshot `docs/lesson-03-interactive/main.cpp`.

---

## ✅ Stage 3b — Retained-mode framebuffer (Arduino_Canvas in PSRAM)   [DONE]
**Goal:** stop drawing straight to the panel; keep a **saved scene** in memory and
push it to the display each frame. Unlocks effects that immediate mode can't do
cleanly: easy clear, **fading / expiring dots** (the "living trail" we deferred),
smooth animation, and flicker-free redraws.

**Why now:** the "clunky" feel in Stage 2b came from immediate-mode drawing. This
board has **8 MB PSRAM** sitting idle — a 222×480×2-byte framebuffer is ~213 KB,
trivially affordable. This is also exactly how LVGL works, so it's the natural
bridge to Stage 4.

**Approach:** wrap the ST7796 in an `Arduino_Canvas` (framebuffer allocated in
PSRAM); draw into the canvas; call `flush()` to blit. Then implement the
expiring-dot trail (per-dot timestamps, redraw scene each frame).

**Done when:** dots fade/expire smoothly and the screen redraws without flicker.

---

## ✅ Stage 4 — Real UX with LVGL   [DONE]
**Goal:** build a proper widget-based interface (buttons, labels, events).

**Decisions made:** **LVGL v9** (resolved to 9.5.0; current API over LilyGO's
pinned v8) and **hand-coded** UI (over a SquareLine/EEZ designer) — the learning
project wants the fundamentals.

**Built (4 incremental steps, each a checkpoint):**
1. **Build integration** — `lvgl/lvgl @ ^9.3.0` + a minimal `include/lv_conf.h`
   found via `-DLV_CONF_INCLUDE_SIMPLE` and `-I include`. Proved `lv_init()`
   compiles/links in isolation (version shown on-screen).
2. **Flush callback** — `my_flush_cb` pushes LVGL's dirty rectangles to the panel
   via `panel->draw16bitRGBBitmap` (`RENDER_MODE_PARTIAL`). Colour/byte order
   verified correct with R/G/B test bars (no swap needed).
3. **Read callback** — `my_touch_read_cb` reports CST226SE press state + point
   (held on release). LVGL does hit-testing / debouncing / click detection.
4. **Widget app** — a Tap Counter: `lv_button_create` + `lv_obj_add_event_cb(...,
   LV_EVENT_CLICKED, ...)`. Fires once per tap; fast taps register, holds don't
   run away — the Stage-3 edge-detection battle, now free.

**Bug fixed — draw-buffer alignment Heisenbug:** step 3 hung inside
`lv_display_set_buffers` (the same call that worked in step 2). Cause: the plain
`static uint8_t drawBuf[]` was only 1-byte-aligned; adding the touch driver's
statics shifted it onto a misaligned address, tripping LVGL's alignment assert
(`while(1)` hang). Diagnosed via on-screen boot markers (serial dies on
native-USB reset). Fix: `__attribute__((aligned(64)))`.

**Done:** LVGL Tap Counter responds to touch; confirmed on hardware. Lesson
`docs/lesson-04-lvgl.md` + snapshot `docs/lesson-04-lvgl/` (main.cpp + lv_conf.h).

---

## ✅ Stage 5a — Battery gauge (SY6970 PMU)   [DONE]
**Goal:** an on-screen battery instrument to visualise power for the low-power work.

**Built:** brought up the **SY6970** charger/power-path IC (XPowersLib) on the
**shared I²C bus** (0x6A, alongside touch's 0x5A — no extra wiring). `PPM.init(Wire,
5, 6, SY6970_SLAVE_ADDRESS)` + `enableMeasure()` (ADC on), then read
`getBattVoltage/getVbusVoltage/getChargeCurrent/isVbusIn/isCharging`. Step 1 = raw
text dump (proved comms: VBUS 5100 mV, Batt 4124 mV, charging). Step 2 = an **LVGL
gauge** (`lv_bar` with track/indicator parts + colour by level, % + voltage +
status), refreshed 1 Hz. **% is derived from voltage** via a piecewise LiPo curve —
NOT a true fuel gauge. Demonstrated the caveat live: 97% on USB → 90% on unplug
(charge current inflates terminal voltage). Board keeps running on battery.

**Gotcha:** `XPowersPPM` alias only exists if you `-D XPOWERS_CHIP_SY6970`; without
it the header's `#else` branch defines no alias → *"XPowersPPM not declared."* Fix:
use the concrete class `PowersSY6970`. Lesson `docs/lesson-05-battery.md` + snapshot
`docs/lesson-05-battery/main.cpp`.

---

## ✅ Stage 5b — Low-power lesson (design C)   [DONE]
**Goal:** CPU **light-sleeps** when idle and **wakes on touch**, instead of
busy-polling ~178×/sec. The "design C" deferred since Lesson 02 Module 6.

**Built (3 steps):** (1) an on-screen **activity instrument** (`loops/sec`) to
read a baseline — ~178/s, constant even untouched. (2) **`attachInterrupt`** on
GPIO 21 (`IRAM_ATTR` ISR, `volatile` flag) — proved the touch line fires; found
**phantom idle edges** → fixed the floating pin with `INPUT_PULLUP` (a small
residual creep remained = controller heartbeat + our own polling). (3)
**`esp_light_sleep_start()`** with **GPIO level-LOW + 1 s timer** wake sources;
gated to sleep **only on battery** (`!isVbusIn`, cached) to dodge native-USB
re-enumeration and mirror real devices; `IDLE_SLEEP_MS`=400 guard + no-sleep-
during-animation. Level wake (not edge) so we dropped the edge ISR to avoid
fighting over the pin's interrupt type.

**Result (on battery, idle):** CPU **178 → 1 /s** (~99.4% less loop activity),
**Wakes 1/s** (timer only — the idle creep vanished because we stop polling while
asleep), touch **instant + responsive**, returns to 178 on touch/replug.

**Honest caveat:** loops/sec = CPU activity, NOT mA. Light sleep cuts CPU current
(tens of mA → ~1–2 mA) but the **backlight dominates** and stays on — real battery
gain, not 178×. Next levers: dim/kill backlight when idle (LTR-553 / timeout),
deep sleep, inline USB power meter for true mA. Lesson `docs/lesson-05b-low-power.md`
+ snapshot `docs/lesson-05b-low-power/main.cpp`.

---

## ✅ Stage 5c — Backlight power + LTR-553 auto-brightness   [DONE]
**Goal:** attack the biggest remaining power draw (the backlight) with PWM dimming,
auto-brightness from ambient light, and screen-off-when-idle.

**Built (3 steps):** (1) **PWM backlight** via LEDC core-3.x API
(`ledcAttach(TFT_BL, 5000, 8)` + `ledcWrite(pin, duty)`) — dimmable, flicker-free.
(2) **LTR-553** light+proximity sensor (SensorLib `SensorLTR553`, I²C **0x23** — a
3rd chip on the shared SDA5/SCL6 bus): `enableLightSensor()` + `enableProximity()`,
read `getLightSensor(0)` / `getProximity()`. (3) **Auto-brightness** (constrain+map
ambient→duty, EMA smoothing, readability floor) + **screen-off idle timeout**
(`BL_OFF_MS`=5 s) waking on **touch or proximity** (PS is short-range, ~3 cm /
cover; `PROX_WAKE`=30).

**Key interaction:** light-sleep **only when the screen is off** — a PWM backlight's
clock gates during light sleep, glitching mid-level duty; duty 0 (steady low) is
glitch-free, and that's when the big win lands (screen dark + CPU asleep together).
**Observability fix:** the CPU counter is on the screen we turn off, so we **capture
the idle loops/sec while asleep** and show it on wake — `idle:1` on battery,
`idle:178` on USB (the battery-gate made visible). Lesson
`docs/lesson-05c-backlight.md` + snapshot `docs/lesson-05c-backlight/main.cpp`.

---

## ✅ Stage 5d — Deep sleep   [DONE]
**Goal:** the deepest power state — power down to the RTC domain (~tens of µA),
wake by rebooting.

**Built:** a **"Deep Sleep" button** (kept the always-on gauge's light-sleep idle
behaviour intact). Tapping it → `esp_deep_sleep_start()`. Wake sources: **EXT1** on
the touch line (GPIO 21, an RTC-capable pad; S3 dropped EXT0) via
`ESP_EXT1_WAKEUP_ANY_LOW` + RTC pull-up, and a **20 s timer**. On wake the chip
**reboots** (`setup()` re-runs, all peripherals re-init). A **`RTC_DATA_ATTR`
bootCount** survives (proven: climbed 1→2→3 while RAM was wiped) and
`esp_sleep_get_wakeup_cause()` reports the reason (**woke: touch / timer** — both
confirmed). Backlight (GPIO 48, non-RTC) driven low + `gpio_hold_en` /
`gpio_deep_sleep_hold_en` so it stays off through sleep (released on next boot).
Waited for finger-release before sleeping to avoid instant re-wake.

**Honest note:** ~tens of µA is below what the on-board SY6970 can show; the proof
is behavioural (reboot + surviving counter + wake cause). Real µA needs an inline
USB meter. Lesson `docs/lesson-05d-deep-sleep.md` + snapshot
`docs/lesson-05d-deep-sleep/main.cpp`.

**Power ladder complete:** busy-poll (~178 loops/s) → light sleep (resume, ~mA) →
deep sleep (reboot, ~µA).

---

## ✅ Stage 5e — Inline USB power meter   [DONE]

A "Bench" button holds one power state still (announce 4 s at full brightness, then
settle; any touch advances) so a slow USB meter can settle on it.

| State | Meter | Δ = cost of… |
|---|---|---|
| A: poll + BL 100% | 129 mA | |
| B: poll + BL off | 111 mA | backlight **18 mA** (14%) |
| C: light sleep | 80 mA | busy CPU **31 mA** (24%) |
| D: + display off (SLPIN) | 71 mA | ST7796 scanning **9 mA** (7%) |
| E: deep sleep (timer only) | 78 mA | rest of chip ≈ **2 mA** (display still awake) |
| RESET held | 66 mA | ESP32 running at all ≈ 12 mA |
| unexplained floor | ~69 mA | **53% — not reachable from firmware** |

**What this overturned:** the backlight is *not* dominant (a busy CPU costs ~2×);
deep sleep buys 2 mA over light sleep and is 12 mA *worse* than holding RESET (the
price of keeping `ESP_PD_DOMAIN_RTC_PERIPH` powered for wake-on-touch); and Stages
5b–5d optimised ~47% of the problem — **loops/sec cannot see a load that isn't the CPU.**

**Shipped:** `applyScreenPower()` now sends ST7796 `SLPIN` (`panel->displayOff()`)
alongside the backlight in the idle path — 9 mA we'd been leaving on the table since
05c. Order matters: dark before SLPIN; `lv_obj_invalidate` + `lv_refr_now` before
lighting back up (SLPIN discards the frame buffer).

**One confusion, three bugs.** We'd conflated "the backlight" (a PWM pin) with "the
display" (a separate chip with its own power state and memory) since Stage 1. That
caused: 9 mA wasted while "off", **white noise on wake**, and **white noise on
power-on**. Wake fix: `displayOn()` → `fillScreen(RGB565_BLACK)` → invalidate +
`lv_refr_now` → *then* backlight. Sequencing the repaint to beat the backlight was a
**race** (~40 ms full-frame, non-DMA SPI); wiping the random GRAM is a guarantee —
*prefer eliminating a bad state over sequencing around it.* Boot fix: `setup()` was
doing `setBacklight(autoBrightness)` **before** `panel->begin()`; backlight now held
at 0 through bring-up and raised only after `lv_refr_now()` renders the first frame.
**Rule: never illuminate a buffer you haven't drawn.** Tradeoff: ~1 s of black at
boot instead of noise — a splash screen after `fillScreen` would cover it.

**Also:** screen auto-off now gated on `!onUsbCached` (it made slow values
impossible to watch while tethered); buttons moved to one row after the Bench button
was placed on top of `bootLabel` — `lv_obj_align` has no collision detection.

**Caveat on the RESET floor:** it is *not* a hardware floor. Holding RESET stops the
ESP32 but leaves every other chip in whatever state firmware left it — the ST7796
was still scanning. Mode D (CPU alive, display asleep) nearly matches it.

Lesson `docs/lesson-05e-power-meter.md` + snapshot `docs/lesson-05e-power-meter/main.cpp`.

---

## ▶ Next — pick by interest   ← NEXT
- **🐛 OPEN: deep-sleep touch wake fires instantly** (`woke: touch`). Three
  hypotheses tried, all failed — heartbeat (line measured perfectly quiet, 500 ms =
  the function's minimum), missing `rtc_gpio_init()`, and `RTC_PERIPH` powered down
  + floating `TOUCH_RST`. All three are correct necessary fixes and are now in the
  code; none was the bug. **Next step is to BISECT, not guess:** arm EXT1 with no
  pull-up configured at all, and separately on an unconnected pin, to prove whether
  the wake comes from the pin's electrical state or the wake source's config.
  Timer-only deep sleep (bench mode E) works fine.
- **🔍 OPEN: the unexplained ~69 mA** (53% of total draw). Too much for a power LED
  + regulator quiescent. Needs the schematic and a meter on the 3.3 V rail.
- **Flash hazard worth fixing:** a sleeping CPU doesn't service USB CDC, so upload
  fails with `OSError: [Errno 71] Protocol error` (recover: RESET, or BOOT+RESET).
  A short "flash window" at the top of `setup()` before any sleeping would prevent it.
- Other onboard peripherals: physical buttons (GPIO 0/12/16), SD card (SPI CS 14).
- Circle back to UI: a richer LVGL screen / multiple screens. The gauge screen now
  hand-aligns ~9 widgets by absolute y-offset and has already produced one
  invisible-overlap bug — a good argument for LVGL flex/grid containers.
- Small LVGL exercise still open: swap the `millis()` throttle for `lv_timer_create`.

---

## Stage 5 — Other onboard peripherals (optional, pick by interest)
- **LTR-553** light + proximity sensor (I²C) via SensorLib.
- ~~**SY6970** power/battery management (I²C `0x6A`) via XPowersLib~~ — DONE (5a).
- **Buttons** (GPIO 0, 12, 16) and **SD card** (SPI, CS 14).
- **Low-power lesson (design C):** convert touch to a true hardware interrupt
  (`attachInterrupt` on GPIO 21 → `volatile` flag → read in `loop()`) and let the
  CPU light-sleep, waking on touch. Deferred here because the payoff is battery
  life, so it pairs naturally with the SY6970 PMU work. See lesson 02 Module 6.

---

## Cross-cutting (do whenever convenient)
- [ ] `git init` this project for version control (not yet a git repo).
- [ ] Fix editor IntelliSense red squiggles — generate `compile_commands.json`
      (`pio run -t compiledb`) and point clangd at it.
- [ ] Write a `lesson-NN-*.md` doc after each stage (Stage 2b = lesson 02).
- [ ] Add any new gotchas to the `esp32-board-bringup` skill's playbook so it
      compounds in value.

---

## How to resume in a new session
1. `cd ~/Work/Micro/tdisplay`
2. Skim this file + `docs/lesson-01-first-light.md`.
3. Rebuild/flash the current firmware to confirm the board still works:
   `pio run -t upload` then read serial.
4. Say e.g. *"let's do Stage 2b (touch)"* and continue.
