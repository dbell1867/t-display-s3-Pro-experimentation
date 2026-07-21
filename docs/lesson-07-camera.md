# Lesson 07 — the camera, and four probes (three of which lied)

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Detect the camera module on the DVP shield and identify
the sensor — without breaking the peripherals that already work. We got a clean
`CAMERA OK: sensor PID 0x5640` (an OV5640). Getting there took **four probes**,
and the three we built ourselves each measured the wrong thing. This lesson is
really about that: **an instrument is only worth what it can actually measure.**

---

## Learning objectives

By the end of this lesson you can:

1. Detect a DVP camera and read its **sensor PID** with the real driver.
2. Explain why a **hand-rolled I²C scan cannot see a camera** (two reasons).
3. Recognise when to **stop refining a home-made instrument** and reach for the
   purpose-built one.
4. **Borrow a shared bus and give it back** — the I²C mirror of Stage 6's SPI.
5. Read a native-USB **boot log** reliably, and tell a real error from benign noise.

---

## Module 0 — What the camera actually is

Not onboard. It's a **separate module on a ribbon cable** that mates through the
shield connector, and on this unit the sensor was under shielding — no readable
markings. So we had a pin map from LilyGO's `examples/CameraShield/utilities.h`
and **no proof it matched the module in hand.** That uncertainty drove everything.

```
XCLK 11   PCLK 2   VSYNC 7   HREF 15   PWDN 46   RESET -1
D7..D0 = 4,10,3,1,42,40,41,45          SIOD 5   SIOC 6   (== the I²C bus!)
```

Two facts to keep in view:

- **SCCB (SIOD/SIOC) is the I²C bus** we already share with touch (0x5A), the PMU
  (0x6A) and the ALS (0x23). The camera is a fourth tenant.
- **GPIO 45/46 are strapping pins; 40-42 are JTAG.** A fitted camera sits on pins
  the chip cares about during boot — reason enough to leave them clean afterwards.

---

## Module 1 — The probe that couldn't clock (bug #1)

First instinct: scan the I²C bus for a new address. But most DVP sensors clock
their register logic from **XCLK**, so with no master clock they never answer.
So the probe generated a 20 MHz clock on GPIO 11 with the PWM peripheral:

```cpp
ledcAttach(CAM_XCLK, 20000000, 2);   // 20 MHz, 2-bit resolution
// comment I wrote: "20 MHz * 2^2 = 80 MHz exactly — fine"
```

It was not fine. The boot log — once we could finally read it (Module 5) — said:

```
E ledc: requested frequency 20000000 and duty resolution 2 can not be achieved, div_param=0
ledcAttach(): No free timers available for freq=20000000, resolution=2
```

The LEDC divider is `80 MHz / (freq * 2^res)`. At 20 MHz × 4 that's `80/80 = 1`,
and the fractional divider register can't represent it — `div_param` comes out
**0** and the attach **fails**. I never checked the return value, so the "clock"
never ran. **Every "no camera" the probe printed was the instrument failing.**

> **Two lessons.** (1) `ledcAttach` returns a bool — *check it*. (2) **LEDC is a PWM
> generator, not a clock source.** 20 MHz is past where it's usable; that's exactly
> why cameras have a dedicated clock peripheral.

---

## Module 2 — …and couldn't ACK either (bug #2)

Suppose the clock *had* run. The scan would still have failed, because **SCCB is
not quite I²C.** It's OmniVision's variant, and several sensors don't drive the
acknowledge bit the way `Wire.endTransmission()` expects. A generic scan reads
that as "no device."

So the hand-rolled probe had **two independent blind spots**: no clock, and no
ACK. We spent rounds refining it anyway — sweeping the PWDN polarity, lengthening
the settle delay — tuning a measurement that was never happening. Reasoning on
top of a broken instrument produces confident nonsense.

---

## Module 3 — The instrument that works

`esp_camera_init()` does the whole sequence correctly: powers the sensor, clocks
XCLK **through the proper peripheral**, talks real SCCB, and detects the sensor by
**PID** — returning a *specific* `esp_err_t`, not a boolean.

