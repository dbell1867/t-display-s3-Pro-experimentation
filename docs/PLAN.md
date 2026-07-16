# Project Plan — T-Display S3 Pro (C++ learning)

A resumable roadmap. Each stage lists its **goal**, **approach**, **key facts**
already researched, **steps**, and **done-when** criteria. Check items off as you go.

> **Resume here:** read `docs/lesson-01-first-light.md` and `docs/lesson-02-touch.md`
> for what's already done and why. The reusable workflow lives in the
> `esp32-board-bringup` skill. Current position: **Stage 2c complete — smooth
> touch trail via interpolation (sample-rate investigated in Module 8, fix in
> Module 9). Touch is now polished end to end. Next up: Stage 3 (small
> interactive app), or jump to Stage 3b (framebuffer) for the fading-trail
> effect.**

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

## ▶ Stage 3 — Small interactive app   ← NEXT
**Goal:** combine display + touch into event-driven behavior.

**Approach:** draw an on-screen "button" (a filled rect + label); detect when a
touch falls inside its bounds; change its color / increment a counter on tap.
Teaches hit-testing and simple state, all with primitives (no UI framework yet).

**Done when:** tapping the on-screen button visibly reacts.

> Note: Stage 2b's CLEAR button already previews the core of Stage 3 (hit-testing
> + edge detection). Stage 3 can go further: multiple buttons, on-screen state
> (a counter), pressed/return visual states.

---

## Stage 3b — Retained-mode framebuffer (Arduino_Canvas in PSRAM)
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

## Stage 4 — Real UX with LVGL
**Goal:** build a proper widget-based interface (buttons, sliders, labels, screens).

**Approach:** add LVGL; wire two glue callbacks — a *flush* callback that pushes
LVGL's pixels via Arduino_GFX, and a *read* callback that feeds it CST226SE touch.
Then build a screen with real widgets. Optionally design visually in SquareLine or
EEZ Studio and export.

**Open decisions to make here:**
- LVGL **v8** (matches LilyGO's pinned `8.3.1` examples) vs **v9** (current).
- Use a GUI designer (SquareLine / EEZ) or hand-code the UI?

**Done when:** an LVGL button/slider on screen responds to touch.

---

## Stage 5 — Onboard peripherals (optional, pick by interest)
- **LTR-553** light + proximity sensor (I²C) via SensorLib.
- **SY6970** power/battery management (I²C `0x6A`) via XPowersLib — read battery %.
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
