# Lesson 03b — A Fading Trail, and the Cost of a Framebuffer

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Switch from **immediate-mode** drawing to a **retained-mode
framebuffer** (in PSRAM), build the fading touch-trail we deferred back in Lesson
02 — and, just as importantly, *measure what it costs* and learn where the
hardware ceiling is.

> Continues from [Lesson 03](lesson-03-interactive.md). This is where "just redraw
> the whole screen every frame" becomes possible — and where we meet its price.

---

## Learning objectives

By the end of this lesson you can:

1. Explain **immediate mode vs retained mode** and when each is worth it.
2. **Enable PSRAM** on an ESP32-S3 (octal / OPI) and verify it at runtime.
3. Wrap a display in an **`Arduino_Canvas`** framebuffer and `flush()` it.
4. Build a **time-based animation** (a fading trail via per-point timestamps).
5. **Measure a rendering bottleneck** and reason about why a "fix" (raising the
   SPI clock) does or doesn't help.
6. Recognise a genuine **hardware/bandwidth ceiling** and design around it.

---

## Module 1 — Immediate vs retained mode

Until now we drew **immediate mode**: every `gfx->fillCircle(...)` went *straight
to the panel* over SPI. That's fast for small changes, but it has no memory of the
scene — which is why "clear" meant "repaint the fixed UI," and why a gradual
**fade** was impractical (you'd have to read back, dim, and rewrite every pixel in
the right order, every frame).

**Retained mode** keeps a full copy of the screen — a **framebuffer** — in RAM.
All drawing lands there; once per frame you **`flush()`** the whole buffer to the
panel. Now "wipe everything and repaint the entire scene at new brightnesses every
frame" is just... a loop. That's exactly what a fade needs. It's also how **LVGL**
works internally, so this is the bridge to Stage 4.

The trade-off — and the second half of this lesson — is **bandwidth**: you now
push the *whole* screen over SPI every frame, whether one pixel changed or all of
them.

---

## Module 2 — Turning on PSRAM (finally)

A full framebuffer here is 222 × 480 × 2 bytes = **~213 KB**. That won't fit in the
ESP32-S3's internal RAM — it must live in **PSRAM**, the 8 MB we confirmed at boot
in Lesson 01 but *never enabled in the build*.

This board's ESP32-S3**R8** has **octal (OPI)** PSRAM, which needs the right
flash+PSRAM mode. In `platformio.ini`:

```ini
board_build.arduino.memory_type = qio_opi   ; QIO flash + OPI (octal) PSRAM
build_flags =
    ...
    -DBOARD_HAS_PSRAM                        ; tell the Arduino core to init PSRAM
```

**Verify, don't assume.** We proved it at runtime before building anything on it:

```cpp
bool psram = psramFound();
size_t bytes = ESP.getPsramSize();   // expect 8388608 (8 MB)
```

We drew that on-screen (`PSRAM: FOUND / 8192 KB`) — serial had been flaky, and the
screen is a reliable instrument. Green + 8192 KB = the octal config is correct.

> If PSRAM reads as NONE/0, the `memory_type` doesn't match the board's flash/PSRAM
> combo — try the other flash mode (`opi_opi`, etc.) before going further.

---

## Module 3 — The Canvas

The `Arduino_ST7796` becomes just the **physical panel**; an `Arduino_Canvas` (the
framebuffer) sits in front of it:

```cpp
Arduino_GFX     *panel = new Arduino_ST7796(bus, TFT_RST, 0, true, 222, 480, 49, 0);
Arduino_Canvas  *gfx   = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);
...
gfx->begin();     // begins the panel AND allocates the 213 KB buffer in PSRAM
```

Every `gfx->` call now draws into the PSRAM buffer. `gfx->flush()` blits the whole
buffer to `panel`. `Arduino_Canvas::begin()` allocates with `aligned_alloc(16,
213120)` — which only succeeds because PSRAM is on (Module 2). If it fails, we fall
back to the raw panel to show an error.

---

## Module 4 — The fading trail

The trail is a **ring buffer** of recent points, each stamped with the time it was
placed. Every frame we clear the canvas and repaint each *live* point, dimmer by
its age; points older than `TRAIL_TTL_MS` are simply skipped (gone).

```cpp
struct TrailPoint { int16_t x, y; uint32_t t; };   // t = millis() when placed
TrailPoint trail[TRAIL_MAX];

uint16_t buildScene() {
  gfx->fillScreen(RGB565_BLACK);
  uint32_t now = millis();
  uint16_t live = 0;
  for (int i = 0; i < TRAIL_MAX; i++) {
    if (trail[i].t == 0) continue;
    uint32_t age = now - trail[i].t;
    if (age >= TRAIL_TTL_MS) continue;             // expired
    float k = 1.0f - (float)age / TRAIL_TTL_MS;    // 1 at birth → 0 at death
    gfx->fillCircle(trail[i].x, trail[i].y, DOT_RADIUS, fadedYellow(k));
    live++;
  }
  return live;
}
```

