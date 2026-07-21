# Project Plan — T-Display S3 Pro (C++ learning)

A resumable roadmap. Each stage lists its **goal**, **approach**, **key facts**
already researched, **steps**, and **done-when** criteria. Check items off as you go.

> **Resume here:** read `docs/lesson-01-first-light.md` and `docs/lesson-02-touch.md`
> for what's already done and why. The reusable workflow lives in the
> `esp32-board-bringup` skill. Current position: **Stage 10 complete — WiFi first
> light (scan).** Press GPIO 12 → full-screen list of nearby networks (SSID/RSSI/
> lock), tap to exit; 5 found, sensible RSSI. Radio+antenna proven, scan only.
> `#include <WiFi.h>` (bundled, no lib_deps) cost flash 26%→44%. Pattern:
> `WiFi.mode(WIFI_STA); WiFi.disconnect(); WiFi.scanNetworks()` (blocks ~2-4s), read
> `SSID/RSSI(dBm, →0=stronger)/encryptionType`, then `WiFi.mode(WIFI_OFF)` (idle
> radio ~tens of mA — protects the Stage 5 power work). Triggered by the re-found
> GPIO 12 button. **The radio needs NO bus-sharing dance** (on-die, own antenna) —
> only power/RAM. Stages 1–10 complete: display, touch, LVGL, battery/PMU, power
> ladder, auto-brightness, SD, camera detect→viewfinder→capture, WiFi scan. Lesson 10
> written. **NEXT: Stage 11 CONNECT (`WiFi.begin`, needs Derek's credentials +
> STA-vs-AP decision), then Stage 12 HTTP server for the SD JPEGs (the capstone).**

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
settle; any touch advances) so a slow USB meter can settle on it. **Measured on a
dumb charger with `PPM.disableCharge()` for the run** — see the artifacts below.

| State | Meter | Δ = cost of… |
|---|---|---|
| A: poll + BL 100% | 94–120 mA ⚠ not reproducible | |
| B: poll + BL off | 74 mA | backlight ≈ 18–20 mA ⚠ |
| C: light sleep | 41 mA | **busy CPU 33 mA** |
| D: + display off (SLPIN) | 30 mA | **ST7796 11 mA** |
| E: + touch/ALS off | 29 mA | touch + ALS ≈ 1 mA |
| **F: deep sleep, all off** | **27 mA** | rest of chip ≈ 2 mA |

**≈71% of the draw is firmware-reachable; the floor is 27 mA** (regulator quiescent,
status LED, panel bias, sleeping I²C parts). No schematic hunt needed.

**FOUR measurement artifacts, all of them the apparatus not the board.** The first
run concluded "backlight isn't dominant, deep sleep saves 2 mA, 53% unreachable" —
all artifact. (1) **Battery charge current** ~35 mA, variable; the meter is upstream
of the PMU and even a full battery top-off cycles. Detected because `A − B` (a fixed
difference between two pinned duties) read 18, then 6, then 20. Fixed with
`disableCharge()`. (2) **USB host link** ~26–35 mA — the S3's PHY + enumerated link
stay alive *through deep sleep*, and it looked exactly like a hardware floor.
**Larger than the backlight.** Fixed by using a dumb charger. (3) **The
instrument's own I²C polling**, 31 mA (mode E). (4) **A defeated sleep** (also E).
⚠ High-current rows still don't reproduce (A: 94 then 120 on identical settings) —
likely SY6970 **power-path supplement mode**: the meter reads *input* current, which
equals the load only while the battery is neither supplementing nor absorbing.
Below ~40 mA everything reproduced exactly. Trust the sleep states, not A.

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

## ✅ Stage 5f — Button wake   [DONE]

**Why:** 05e proved the touch line can never work as a deep-sleep wake — the
CST226SE **holds IRQ LOW until someone reads it**, so it latches low the instant we
stop polling to sleep. A touch controller asserts precisely when nobody is listening.