```cpp
#include "esp_camera.h"          // bundled with the ESP32-S3 core; no lib_deps

camera_config_t cfg = {};
cfg.pin_pwdn = 46; cfg.pin_xclk = 11; cfg.pin_reset = -1;
cfg.pin_sccb_sda = 5; cfg.pin_sccb_scl = 6;   // our existing I²C pins
cfg.pin_d7 = 4; /* … d6..d0 … */ cfg.pin_d0 = 45;
cfg.pin_vsync = 7; cfg.pin_href = 15; cfg.pin_pclk = 2;
cfg.xclk_freq_hz = 20000000;
cfg.ledc_timer = LEDC_TIMER_1;  cfg.ledc_channel = LEDC_CHANNEL_2;  // NOT the backlight's
cfg.pixel_format = PIXFORMAT_RGB565; cfg.frame_size = FRAMESIZE_QVGA;
cfg.fb_count = 1; cfg.fb_location = CAMERA_FB_IN_PSRAM;

esp_err_t err = esp_camera_init(&cfg);
if (err == ESP_OK) {
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("CAMERA OK: sensor PID 0x%04X\n", s->id.PID);   // 0x5640 = OV5640
  esp_camera_deinit();
} else {
  Serial.printf("camera init failed: 0x%X (%s)\n", err, esp_err_to_name(err));
  // 0x105 = ESP_ERR_NOT_FOUND: sequence ran, no sensor answered.
}
```

First try: `CAMERA OK: sensor PID 0x5640`. **OV5640, 5 MP** — and notably *not*
the OV2640 the vendor example implies. Same pin map, different sensor.

Two details worth their comments:

- **`ledc_timer`/`ledc_channel` must dodge the backlight.** `ledcAttach(TFT_BL,…)`
  auto-allocated a channel (almost certainly 0); pointing the camera at TIMER_1 /
  CHANNEL_2 stops `esp_camera_init` reprogramming the timer driving the screen.
- **`FRAMESIZE_QVGA`, `fb_count=1`.** We only want *detection*. The framebuffer
  goes in **PSRAM** — the first real use of this board's 8 MB.

> **When a purpose-built instrument exists, prefer it to one you wrote yourself.**
> The driver got right, on the first attempt, the two things our probe got wrong.
> We deleted the hand-rolled probe: a diagnostic that always lies is worse than none.

---

## Module 4 — Borrow the bus, give it back (bug #3, the mirror of Stage 6)

The camera driver installs its **own SCCB master** on pins 5/6 — the bus touch,
the PMU and the ALS live on. After the probe, the gauge **froze on "Charging"
even off USB.** That looked like a battery bug; it was a bus bug.

The recovery took two goes, and both failures are instructive:

**Attempt 1 — bare `Wire.begin()`.** Re-inits the Arduino layer but not each
driver's register config. The gauge stayed frozen: `PPM.getBattVoltage()` was
NACKing and XPowersLib handed back its **last** value.

**Attempt 2 — re-run each driver's `begin()`.** Better idea, still broke —
`PPM.init()` now *failed*. Why: `esp_camera` uses I²C **port 1**; Arduino `Wire`
is **port 0**; same pins, muxed through the GPIO matrix. `esp_camera_deinit()`
leaves pins 5/6 pointing at the now-dead port 1. And `Wire.begin()` is
**idempotent** — called again on a live instance it early-returns and never
re-does the pin mux. So the pins never came back to port 0.

**The fix — force a real teardown first:**

```cpp
esp_camera_deinit();
Wire.end();                       // <-- the missing step: release port 0
delay(10);
Wire.begin(I2C_SDA, I2C_SCL);     // now this re-muxes pins 5/6 back to port 0
initSharedBusSensors();           // then re-establish touch + PMU + ALS registers
```

> **`begin()` is not always re-entrant.** Many Arduino drivers guard against
> double-init by early-returning — fine until you *need* the re-init to do its
> work. To recover a peripheral you often must `end()` before `begin()` will act.

This is Stage 6's lesson in a mirror. There, `SPIClass::begin()` before
`panel->begin()` stole the **SPI** bus and blacked the display. Here the camera
stole the **I²C** bus and froze the gauge. **A shared bus has an owner; anything
that borrows it — even just to probe — must return it in the state the owner
expects.** "Return it" means re-establishing the *drivers*, not just the bus.

---

## Module 5 — Why you couldn't see any of this (the boot log)

All of the above was invisible at first: `pio device monitor` showed **nothing**.

Not a monitor filter — it was our own earlier "cleanup": `setTxTimeoutMs(0)` plus
`if (Serial)` guards, so a line only prints if a host is attached **at the instant
it runs**. Every interesting print is in `setup()`, which runs **once at reset** —
and on native USB the port **re-enumerates** on reset, so the monitor reconnects
*after* `setup()`'s output has already flown by.

The fix keeps the headless behaviour but catches the log — wait for the host,
**with a timeout** so a battery boot doesn't hang:

```cpp
Serial.begin(115200);
Serial.setTxTimeoutMs(0);
uint32_t t0 = millis();
while (!Serial && (millis() - t0) < 1500) delay(10);   // catch a monitor if present
```

The moment we could read the log, it revealed bug #1 — the probe whose clock never
started. **Making failure visible is a prerequisite for fixing it.** It's not a
coincidence the LEDC bug survived four rounds of a *blind* board.

