# Lesson 08 — the live viewfinder: pixels from the camera onto the glass

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Turn Stage 7's *detected* camera into a **live viewfinder** —
capture RGB565 frames into PSRAM and push them straight to the ST7796. The camera
and the display, finally in one pipeline. It worked in three steps: get pixels on
the glass, fix the colours, fix the orientation — each confirmed by eye.

---

## Learning objectives

By the end of this lesson you can:

1. Run a **capture → display loop**: `fb_get` → push → `fb_return`.
2. Choose a frame size/format that needs **no scaling, cropping or rotation**.
3. Fix the two things that are always wrong first: **byte order** and **orientation**.
4. Explain why a **greedy peripheral** dictates how you enter and leave a mode.
5. Reuse a config cleanly with a **shared pin-fill helper** and a **bus-recovery** helper.

---

## Module 1 — The pipeline

The whole viewfinder is four calls in a loop:

```cpp
camera_fb_t *fb = esp_camera_fb_get();          // pull the latest frame (PSRAM)
if (fb) {
  panel->draw16bitBeRGBBitmap(ox, oy,           // push it to the ST7796
                              (uint16_t *)fb->buf, fb->width, fb->height);
  esp_camera_fb_return(fb);                      // recycle the buffer
}
```

`esp_camera_fb_get()` hands back a pointer to a filled framebuffer living in PSRAM.
We blit it to the panel, then **`esp_camera_fb_return(fb)`** — miss that and the
driver's buffer queue starves within `fb_count` frames and the stream stops.

Streaming config differs from Stage 7's detection config in three fields:

```cpp
cfg.fb_count   = 2;                    // double-buffer: capture overlaps draw
cfg.grab_mode  = CAMERA_GRAB_LATEST;   // always the freshest frame, drop stale
cfg.frame_size = FRAMESIZE_QCIF;       // 176x144 — see Module 2
```

> **`fb_return` is `free()` for frames.** The Python instinct is that the runtime
> reclaims it; here you hand each buffer back by name, exactly like `close()` on a
> `File` (Stage 6). Forget it and the pipeline stalls, silently.

---

## Module 2 — A size that needs no maths

The panel is **222 × 480 portrait**. Camera frame sizes are landscape and mostly
*wider* than 222 (QVGA is 320×240). Push a buffer wider than the display and you're
into clipping or per-row cropping on day one.

So pick the largest standard size that **fits the width**: **QCIF, 176 × 144**.
It drops onto the screen with a plain centred offset and **no scaling, no cropping,
no rotation**:

```cpp
const int ox = (SCREEN_W - 176) / 2;   // 23
const int oy = (SCREEN_H - 144) / 2;   // 168
```

That's the "first light" discipline again (Stage 2a): get *something* correct on the
glass with the least machinery, then refine. Scaling up or rotating to fill the
portrait screen is real work — CPU per frame — and it's a *later* decision, made
against the measured frame rate, not guessed at up front.

---

## Module 3 — The two things that are always wrong first

A raw camera image almost never comes up right. Two independent problems, each with
a cheap fix — and each **only diagnosable by eye**, which is why they shipped as
"look and tell me."

### Byte order (the colours)

esp_camera's RGB565 is **big-endian** relative to what the panel's default push
expects. First attempt used `draw16bitRGBBitmap` (native-endian) → wrong colours.
The fix is not a loop that swaps bytes — it's **picking the matching push function**:

```cpp
panel->draw16bitBeRGBBitmap(...);   // "Be" = big-endian source
```

Arduino_GFX ships both variants precisely because sources disagree on byte order.
(The LVGL equivalent from Stage 4 was `lv_draw_sw_rgb565_swap` — same problem, same
family of fix.)

### Orientation (upside down)

The image came up **inverted**. The wrong fix is to flip the buffer in software
(CPU every frame). The right fix is to tell the **sensor** to flip during readout —
free, because the pixels arrive already the right way up:

```cpp
sensor_t *s = esp_camera_sensor_get();
if (s) {
  s->set_vflip(s, 1);      // "inverted" = upside down = a vertical flip
  // s->set_hmirror(s, 1); // + this if it's also mirrored (a full 180° mount)
}
```

> **Orientation belongs in the sensor config, not the draw loop.** `set_vflip` /
> `set_hmirror` cost nothing per frame. A software rotation is the fallback *only*
> for a true 90° mount, which the sensor's flips can't express.

Result after both fixes: **correct colours, right-side up.** Confirmed by eye.

---

## Module 4 — A greedy peripheral dictates the controls

The camera's SCCB sits on the **shared I²C pins 5/6** (Stage 7). While it streams it
owns that bus, so **touch and the gauge are frozen** for the whole session. That's
not a bug to fix — it's a constraint that decides the UX:

- **Enter** with the touch **"Cam" button** (touch still works — we haven't started
  streaming yet).
- **Exit** with the physical **GPIO 16 button** — the one input that survives
  everything the firmware takes over. This is the exact primitive Stage 5f built for
  deep-sleep wake, earning its keep a second time.

```cpp
bool prevHigh = (digitalRead(BTN_WAKE) == HIGH);   // seed: a held button won't exit
while (true) {
  /* … capture + draw … */
  bool high = (digitalRead(BTN_WAKE) == HIGH);
  if (prevHigh && !high) break;                    // falling edge = leave
  prevHigh = high;
}
```

The viewfinder **blocks** the main loop. That's fine: we've taken over the screen,
and `esp_camera_fb_get()` blocks on a semaphore that **yields to the RTOS**, so
nothing starves and no watchdog trips.

> **When a peripheral is exclusive, don't fight it — design around it.** Trying to
> interleave touch with a bus the camera owns would be fragile; giving the camera
> the whole session and exiting on the one always-available input is simple and
> correct.

---

## Module 5 — Borrow the bus, give it back (again)

Entering and leaving reuse two helpers factored out of Stage 7's probe, so the
config and the recovery can't drift between the two callers:

```cpp
cameraFillPins(cfg);     // the pin map + XCLK + LEDC-away-from-backlight, once
// … set format/size/buffers per use …
esp_camera_init(&cfg);
// … stream …
esp_camera_deinit();
cameraRecoverBus();      // Wire.end(); Wire.begin(); + re-init touch/PMU/ALS
```

`cameraRecoverBus()` is the Stage 7 lesson made reusable: esp_camera leaves I²C
pins 5/6 muxed to its port, and `Wire.begin()` is idempotent, so you must
`Wire.end()` first, then re-establish the sensor drivers. On viewfinder exit we
also repaint the LVGL screen we clobbered:

```cpp
panel->fillScreen(RGB565_BLACK);
lv_obj_invalidate(lv_screen_active());
lv_refr_now(lv_display_get_default());
```

Same rule as the wake path from Stage 5e: **never illuminate a buffer you haven't
drawn** — wipe to black, repaint, then let it show.

---

## Module 6 — Board & build notes

- **Frame rate** prints on exit (`viewfinder: N frames / M ms = ~X fps`). QCIF over
  the **non-DMA SPI** bus (the Stage 3b ceiling) is the limiter; a bigger frame or a
  software rotation would cost against this number, so measure before deciding.
- **PSRAM**: QCIF × 2 buffers ≈ 101 KB — comfortable in 8 MB. Larger frames are
  where PSRAM actually starts to matter.
- **`draw16bitBeRGBBitmap`** vs `draw16bitRGBBitmap` — the only difference is source
  byte order; wrong one = wrong colours, not a crash.
- **A third button** ("Cam" / "Bench" / "Sleep") now shares the bottom row at
  68 px each. Still hand-aligned by absolute offset — this is the pressure that
  finally argues for LVGL flex/grid (`lv_obj_align` has no collision detection,
  Stage 5e).

---

## What you built and learned

- ✅ A **live viewfinder**: capture into PSRAM → push to the ST7796, in a loop
- ✅ Picked a frame size (**QCIF**) that needs no scaling/cropping/rotation
- ✅ Fixed **byte order** by choosing the matching push (`draw16bitBeRGBBitmap`)
- ✅ Fixed **orientation** in the sensor (`set_vflip`) — free, not in the draw loop
- ✅ Handled a **greedy shared-bus peripheral**: touch enter, physical-button exit
- ✅ Reused **`cameraFillPins` / `cameraRecoverBus`** so probe and viewfinder agree

## Command cheat-sheet

```bash
pio run -t upload -t monitor     # watch the fps line print on viewfinder exit
```

## Glossary

- **`camera_fb_t`** — a captured frame: `.buf`, `.width`, `.height`, `.len`.
- **`fb_return`** — hand a frame back to the driver's queue (the `free()` of frames).
- **`CAMERA_GRAB_LATEST`** — always serve the freshest frame; drop stale ones.
- **`set_vflip` / `set_hmirror`** — sensor-side flips, done during readout (free).
- **`draw16bitBeRGBBitmap`** — push a big-endian RGB565 buffer (esp_camera's order).

## Next lesson

The viewfinder is first-light small (176×144, centred). Natural follow-ons: **scale
or rotate to fill** the portrait screen (measure the fps cost first), a **capture-
to-SD** still (ties in Stage 6 — save a JPEG frame to the card), or step back to the
deferred threads (log the gauge to CSV, the LVGL flex/grid refactor).
