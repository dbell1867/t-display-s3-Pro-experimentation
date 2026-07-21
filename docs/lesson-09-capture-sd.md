# Lesson 09 — capture a still: three buses, one JPEG on the card

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Add a **capture** to the live viewfinder — grab the current
frame, encode it to JPEG, and write it to the SD card as `/IMG_NNNN.JPG`. This is
the first time the camera, the encoder and the SD card all run in the same moment,
so it's really a lesson about **three subsystems on three different buses not
fighting** — plus one physical button doing two jobs.

---

## Learning objectives

By the end of this lesson you can:

1. Explain why capture-during-streaming **doesn't** cause a bus conflict here.
2. Encode a frame to JPEG with `frame2jpg()` and **free what it allocates**.
3. Pick the **next free filename** without a database — just the card.
4. Get **two actions from one button** by press duration.
5. Show feedback on a screen you've taken over from LVGL.

---

## Module 1 — Three subsystems, three buses

Capturing while streaming sounds like it should collide with something. It doesn't,
and the reason is worth seeing laid out — each step uses a *different* resource:

| Step | Resource it uses |
|------|------------------|
| Camera fills the frame | **DVP parallel** bus (D0–D7, PCLK, VSYNC, HREF) |
| `frame2jpg` encodes RGB565 → JPEG | pure **CPU** (+ PSRAM for the output) |
| Write the JPEG to `/IMG_NNNN.JPG` | **SPI** (shared with the display) |

The only shared bus in play is **SPI**, and Stage 6 already proved the SD and display
drivers bracket their transfers (`is_shared_interface=true`), so a write slipped
between frame-pushes is safe. The camera's data never touches SPI (it's parallel),
and SCCB (I²C) isn't used during a capture. So the three can interleave freely.

> **Before assuming two operations conflict, ask which bus each actually uses.**
> "The camera and the SD card are both busy" *sounds* like contention; map them to
> DVP / CPU / SPI and the conflict evaporates.

---

## Module 2 — `frame2jpg`, and who owns the memory

```cpp
#include "img_converters.h"    // bundled with esp_camera — no new lib_deps

camera_fb_t *fb = esp_camera_fb_get();
uint8_t *jpg = NULL;
size_t   jpgLen = 0;
bool ok = frame2jpg(fb, 80, &jpg, &jpgLen);   // quality 80; it malloc's `jpg`
esp_camera_fb_return(fb);                       // done with the RAW frame right away
...
free(jpg);                                       // OUR job — frame2jpg allocated it
```

Two different buffers, two different owners:

- `fb` belongs to the **camera driver** — you borrow it and hand it back with
  `esp_camera_fb_return()`.
- `jpg` is **malloc'd by `frame2jpg`** — you own it and must `free()` it.

> **For Python people:** this is the same manual-cleanup rule as `File::close()`
> (Stage 6) and `fb_return` (Stage 8), a third time. Anything that hands you a buffer
> or a handle in C is asking you to give it back. Miss the `free()` and each capture
> leaks ~a frame's worth of heap until the board runs out.

A QCIF frame at quality 80 encodes to roughly **5–8 KB** — versus ~50 KB for the raw
RGB565 — which is the whole reason to encode rather than dump raw.

---

## Module 3 — The next filename is on the card

No database, no config file. The card *is* the record of what exists:

```cpp
static uint16_t imgSeq = 1;                 // survives across captures this session
char path[24];
while (true) {
  snprintf(path, sizeof(path), "/IMG_%04u.JPG", imgSeq);
  if (!SD.exists(path)) break;              // first gap = next filename
  imgSeq++;
}
```

On the first capture after a reboot, `imgSeq` starts at 1 and the loop **scans past
any files already on the card** until it finds a gap; after that it just increments
in RAM. So numbering never collides across power cycles or reflashes — the same
property that made `/boots.csv` survive in Stage 6, now protecting image filenames.