**Probed before designing** (scaffolding since removed): all pins idle HIGH,
active-low, and all have **EXTERNAL pull-ups** — tested by driving an internal
`INPUT_PULLDOWN` and seeing the pin stay HIGH. ~~Board correction: only TWO buttons
exist; GPIO 12 has a pull-up but nothing attached.~~ **↳ CORRECTED 2026-07-21: the
two controls are ROCKERS (4 switches). Rocker 1 = BOOT (GPIO 0) + RESET (EN pin);
Rocker 2 = GPIO 12 + GPIO 16. ALL of GPIO 0/12/16 are real buttons, external
pull-up, RTC-capable — the first probe only pressed one side of each rocker so GPIO
12 looked dead. GPIO 12 is a usable free input.**

**Wired GPIO 16 to EXT1** (`ESP_EXT1_WAKEUP_ANY_LOW`) + a release-wait before
arming. Worked **first try** (`Boot #12 woke: button`) after four failures on the
touch line. The external pull-up let us **delete** `rtc_gpio_init`,
`rtc_gpio_pullup_en`, `rtc_gpio_hold_en` and `esp_sleep_pd_config(RTC_PERIPH, ON)` —
internal pull-ups live in a domain deep sleep powers down; a resistor doesn't.

**Bonus:** the button now advances the power bench in **every** mode, replacing a
silent timer — E and F are both dark, so you couldn't tell which one the meter was
showing. A physical input that survives everything the firmware turns off is exactly
what a bench needs.

**A probe that measured the wrong thing:** it reported zero bounce, because the edge
was detected in `loop()` (~5 ms) and the 50 ms sampling window started only after
that — bounce (1–5 ms) was already over. Left unfixed deliberately: **bounce is
harmless for a wake source**, it only matters when counting presses.

**Final bench** (charger, charging disabled): 97.5 / 71 / 38.5 / 28.3 / 27.5 / 25 mA.
Differences reproduced within 0.8 mA of the previous session — but every mode also
drifted ~2 mA, so **the RTC_PERIPH saving is unmeasurable here. Resolution limit
reached:** ≥10 mA solid, ~2 mA is drift. `A − B` has read 18/6/20/26.5 across four
runs — the backlight is "≈20 mA, ± quite a lot" and that's as good as this gets.

Lesson `docs/lesson-05f-button-wake.md` + snapshot `docs/lesson-05f-button-wake/main.cpp`.

---

## ✅ Stage 6 — microSD on the shared SPI bus   [DONE]

**The bus.** SD (CS 14) shares SCLK/MOSI/MISO with the display (CS 39). Verified
*from the driver source* before writing code: `Arduino_ESP32SPI` defaults to
`is_shared_interface=true`, calling `spiTransaction()` in `beginWrite()` and
`spiEndTransaction()` in `endWrite()` — so it re-establishes its clock/mode per
transfer and releases the bus. Both devices must be on the **same SPI host**:
`SPIClass sdSPI(FSPI)` (S3: FSPI=0=SPI2, which is what Arduino_GFX picks).

**Init order is a separate problem from run-time sharing.** Bringing the SD up
before `panel->begin()` left the **screen black** — the display can't initialise a
bus another driver already owns. Display first, then `initSD()`. A probe that must
run early can borrow and return the bus (`SD.end(); sdSPI.end();`).

**Mount:** speed ladder 20M → 400k, **fastest first**. Original version ran
slow→fast and stopped at the first success — which finds the *slowest* working
clock (reported 400 kHz ≈ 50 KB/s when the card does 20 MHz ≈ 2.5 MB/s).
Result `SDHC 61120MB@20000k`.

**Filesystem:** write→read-back self-test, plus `/boots.csv` appended once per boot
with the line count read back (`rw L3`, climbs on every reset). `FILE_APPEND` not
`FILE_WRITE` (which truncates); `File` has a bool conversion; `close()` is manual —
buffered data is lost without it. Three storage tiers now: RAM → `RTC_DATA_ATTR`
(survives deep sleep) → SD (survives power loss *and* reflashing).