`fadedYellow(k)` scales yellow toward black in RGB565 (dim the 5-bit red and 6-bit
green channels). Returning `live` lets the loop **stop flushing once the trail has
fully faded** — no point blitting an unchanging black screen.

---

## Module 5 — The performance investigation (the real lesson)

The first version looked *terrible*: scattered dots, choppy fade — worse than
immediate mode. Rather than guess, we measured (an on-screen `flush X ms`
readout). Findings, in order:

### Finding 1 — the flush is ~40 ms, and the clock won't fix it

Blitting 213 KB over SPI takes **~40 ms**, capping us at **~22 fps**. The obvious
fix — raise the SPI clock from 40 → 80 MHz — **did nothing**: the flush stayed
40 ms. Reading the library explained why:

- `Arduino_ESP32SPI` pushes pixels by **CPU-polling the SPI FIFO** (no DMA), and
- its clock path effectively **caps at ~40 MHz** on this target.

So `gfx->begin(80000000)` silently ran at 40 MHz. **Lesson: verify that a "fix"
actually changed the thing you think it did.** The transfer is a fixed full-screen
16-bit payload, so this is a real bandwidth wall, not a tuning knob.

### Finding 2 — a blocking flush starves touch sampling

Because `flush()` **blocks** for 40 ms, `loop()` only checked touch ~22×/sec — and
gating that check behind the flickering IRQ (`isPressed()`, Lesson 02) meant we
*missed* pulses, landing samples 100–150 ms apart. That exceeded our stroke-gap
window, so **interpolation disengaged** → isolated dots ("scattered"). Fix: poll
`getPoint()` **directly every frame** (no IRQ gate) and treat `count == 0` as
finger-up. One solid sample per frame → interpolation stays engaged → continuous
ribbon. (Interpolation itself is the Stage 2c trick, reused.)

### Finding 3 — integer radius steps look "steppy"

Fading the dot *radius* as `1 + 5*k` gives only **six** integer sizes, so aging
dots visibly *pop* between them. Keeping the radius **constant** and fading only
the **colour** (many more levels) made the fade-out smooth — no code faster, just
fewer visible quantisation steps.

### What's left: fast-motion stutter (the honest ceiling)

After those fixes the fade-out is smooth and the ribbon continuous. Fast *swipes*
still stutter — because that's the ~22 fps full-frame flush, full stop. We
deliberately did **not** chase it with a dirty-rectangle (partial) flush: a fading
trail redraws *every live point* each frame, so during a fast swipe the changed
region spans most of the screen and a "partial" flush ≈ a full one. The real fixes
are architectural (DMA to overlap transfer with compute; a parallel/QSPI display) —
beyond this stage.

> **This ceiling is the lesson.** Retained mode buys flicker-free whole-scene
> effects; it pays in bandwidth. It's exactly why serious UIs (LVGL included)
> track **dirty rectangles** and only redraw what changed, and why fast displays
> use DMA and wider buses. You now understand *why* those exist.

---

## What you built and learned

- ✅ Enabled and verified **octal PSRAM**; understand `memory_type` + `BOARD_HAS_PSRAM`
- ✅ Wrapped the panel in an **`Arduino_Canvas`** framebuffer and `flush()`ed it
- ✅ Built a **time-based fading trail** (ring buffer + per-point timestamps)
- ✅ **Measured** the flush bottleneck and learned the SPI path has no DMA / a 40 MHz cap
- ✅ Fixed under-sampling (poll directly) and size-popping (constant radius)
- ✅ Understand the **retained-mode bandwidth ceiling** — and why dirty-rectangles / DMA exist

## Command cheat-sheet

```bash
pio run -t upload       # build + flash (a memory_type change triggers a full rebuild)
```

## Glossary

- **Immediate mode** — draw straight to the panel; no stored scene.
- **Retained mode / framebuffer** — a full in-RAM copy of the screen you draw into, then blit.
- **`flush()`** — copy the whole framebuffer to the panel over the bus.
- **PSRAM** — external RAM (8 MB here, octal/OPI); needed for a full-screen framebuffer.
- **DMA** — hardware that moves data without the CPU; absent in this SPI path, so the flush is CPU-bound.
- **Dirty rectangle** — redrawing only the region that changed, instead of the whole screen.
- **Quantisation stepping** — visible banding from too-few discrete levels (e.g. integer radii).

## Next lesson

**Stage 4 — LVGL.** A real widget toolkit: buttons, sliders, labels, screens,
animations. It layers on the display + touch we've built, and — as this lesson
foreshadowed — uses **dirty-rectangle rendering** so it doesn't pay the full-frame
flush cost we just hit. Open decisions: LVGL **v8** (matches LilyGO's examples) vs
**v9**, and whether to hand-code the UI or use a designer (SquareLine / EEZ).
