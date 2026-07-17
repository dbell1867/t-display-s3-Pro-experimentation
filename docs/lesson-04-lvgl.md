# Lesson 04 — LVGL: a real widget toolkit

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Move from drawing *pixels* by hand to using a real
**widget toolkit** — LVGL v9. Create buttons, labels and screens; attach event
callbacks; and let LVGL handle layout, touch hit-testing, debouncing, and
**dirty-rectangle rendering** (which sidesteps the full-frame flush ceiling we
hit in Lesson 03b).

> Continues from [Lesson 03b](lesson-03b-fading-trail.md). That lesson ended on
> a bandwidth wall — repainting the whole screen every frame. LVGL only repaints
> what *changed*, which is exactly the way out.

---

## Learning objectives

By the end of this lesson you can:

1. Explain what LVGL is and how it layers on top of a display + touch driver.
2. Add LVGL v9 to a PlatformIO project and configure it with `lv_conf.h`.
3. Write the **two glue callbacks** that connect LVGL to any hardware: a
   **flush** callback (pixels out) and a **read** callback (touch in).
4. Create widgets (buttons, labels), style them, and attach **event callbacks**.
5. Recognise and fix a **buffer-alignment** bug — a classic embedded Heisenbug.

---

## Module 1 — What LVGL is (and isn't)

Until now we drew *pixels*: `fillCircle`, `drawLine`, text at coordinates. That's
**immediate-mode graphics** — powerful, but you build everything (buttons, hit
detection, press feedback, debouncing) by hand. We felt that in Lesson 03, where
getting *one clean increment per tap* took ~40 lines of edge detection.