**🐛 The debugging failure worth remembering.** The card wouldn't mount and the
diagnostic printed `SD.cardType()` to separate "no comms" (electrical) from
"filesystem unreadable" (formatting). It reported `t=0` = CARD_NONE, so formatting
was ruled out and four electrical hypotheses were chased. **`cardType()` reads the
MOUNTED card struct — it returns 0 for ANY `begin()` failure.** The field could
never distinguish the two cases. Reformatting FAT32 fixed it instantly (>32 GB cards
ship **exFAT**; the ESP32 `SD` library mounts only FAT16/32). *A diagnostic is worth
exactly what its derivation is worth — reading the driver source made the bus work
first time; assuming a return value's meaning cost four wrong turns.* Third
instrument in two stages to measure the wrong thing.

**Board notes:** LilyGO ship **no SD example** (16 examples, none for SD), and their
`utilities.h` lists **GPIO 16 as both a user button and `VIBRATING_MOTOR`** — we only
ever *read* it; driving it as an output would buzz a motor.

Lesson `docs/lesson-06-sdcard.md` + snapshot `docs/lesson-06-sdcard/main.cpp`.

---

## ✅ Stage 7 — Camera shield: detect the sensor   [DONE]

**Result:** `CAMERA OK: sensor PID 0x5640` — an **OV5640** (5 MP), on a
ribbon-cable module (shielded, no readable markings). Detection only; no capture yet.

**The instrument.** `esp_camera_init()` (from `esp_camera.h`, **bundled with the S3
core — no `lib_deps`**) powers the sensor, clocks XCLK through the proper peripheral,
talks real SCCB, detects by PID, and returns a specific `esp_err_t`. Config: pins
from `CameraShield/utilities.h` (PWDN 46, XCLK 11, PCLK 2, VSYNC 7, HREF 15, D0-D7 =
45/41/40/42/1/3/10/4, SIOD/SIOC = the I²C pins 5/6); `ledc_timer=1`/`channel=2` to
dodge the backlight's LEDC; `FRAMESIZE_QVGA`, `fb_count=1`, `CAMERA_FB_IN_PSRAM`
(first real PSRAM use). PID `0x5640` = OV5640 — **the vendor example implies OV2640;
pin map matched, sensor didn't.**