`FILE_WRITE` then *creates* the file (we've guaranteed the path is free), and
`f.write(jpg, jpgLen)` returns the byte count to check against `jpgLen`.

---

## Module 4 — One button, two jobs

Touch is frozen while the camera streams (it owns the I²C bus, Stage 8), so the
**GPIO 16 button** is the only input — and it has to both *capture* and *exit*. Split
by **duration**:

```cpp
const uint32_t LONGPRESS_MS = 700;
if (prevHigh && !high) {                       // press begins: start the clock
  pressStart = millis();
} else if (!prevHigh && high) {                // release: was it a tap?
  uint32_t held = millis() - pressStart;
  if (held >= 40 && held < LONGPRESS_MS) captureStill(msgY);   // 40 ms debounces
} else if (!high && (millis() - pressStart) >= LONGPRESS_MS) {
  exiting = true;                              // still held past threshold: leave
}
```

Two deliberate choices:

- **Capture fires on RELEASE** (you only know it was a *tap* once it ends), but
  **exit fires the moment the hold crosses 700 ms** — so you don't have to release to
  leave, the hold itself is the gesture.
- **40 ms floor** rejects contact bounce (Stage 5f measured bounce at 1–5 ms, but the
  floor is cheap insurance).

> **Press-duration is a clean way to overload a scarce input.** When you're down to
> one button, tap-vs-hold gives you two actions with no extra hardware — common on
> real devices (think a single-button earbud).

---

## Module 5 — Feedback on a screen you've taken over

We're drawing camera frames straight to the panel (no LVGL while streaming), so the
status line is direct GFX text — placed **below** the image so live frames don't
paint over it:

```cpp
static void drawViewfinderMsg(int y, const char *msg, uint16_t colour) {
  panel->fillRect(0, y, SCREEN_W, 18, RGB565_BLACK);
  panel->setTextSize(1);
  panel->setTextColor(colour);
  panel->setCursor(6, y + 5);
  panel->print(msg);
}
```

Frames only cover the image rectangle (`oy … oy+fh`), so anything drawn at `oy+fh+6`
**persists** until the next capture rewrites it — no timer, no redraw loop. Green
filename = saved; red = failed; white hint on entry ("tap = save   hold = exit").

---

## Module 6 — Board & build notes

- **`frame2jpg` / `img_converters.h`** ship with the esp32-camera component — no
  `lib_deps`, same as `esp_camera.h`.
- **Capture briefly freezes the live image** (encode + SD write ≈ 100–300 ms). Not a
  bug — the stream resumes right after; with `CAMERA_GRAB_LATEST` the buffers that
  filled meanwhile are just overwritten.
- **Stills are QCIF** — "save what you see." A higher-res still would need a *sensor
  reconfigure* mid-session (bigger frame size, re-init), a later refinement.
- The clang editor may flag `img_converters.h` as not found — the same false alarm
  as `Arduino.h`; the pio build resolves it.

---

## What you built and learned

- ✅ **Captured a still to SD as a real JPEG** — `/IMG_NNNN.JPG`, viewable anywhere
- ✅ Saw why **three subsystems on three buses** (DVP / CPU / SPI) don't conflict
- ✅ Used `frame2jpg` and **freed what it allocated** (the C ownership rule, again)
- ✅ Chose the **next filename from the card itself** — no external state
- ✅ Got **two actions from one button** by press duration
- ✅ Drew **persistent feedback** on a panel taken over from LVGL

## Command cheat-sheet

```bash
pio run -t upload -t monitor     # "capture /IMG_0001.JPG: OK (6231 bytes)" per tap
# then read the card:
ls -l /run/media/$USER/*/IMG_*.JPG
```

## Glossary

- **`frame2jpg(fb, q, &out, &len)`** — encode a frame to JPEG; mallocs `out` (you free it).
- **DVP** — the camera's parallel data bus; independent of SPI and I²C.
- **`SD.exists(path)`** — bool; the basis for "next free filename".
- **Long-press** — hold past a threshold as a distinct gesture from a tap.

## Next lesson

The still is a proof of concept at viewfinder resolution. Natural follow-ons:
**higher-resolution stills** (reconfigure the sensor for the capture, then back to
QCIF for preview), **scale/rotate the live viewfinder** to fill the portrait screen,
or step to the last untouched subsystem — **WiFi/BLE** (e.g. serve the captured
JPEGs over HTTP).