**LVGL** is a **retained-mode widget toolkit**. You create *objects* — a button,
a label, a slider — set their properties, and attach *callbacks* ("when this
button is clicked, run this function"). LVGL keeps the object tree, lays it out,
styles it, tests which object is under your finger, and redraws only what
changed. In Python terms it's the jump from painting on a canvas to using a GUI
framework like Tkinter or Qt.

Crucially, **LVGL is hardware-agnostic**. It doesn't know what a ST7796 or a
CST226SE is. It talks to the world through exactly two callbacks that *we* write.
Everything else — the widgets, the events — is portable LVGL code.

### Two decisions we made

- **v9, not v8.** v8 matches LilyGO's pinned examples (less translation), but v9
  is the current API and what new projects should learn. We took v9 and
  translated the display/touch glue ourselves.
- **Hand-coded, not a designer.** Tools like SquareLine/EEZ Studio drag-and-drop
  a UI and export C — fast, but they hide the fundamentals. We hand-coded so
  every widget is understood.

---

## Module 2 — Wiring LVGL into the build (the fiddly part)

Two things go into `platformio.ini`:

```ini
lib_deps =
    ...
    lvgl/lvgl @ ^9.3.0          ; resolved to 9.5.0

build_flags =
    ...
    -DLV_CONF_INCLUDE_SIMPLE    ; LVGL does `#include "lv_conf.h"`
    -I include                  ; put our include/ (holding lv_conf.h) on the path
```

**How LVGL finds its config** is the notorious stumble ("lv_conf.h not found").
LVGL needs a configuration header, `lv_conf.h`. With `LV_CONF_INCLUDE_SIMPLE`,
LVGL includes it by plain name (`#include "lv_conf.h"`), and `-I include` puts
our `include/` folder on the compiler's search path so that resolves to ours.

Our `include/lv_conf.h` is deliberately **short** — we only override what matters
for this board (`LV_COLOR_DEPTH 16`, a memory pool size, two fonts). Every macro
we *don't* set falls back to LVGL's built-in default (`lv_conf_internal.h`
backfills all of them), so we skip the ~1000-line template entirely.

> **Checkpoint (step 1):** with just `lv_init()` called, the project *compiles
> and links*. We confirmed the LVGL version (9.5.0) on screen before wiring any
> hardware. The build integration is the hard part; prove it in isolation.

---

## Module 3 — Glue callback #1: flush (pixels out)

LVGL renders into a small **draw buffer** in RAM (here a ~48-line slice, not the
whole screen), then calls *our* flush callback: "here's a rectangle and its
pixels — put them on the panel."

```cpp
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  panel->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);   // tell LVGL: this region is on screen
}
```

Registering it:

```cpp
lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
lv_display_set_flush_cb(disp, my_flush_cb);
lv_display_set_buffers(disp, drawBuf, NULL, sizeof(drawBuf),
                       LV_DISPLAY_RENDER_MODE_PARTIAL);
```

`RENDER_MODE_PARTIAL` is the point: LVGL hands us only the rectangles that
**changed**. Tapping a button repaints ~a button's worth of pixels, not the
213 KB whole-screen blit that capped Lesson 03b at ~22 fps.

**Colour/byte order** is the classic risk here (RGB565 can come out swapped). We
drew pure red/green/blue bars to verify at a glance — they read correctly, so no
swap was needed. (If they'd been wrong, the fix is one line:
`lv_draw_sw_rgb565_swap(px_map, w * h);` before the blit.)

> **Checkpoint (step 2):** "Hello, LVGL 9.5!" + three correct colour bars. LVGL
> now owns the screen.

---

## Module 4 — Glue callback #2: read (touch in)

The mirror image: LVGL periodically asks "is the screen pressed, and where?" We
answer from the CST226SE driver.

```cpp
static void my_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  int16_t x[5], y[5];
  uint8_t count = touch.getPoint(x, y, 1);
  if (count > 0) {
    lastTouchX = x[0]; lastTouchY = y[0];
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lastTouchX;   // HELD on release (see below)
  data->point.y = lastTouchY;
}
```

Registered as a *pointer* input device:

```cpp
lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, my_touch_read_cb);
```

Two things worth understanding:

- **Hold the last point on release.** On finger-up we report `RELEASED` but keep
  the last `(x, y)`. LVGL uses the *release position* to decide which widget was
  clicked; if we zeroed it, clicks would register at (0,0).
- **All the hard stuff is gone.** No debounce, no IRQ-pulse handling, no
  touch-up edge detection — the things we hand-rolled in Lesson 03. LVGL's input
  pipeline turns our raw press/release stream into press/click/long-press/drag
  events internally.

> **Checkpoint (step 3):** a button that highlights only when pressed *on it*
> (hit-testing works) and a live coordinate readout (mapping still identity, as
> calibrated in Lesson 02).

---

## Module 5 — The alignment Heisenbug (the debugging lesson)

Step 3 refused to boot: the screen froze on step 2's image. A frozen *old* image
is a strong tell — the new firmware **died in `setup()` before LVGL ever
rendered**, and the panel holds its last picture until something overwrites it.

Serial was no help (native-USB CDC drops its connection on reset), so we used the
**display as the instrument**: print a numbered marker after each setup step and
see which is the last one visible.

```
1 panel ok
2 touch ok
3 lv_init ok
4a create
4b flush_cb      <-- last marker seen; hangs here, stable (not a reboot loop)
```

It hung *inside* `lv_display_set_buffers` — the **exact same call** that worked
in step 2. When identical code behaves differently between builds, suspect
**memory layout**:

- LVGL v9 requires its draw buffer to be **aligned** (to a 4/16/32/64-byte
  boundary, for cache/DMA reasons).
- Our buffer was a plain `static uint8_t[]`, which C only guarantees to 1-byte
  alignment — its real address is whatever the linker gives it.
- In step 2 it *happened* to land aligned. In step 3, adding the touch driver's
  static data shifted every later symbol, moving `drawBuf` onto a **misaligned**
  address. LVGL's alignment check failed, and its default assert handler is an
  infinite `while(1)` — the stable hang.

The fix is to *ask* for the alignment instead of relying on luck:

```cpp
static uint8_t drawBuf[SCREEN_W * LVBUF_LINES * (LV_COLOR_DEPTH / 8)]
    __attribute__((aligned(64)));
```

**Lesson:** a bug that appears/disappears when you touch *unrelated* code is
almost always uninitialised memory, a race, or **alignment**. Buffers handed to
libraries (LVGL, DMA engines, SIMD) must be explicitly aligned.

---

## Module 6 — A real widget with an event

With both callbacks in place, the app itself is pure LVGL. We rebuilt Lesson 03's
Tap Counter — the same behaviour, a fraction of the code:

```cpp
static void inc_event_cb(lv_event_t *e) {
  counter++;
  lv_label_set_text_fmt(countLabel, "%d", counter);
}
...
lv_obj_t *incBtn = lv_button_create(scr);
lv_obj_set_size(incBtn, 170, 90);
lv_obj_align(incBtn, LV_ALIGN_CENTER, 0, 0);
lv_obj_add_event_cb(incBtn, inc_event_cb, LV_EVENT_CLICKED, NULL);
lv_obj_t *incLbl = lv_label_create(incBtn);   // a label as a child of the button
lv_label_set_text(incLbl, "Tap +1");
lv_obj_center(incLbl);
```

`LV_EVENT_CLICKED` fires **exactly once per completed tap** — LVGL debounces and
does the press/release edge detection for us. On hardware: fast taps all register
one-for-one, holds don't run away. That is precisely the behaviour we fought for
by hand in Lesson 03, now free.

The main loop is just LVGL's heartbeat:

```cpp
void loop() {
  lv_timer_handler();   // render dirty areas + service input timers
  delay(5);
}
```

Note there's **no per-frame label update** — the count label only redraws inside
the event callback, when it actually changes. Event-driven, not polled.

---

## What you built and learned

- ✅ Added **LVGL v9.5** to PlatformIO and configured it via a minimal `lv_conf.h`
      (`LV_CONF_INCLUDE_SIMPLE` + `-I include`)
- ✅ Wrote the **flush** callback (dirty-rectangle pixels → Arduino_GFX) and
      verified colour/byte order with RGB test bars
- ✅ Wrote the **read** callback (CST226SE → LVGL), letting LVGL handle
      hit-testing, debouncing, and click detection
- ✅ Diagnosed a **draw-buffer alignment** hang with the display-as-instrument
      marker technique, and fixed it with `__attribute__((aligned(64)))`
- ✅ Built a widget app with **event callbacks** — one clean increment per tap,
      no hand-rolled edge detection

## Command cheat-sheet

```bash
pio run -t upload       # build + flash (first LVGL build is slow — it compiles the lib)
```

## Glossary

- **Widget / object** — an LVGL UI element (button, label, slider) in a tree.
- **Retained mode** — the toolkit keeps the scene and redraws it for you (vs
  immediate mode, where you redraw everything yourself).
- **Flush callback** — you push LVGL's rendered pixels to the panel.
- **Read (indev) callback** — you report input (touch state + point) to LVGL.
- **Draw buffer** — the RAM LVGL renders into before flushing; must be aligned.
- **Dirty rectangle** — the changed region LVGL redraws instead of the whole screen.
- **Event callback** — a function LVGL calls on an interaction (`LV_EVENT_CLICKED`, …).
- **Alignment** — placing data at an address that's a multiple of N bytes;
  required by many libraries/hardware.

## Next lesson

**Stage 5 — onboard peripherals** (pick by interest): the LTR-553 light/proximity
sensor, the SY6970 battery/power gauge, the physical buttons / SD card — and the
deferred **low-power lesson** (a true hardware interrupt on the touch IRQ so the
CPU can sleep and wake on touch, which pairs naturally with battery work).