**🐛 Four probes; the three hand-rolled ones each measured the wrong thing.** An
I²C scan that toggled PWDN and generated XCLK with `ledcAttach(pin, 20MHz, 2)`. It
printed "no camera" four rounds running while being **physically incapable of ever
printing anything else**, for two independent reasons: (1) `ledcAttach` **failed** —
20 MHz needs an LEDC divider of ~0 (`div_param=0`, past the peripheral's limit) and
its return was never checked, so the clock never ran; LEDC is a PWM generator, not a
clock source. (2) Even clocked, **SCCB doesn't ACK like I²C**, so `Wire` reads it as
absent. We refined the broken instrument (PWDN polarity sweep, longer settle) before
reaching for the driver. **Removed the hand-rolled probe** (post-mortem kept in a
code comment): a diagnostic that always lies is worse than none.

**🐛 The probe broke the bus it borrowed (mirror of Stage 6's black screen).** The
camera driver installs its own SCCB master on I²C pins 5/6 — touch/PMU/ALS live
there. After the probe the **gauge froze on "Charging" off USB** (stale reads).
`esp_camera` uses I²C **port 1**, `Wire` uses **port 0**, same pins via the GPIO
matrix; `esp_camera_deinit()` leaves the pins muxed to the dead port 1, and
`Wire.begin()` is **idempotent** (early-returns, never re-muxes). Fix:
`Wire.end(); Wire.begin(); initSharedBusSensors()` — factored the three sensor
inits into one helper called at setup **and** post-probe. *`begin()` is not always
re-entrant; `end()` first to force a redo.*

**Serial visibility (why all this was invisible).** `pio device monitor` showed
nothing: native-USB CDC re-enumerates on reset, so `setup()`'s one-shot logs fly
past before the monitor reconnects, and the `if (Serial)` guards skip them. Fix:
`while (!Serial && (millis()-t0) < 1500) delay(10)` — wait for the host **with a
timeout** so a battery boot doesn't hang. The moment the log was readable it exposed
the LEDC bug. **Benign remaining `[E]` noise:** `Wire … already initialized` (each
driver re-calls `Wire.begin`) and `SPI_MASTER_MISO pin 8` (SD shares the display's
MISO, Stage 6) — core `log_e`, silence-able with `-DCORE_DEBUG_LEVEL=0` but left on.

Lesson `docs/lesson-07-camera.md` + snapshot `docs/lesson-07-camera/main.cpp`.

---

## ✅ Stage 8 — Live viewfinder   [DONE]

**Result:** tap the on-screen **"Cam"** button → a **live camera image** on the
ST7796; press the **physical GPIO 16 button** to exit back to the gauge. Correct
colours, right-side up (confirmed by eye). First light for the camera *pipeline*.

**The pipeline** (blocking loop, takes over the screen): `esp_camera_fb_get()` →
`panel->draw16bitBeRGBBitmap(ox, oy, fb->buf, fb->width, fb->height)` →
`esp_camera_fb_return(fb)` (recycle or the queue starves). Streaming config vs
Stage 7's detection: `fb_count=2`, `grab_mode=CAMERA_GRAB_LATEST`,
`frame_size=FRAMESIZE_QCIF`.

**Frame size = QCIF 176×144.** The panel is 222 wide; QCIF is the largest standard
size that **fits with no scaling, cropping or rotation** — centred at ox=23, oy=168.
First-light discipline: smallest correct thing on the glass, refine later.

**The two things always wrong first (both eye-only diagnosable):** (1) **byte
order** — esp_camera RGB565 is **big-endian** vs the panel; fix is choosing the
matching push `draw16bitBeRGBBitmap` (not `draw16bitRGBBitmap`), not a swap loop.
(2) **orientation** — image came up inverted; fixed in the **sensor** via
`esp_camera_sensor_get()->set_vflip(s,1)` (free, done during readout — not a
software buffer flip). `set_hmirror` is the other half if a module is mounted a
full 180°.

**A greedy peripheral dictates the controls.** SCCB is on the shared I²C pins 5/6,
so touch + the gauge are **frozen while streaming** — not a bug, a constraint.
**Enter via the touch "Cam" button, EXIT via the physical GPIO 16 button** (the
Stage 5f primitive that survives everything the firmware takes over, earning its
keep again). The loop blocks the main loop; fine, because `esp_camera_fb_get()`
blocks on a semaphore that yields to the RTOS (nothing starves, no watchdog).

**Reuse:** factored Stage 7's config into `cameraFillPins(cfg)` (pin map + XCLK +
LEDC-away-from-backlight) and its recovery into `cameraRecoverBus()`
(`Wire.end(); Wire.begin(); ` + re-init touch/PMU/ALS) so probe and viewfinder can't
drift. On exit, repaint the clobbered LVGL screen (`fillScreen` → `lv_obj_invalidate`
→ `lv_refr_now`) — never illuminate a buffer you haven't drawn (Stage 5e). A **third
button** now shares the bottom row (68 px each) — the pressure that argues for LVGL
flex/grid.

Lesson `docs/lesson-08-viewfinder.md` + snapshot `docs/lesson-08-viewfinder/main.cpp`.

---

## ✅ Stage 9 — Capture a still to SD (JPEG)   [DONE]

**Result:** in the viewfinder, **short-tap GPIO 16 → saves the frame as
`/IMG_NNNN.JPG`** (green filename shown under the image), **long-hold → exits**.
Confirmed: green filename on tap, hold exits, JPEG written to the card.

**No bus conflict — three subsystems, three buses:** camera fills the frame over the
**DVP parallel** bus, `frame2jpg` encodes on **CPU** (+PSRAM), the SD write is
**SPI** (shared with the display but bracketed per transfer, Stage 6). SCCB/I²C isn't
touched during a capture. *Before assuming two ops conflict, map each to its actual
bus.*

**Encode + ownership:** `#include "img_converters.h"` (bundled, no lib_deps);
`frame2jpg(fb, 80, &jpg, &jpgLen)` mallocs `jpg` → **we `free(jpg)`** (the C
ownership rule again, after `fb_return` and `File::close()`). QCIF@q80 ≈ 5–8 KB vs
~50 KB raw.

**Next filename from the card itself:** `static uint16_t imgSeq`; loop
`snprintf("/IMG_%04u.JPG") ; if(!SD.exists) break; imgSeq++` — first gap = next name,
scans past existing files on the first capture after a reboot, so numbering never
collides across power cycles/reflashes.

**One button, two jobs by DURATION:** short tap (<700 ms, ≥40 ms debounce) = capture
on RELEASE; long hold (≥700 ms) = exit the moment the threshold is crossed (no need
to release). Feedback is direct GFX text BELOW the image (`drawViewfinderMsg`) — live
frames only cover the image rect, so the status line persists until the next capture.

**Note:** stills are viewfinder-resolution (QCIF, "save what you see"); higher-res
would need a sensor reconfigure mid-session. Capture briefly freezes the live image
(~100–300 ms encode+write) — expected, GRAB_LATEST discards the frames that filled.

Lesson `docs/lesson-09-capture-sd.md` + snapshot `docs/lesson-09-capture-sd/main.cpp`.

---

## ✅ Stage 10 — WiFi first light (scan)   [DONE]

**Result:** press the **GPIO 12 button** → a full-screen list of nearby WiFi
networks (SSID, RSSI, lock), tap to exit. Confirmed: 5 networks found, sensible RSSI
(-40 near … -76 far). Radio + antenna proven. Scan only — no connection yet.

**The radio needs NO bus-sharing dance** (unlike camera=I²C, SD=SPI): it's on-die
with its own antenna, touches none of our pins — no recovery helper, no init-order
trap. The one cost is **weight**: `#include <WiFi.h>` took flash **26%→44%** (~600 KB
TCP/IP + supplicant) and RAM 33%→42%.

**Scan = the radio's "hello world"** — no credentials, no STA/AP decision (those come
at connect time). `WiFi.mode(WIFI_STA); WiFi.disconnect(); n = WiFi.scanNetworks()`
(BLOCKS ~2-4 s); per network `WiFi.SSID(i)` / `RSSI(i)` (dBm, closer to 0 = stronger)
/ `encryptionType(i)`. **Radio OFF after** (`WiFi.scanDelete(); WiFi.mode(WIFI_OFF)`)
— an idle radio draws tens of mA and would undo the Stage 5 power work. Triggered by
the **GPIO 12 button just re-found in the rocker correction** — earning its first job.