### Reading the cleaned-up log: real vs benign

```
SY6970 online.
[E][Wire.cpp:135] setPins(): bus already initialized. change pins only when not.
LTR-553 online.
[E][esp32-hal-periman.c] No deinit function for type SPI_MASTER_MISO (pin 8)
CAMERA OK: sensor PID 0x5640
SY6970 online.          <- the recovery init: the bus came back
LTR-553 online.
```

The two `[E]` lines are **benign core chatter**, inherent to sharing buses across
libraries — not bugs:

- **`Wire … already initialized`** — touch, PMU and ALS each call `Wire.begin()`
  in their own `begin()`; the 2nd/3rd just re-assert pins that are already right.
- **`SPI_MASTER_MISO (pin 8)`** — `initSD` gives the card its own `SPIClass` on the
  display's MISO. Both share it fine at runtime (Stage 6). Known Arduino-ESP32 3.x
  quirk.

They're all emitted by the core's `log_e`, gated by `CORE_DEBUG_LEVEL`. You *could*
silence them with `-DCORE_DEBUG_LEVEL=0`, but it's all-or-nothing — it would also
hide a genuinely useful future core error. Left on during active camera work.

---

## Module 6 — Board notes

- **Sensor is an OV5640** (PID `0x5640`), on a ribbon-cable module, under shielding.
  The vendor `CameraShield` example implies OV2640 — the **pin map matched, the
  sensor didn't**. Don't assume the sensor from the example.
- **`esp_camera` ships with the S3 Arduino core** — no `lib_deps` entry. Header:
  `…/esp32s3/include/espressif__esp32-camera/driver/include/esp_camera.h`.
- **SCCB = I²C port 1, Wire = port 0** on this setup. Same pins, and the camera
  driver leaves them muxed to port 1 on deinit — `Wire.end()`/`begin()` to reclaim.
- Field names are `pin_sccb_sda/scl` (the older `pin_sscb_*` are deprecated typos).

---

## What you built and learned

- ✅ Detected the camera and read its **PID (OV5640)** with `esp_camera_init()`
- ✅ Learned two reasons a hand-rolled scan **can't** see a DVP camera (no clock
  via LEDC; SCCB doesn't ACK like I²C)
- ✅ Recovered a **shared I²C bus** the probe borrowed — `Wire.end()` before
  `begin()`, then re-establish the drivers
- ✅ Made the **native-USB boot log** readable, then told real errors from benign
- ✅ Used **PSRAM** for the first time (the detection framebuffer)
- ✅ Deleted a probe that always lied — and kept its post-mortem in a comment

## The through-line

Four probes; three we built measured the wrong thing:

| # | Probe | Verdict | Reality |
|---|-------|---------|---------|
| 1 | I²C scan, PWDN low | no camera | clock never started; SCCB won't ACK |
| 2 | + PWDN polarity sweep | no camera | same two blind spots, now with more code |
| 3 | + longer settle | no camera | refining a measurement that wasn't happening |
| 4 | **`esp_camera_init()`** | **OV5640** | the driver knows the protocol |

This is the same theme as `SD.cardType()` (Stage 6) and the bounce probe / power
bench (Stage 5): **before trusting a number, check what it's derived from — and
whether the instrument can even produce a different answer under each hypothesis.**
Our probe *couldn't*: with no clock and no ACK, it was physically incapable of ever
printing "camera found." A verdict an instrument can't help but return is not
evidence.

## Command cheat-sheet

```bash
# See the boot log on native USB: start monitor, THEN reset (or upload+monitor)
pio run -t upload -t monitor
# esp_camera is bundled — confirm the header exists for your target:
find ~/.platformio -path '*esp32s3*esp_camera.h'
```

## Glossary

- **DVP** — Digital Video Port: parallel camera interface (8 data + PCLK/VSYNC/HREF).
- **SCCB** — OmniVision's I²C-like sensor control bus (SIOD/SIOC = SDA/SCL).
- **XCLK** — master clock the MCU generates *for* the sensor; its logic runs on it.
- **PID** — sensor product ID; `esp_camera_sensor_get()->id.PID`. `0x5640` = OV5640.
- **Strapping pin** — GPIO sampled at reset to set boot mode; here 45/46.
- **Idempotent `begin()`** — re-calling does nothing; `end()` first to force a redo.

## Next lesson

Detection only. The payoff is a **live viewfinder**: capture a frame into PSRAM and
push it to the ST7796 — the camera and the display finally in the same pipeline,
and the first heavy use of that 8 MB. Otherwise the deferred threads remain: log
the battery gauge to CSV (Stage 6 card), a button-driven UI, or the LVGL flex/grid
refactor.
