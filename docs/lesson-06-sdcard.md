# Lesson 06 — microSD: sharing a bus, and a diagnostic that lied

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Mount the microSD card — which **shares the display's SPI
bus** — then read and write real files. The bus sharing worked first time because we
read the driver source. The card took five attempts because we trusted a return
value we hadn't checked.

---

## Learning objectives

By the end of this lesson you can:

1. Explain how **two devices share one SPI bus** — and what each driver must do.
2. Know that **init order matters** even when run-time sharing is fine.
3. Mount a card, find its **fastest working clock**, and know why the ladder
   direction matters.
4. Read and write files: `File`, `close()`, and `FILE_APPEND` vs `FILE_WRITE`.
5. Recognise a **diagnostic that cannot distinguish the cases it claims to**.

---

## Module 1 — One bus, two devices

```
SCLK 18, MOSI 17, MISO 8   ← shared by both
CS 39 → display (ST7796)   CS 14 → microSD
```

Only the device whose **chip-select** is low listens; the others must tri-state.
That's the easy half. The hard half is that each driver assumes it owns the bus's
**clock speed and mode**, so they must bracket every transfer.

Before writing a line of code we checked whether our display driver actually does
that:

```cpp
// Arduino_ESP32SPI.cpp
void Arduino_ESP32SPI::beginWrite() {
  if (_is_shared_interface) { spiTransaction(_spi, _div, _dataMode, _bitOrder); }
  ...
}
void Arduino_ESP32SPI::endWrite() {
  if (_is_shared_interface) { spiEndTransaction(_spi); }
}
```

`is_shared_interface` defaults to **true**: it re-establishes its settings on every
write and **releases the bus** afterwards. `SPIClass` does the same around its
transfers. So they cooperate — *provided both are on the same SPI host*:

```cpp
SPIClass sdSPI(FSPI);   // on the ESP32-S3 the core numbers FSPI = 0 = the SPI2 bus
```

Arduino_GFX's non-ESP32 constructor defaults to `FSPI`, so the SD card must use
`FSPI` too. Two *different* hosts matrixed onto the same pins would simply fight.

> **A shared bus is a shared mutable resource.** Correctness depends on every driver
> bracketing its access — the embedded equivalent of two threads and one lock.

---

## Module 2 — Run-time sharing is fine; INIT order is not

Debugging a mount failure, we moved `initSD()` *before* `panel->begin()`. Result:
**the screen went black.**

`SPIClass::begin()` claimed the pins, and `panel->begin()` couldn't then drive a bus
another driver already owned. Note this is *not* the same as run-time sharing, which
works fine — it's about who sets the bus up.

```cpp
panel->begin();   // display FIRST — it must own the bus setup
...
initSD();         // then the card
```

A probe that must run early can borrow the bus and hand it back:

```cpp
SD.end();
sdSPI.end();      // release before the display initialises
```

---

## Module 3 — Mounting, and getting the ladder the right way round

SD cards must be initialised at a low clock, and cards vary in what they tolerate.
So we try a range and report which worked — turning a boolean failure into a data
point:

```cpp
// FASTEST FIRST.
static const uint32_t sdSpeeds[] = {20000000, 10000000, 4000000, 1000000, 400000};
for (uint8_t i = 0; i < 5 && !sdOk; i++) {
  if (SD.begin(SD_CS, sdSPI, sdSpeeds[i])) { sdOk = true; sdHz = sdSpeeds[i]; }
  else SD.end();
}
```

**The first version ran slow→fast and stopped at the first success — which finds the
*slowest* working speed.** It reported 400 kHz (~50 KB/s) when the card was perfectly
happy at 20 MHz (~2.5 MB/s), a 50× difference. A search that stops at the first hit
must be ordered by what you actually want.

Final: `SDHC 61120MB@20000k`.

---

## Module 4 — The diagnostic that lied

The card would not mount. To distinguish causes, the failure path reported the card
type:

```cpp
snprintf(sdInfo, ..., "SD fail t=%u", SD.cardType());
```

The reasoning was: `t=0` (`CARD_NONE`) means no communication → electrical; `t=3`
(`CARD_SDHC`) means the card answered but the filesystem is unreadable → formatting.
It reported `t=0`, so we chased the electrical path — MISO contention, init order,
floating chip-selects, clock speeds. Confidently ruled out formatting.

**Reformatting the card as FAT32 fixed it immediately.**

