# Lesson 10 — WiFi first light: proving the radio with a scan

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Bring up the **radio** — the last untouched subsystem — the
same way we brought up everything else: the smallest thing that proves it works. For
a radio that's a **scan**: list the WiFi networks in range. No credentials, no
network decisions, just "is the antenna alive?" It is — five networks, right signal
strengths.

---

## Learning objectives

By the end of this lesson you can:

1. Explain why the radio needs **no bus-sharing dance** (unlike the camera or SD).
2. Run a WiFi **scan** and read SSID / RSSI / encryption.
3. Interpret **RSSI** (dBm) as signal strength.
4. Turn the radio **off** when idle, and know why that matters here.
5. Rule out your own recent change **by timing** before blaming it.

---

## Module 0 — Why the radio is the easy subsystem (for once)

Every peripheral in Stages 6–9 came with a sharing problem: the SD card and display
fight over **SPI**; the camera and the sensors fight over **I²C**; capture juggled
**three buses at once**. The radio has none of that. It's **on-die**, with its own
dedicated hardware and antenna — it doesn't touch SPI, I²C, or any GPIO we use. So
there's no recovery helper, no init-order trap, no "give the bus back." It just needs
**power and RAM**.

That "RAM" (and flash) is the one real cost, and it's visible:

```
Flash: 26% → 44%    RAM: 33% → 42%
```

Adding `#include <WiFi.h>` pulled in the TCP/IP stack, the supplicant, crypto — ~600
KB of flash. The radio is *heavy*, even though it's electrically simple. Worth seeing
the number: a single include can be a big commitment.

---

## Module 1 — A scan is the radio's "hello world"

The same discipline as Stage 1 (serial hello) and Stage 2a (first light): don't
connect, don't configure — just prove the hardware responds. A scan is perfect
because it needs **no credentials** and **no STA-vs-AP decision** (both of those come
when you actually *join* a network, Stage 11).

```cpp
#include <WiFi.h>            // bundled with the ESP32 core — no lib_deps

WiFi.mode(WIFI_STA);         // station mode: we look FOR access points
WiFi.disconnect();           // drop any stale association first
delay(100);
int n = WiFi.scanNetworks(); // BLOCKS ~2–4 s; returns the count (negative = error)

for (int i = 0; i < n; i++) {
  WiFi.SSID(i);              // network name (String)
  WiFi.RSSI(i);              // signal strength, dBm (negative)
  WiFi.encryptionType(i);    // WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, …
}
```

`scanNetworks()` **blocks** for a few seconds while the radio sweeps channels — fine
here (we've taken over the screen), but a reason real apps scan asynchronously.

---

## Module 2 — Reading RSSI

**RSSI is a negative dBm number, and closer to zero is stronger.** From our result:

| Network | RSSI | Meaning |
|---|---|---|
| Hyperoptic Fibre 4F40 | **-40** | strong — a few metres away |
| NOWTVBD712 | **-76** | weak — through walls / next door |

Rough scale: **-30** excellent, **-67** good, **-70** okay, **-80** marginal,
**-90** unusable. So a first scan doubles as a sanity check on the antenna: if your
*own* network — which you know is close — showed up at -85, something's wrong with the
RF. Ours came in at -40, exactly right.

Encryption: nearly everything reads `WIFI_AUTH_*` (locked, shown grey with a `#`); an
open network shows white. On screen and on serial, one row each.

---

## Module 3 — Turn the radio off again

```cpp
WiFi.scanDelete();       // free the scan result list
WiFi.mode(WIFI_OFF);     // stop the radio idling
```

After five stages of power work (light sleep, deep sleep, backlight, the inline
meter), leaving the radio **on** would quietly undo a lot of it — an idle WiFi radio
draws **tens of mA**, in the same league as the whole rest of the board. First light
shouldn't wreck the power budget, so the scan powers the radio down when it's done.
`scanDelete()` frees the result buffer — the same manual-cleanup rule as `fb_return`
and `free(jpg)`.

---

## Module 4 — The button we just re-found earns a job

The scan is triggered by the **GPIO 12 button** — the rocker half we re-discovered
one stage ago (Stage 5f's "only two buttons" was wrong; the controls are rockers).
It's read exactly like the viewfinder's GPIO 16 exit:

```cpp
static bool wifiBtnPrevHigh = true;
bool wifiHigh = (digitalRead(BTN_WIFI) == HIGH);
if (wifiBtnPrevHigh && !wifiHigh) { wifiBtnPrevHigh = wifiHigh; runWifiScan(); return; }
wifiBtnPrevHigh = wifiHigh;
```

A satisfying loop closes: we corrected a measurement mistake about GPIO 12, and the
very next stage put it to work — the same pattern as Stage 5f's GPIO 16, where a new
input immediately found a use.

---

## Module 5 — A debugging aside: rule out your own change first

The scan build's *first* boot also showed the SD card failing:

```
sd_diskio.cpp: sdcard_mount(): f_mount failed: (3) The physical drive cannot work   ×5
```

Two disciplines kept this from becoming a wild-goose chase:

1. **Timing exonerated the new code.** SD mounts in `setup()`, seconds *before* any
   WiFi code can run (the radio only wakes on a button press). A change that can't
   have executed yet can't be the cause.
2. **The error *kind* pointed at hardware.** `(3)` is FatFs `FR_NOT_READY` — a
   *block-level comms* failure across all five clock speeds, not a filesystem error.
   That's "no card responding," not "bad format." (Contrast Stage 6, where
   `SD.cardType()` gave a *meaningless* code and we misdiagnosed a format issue as
   electrical — here the error was specific and honest.)

Cause: the card had been pulled to view the JPEGs on a computer and wasn't reseated.
Reseat → clean boot. **Before blaming the thing you just changed, check whether it
even ran, and whether the error's *type* matches your change.**

---

## What you built and learned

- ✅ Proved the **radio + antenna** with a scan — five networks, correct RSSI
- ✅ Learned the radio needs **no bus-sharing** — but costs ~600 KB of flash
- ✅ Read **SSID / RSSI / encryption**; interpreted dBm signal strength
- ✅ Powered the radio **off** when idle to protect the power budget
- ✅ Put the **re-found GPIO 12 button** to its first real use
- ✅ Practised **ruling out a recent change by timing + error-kind**

## Command cheat-sheet

```bash
pio run -t upload -t monitor     # press GPIO 12; the scan prints to serial too
```

## Glossary

- **Scan** — sweep channels for advertised networks; needs no credentials.
- **SSID** — the network name.
- **RSSI** — received signal strength, dBm; negative, closer to 0 = stronger.
- **STA (station)** — the board joins an existing AP. **AP** — the board *is* the AP.
- **`WIFI_AUTH_OPEN`** — unencrypted network (vs `WIFI_AUTH_WPA2_PSK`, etc.).

## Next lesson

The radio works; now **join** a network (Stage 11) — `WiFi.begin(ssid, pass)`, wait
for `WL_CONNECTED`, print the IP. That's where credentials and the **STA-vs-AP**
decision come in. Then Stage 12: an **HTTP server** that serves the SD card's
captured JPEGs to a phone — tying camera, SD and radio together.