**Debugging aside:** the scan build's first boot also showed SD `f_mount failed: (3)`
×5. Ruled out the new code by **timing** (SD mounts in setup, before any WiFi runs)
and by **error KIND** (`FR_NOT_READY` = block-level comms, not a format error) →
physical: the card had been pulled to view JPEGs and wasn't reseated. Reseat → clean
boot. (Contrast Stage 6's meaningless `SD.cardType()`; this error was specific.)

Lesson `docs/lesson-10-wifi-scan.md` + snapshot `docs/lesson-10-wifi-scan/main.cpp`.

---

## ▶ Next — pick by interest   ← NEXT
- **📡 Stage 11 — CONNECT to WiFi:** `WiFi.begin(ssid,pass)` → wait `WL_CONNECTED` →
  show the IP. **Needs Derek's credentials + the STA-vs-AP decision** (join home
  network vs board-as-hotspot). Then **Stage 12 — HTTP server** serving the SD card's
  JPEGs to a phone (ties camera+SD+radio together — the capstone).
- Smaller camera refinements still open: higher-res stills (sensor reconfigure),
  scale/rotate the viewfinder to fill the screen.
- **Log the battery gauge to CSV** — the card is a logging destination and the gauge
  is already sampling once a second. Natural next build.
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
- Other onboard peripherals: physical buttons — **2 rockers / 4 switches: BOOT(0)+
  RESET(EN) and GPIO 12 + GPIO 16, all three GPIOs real & usable** (re-probed
  2026-07-21) — SD card (SPI CS 14).
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