`SD.cardType()` reads from the **mounted card structure**. If `begin()` fails for
*any* reason there is no mounted card, so it returns `CARD_NONE` — always. The field
could never distinguish the two cases it was built to distinguish. It wasn't a wrong
reading; it was a **meaningless** one, and it was used to rule out the correct
answer.

The underlying hardware fact still matters: **cards larger than 32 GB ship as
exFAT**, and the ESP32 `SD` library mounts only FAT16/FAT32. Reformat with FAT32
(`mkfs.vfat -F 32` on Linux).

> **The lesson isn't "check the format first."** It's that a diagnostic is only worth
> what its derivation is worth. We read the *driver source* before designing the bus
> sharing, and that worked first time. We read a *return value* and assumed its
> meaning, and it cost four wrong turns.

This was the third instrument in two stages to measure the wrong thing — after a
bounce probe that sampled the wrong interval and a power mode that measured its own
polling ([05e](lesson-05e-power-meter.md), [05f](lesson-05f-button-wake.md)).

---

## Module 5 — Files

```cpp
File f = SD.open("/selftest.txt", FILE_WRITE);
if (!f) return false;            // File has a bool conversion; no exception to catch
f.println("tdisplay");
f.close();                       // data sits in a buffer until you close

f = SD.open("/selftest.txt", FILE_READ);
String back = f.readStringUntil('\n');
f.close();
return back.startsWith("tdisplay");
```

**For Python people:** `SD.open()` doesn't raise — you test the object. And there's
no `with` block, so **`close()` is yours to call**; forget it and writes vanish
silently. (The C++ equivalent of `with` is RAII: a wrapper whose destructor closes
the file.)

**`FILE_APPEND` vs `FILE_WRITE` is the difference between a log and a single line** —
`FILE_WRITE` truncates:

```cpp
File f = SD.open("/boots.csv", FILE_APPEND);
f.printf("%lu,%s\n", (unsigned long)bootCount, why);
f.close();
```

Reading the line count back is a cheap read-side check. On screen:
`#16 button SD57G@20M rw L3` — and **L climbs on every reset**.

Compare with `RTC_DATA_ATTR` from [05d](lesson-05d-deep-sleep.md): that survives deep
sleep but dies on power loss. This survives power loss *and reflashing*. Three
storage tiers now: RAM → RTC memory → SD card.

---

## Module 6 — Board notes

- **LilyGO ship no SD example** for this board (16 examples, none for SD). We were
  proving this path ourselves.
- Their `utilities.h` lists **GPIO 16 as both a user button and `VIBRATING_MOTOR`**,
  and claims three user buttons where the board has two ([05f](lesson-05f-button-wake.md)).
  We only ever *read* GPIO 16 — driving it as an output would buzz a motor.
- Pins confirmed from that same header: `BOARD_SD_CS = 14`, SPI shared with the TFT.

> Vendor headers are a starting point, not ground truth. This one was wrong about the
> buttons, ambiguous about GPIO 16, and silent about SD.

---

## What you built and learned

- ✅ Mounted a microSD **sharing the display's SPI bus** — verified by reading the
  driver source, not by hoping
- ✅ Learned that **run-time sharing and init order are different problems**
- ✅ Found the card's **fastest** working clock (20 MHz), after fixing a ladder that
  searched the wrong way round
- ✅ Read and wrote real files; **appended a boot log that survives power loss**
- ✅ Learned `File`'s bool test, why `close()` matters, `FILE_APPEND` vs `FILE_WRITE`
- ✅ Learned that **a diagnostic is worth exactly what its derivation is worth**

## Command cheat-sheet

```bash
# Card >32GB won't mount? It's exFAT from the factory. Reformat FAT32:
sudo mkfs.vfat -F 32 /dev/sdX1     # check the device name FIRST
```

## Glossary

- **Chip select (CS)** — per-device enable; only the low one listens.
- **SPI host** — the hardware peripheral (S3: `FSPI`=0=SPI2, `HSPI`=1=SPI3).
- **`is_shared_interface`** — Arduino_GFX flag: bracket every transfer, release the bus.
- **exFAT vs FAT32** — >32 GB cards ship exFAT; the ESP32 `SD` library needs FAT16/32.
- **`FILE_APPEND`** — open for writing at the end; `FILE_WRITE` truncates.

## Next lesson

The card is a logging destination — the obvious next build is writing the battery
gauge's readings to CSV over time. Otherwise: a **button-driven UI**, or the
**LVGL flex/grid refactor** the layout collisions keep arguing for.
