# Lesson 15 — instrumenting the firmware itself: memory, CPU, tasks

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Every earlier stage went well when we made invisible
state visible. This turns that method on the **firmware itself** — how much
memory is left, how *usable* it is, what the CPU is really doing, and what the
radios cost in bytes. It is also the project's first real **C++ class in its own
module**, the first step in breaking up a 1500-line `main.cpp`.

---

## Learning objectives

By the end of this lesson you can:

1. Explain why **free memory** and **allocatable memory** are different numbers,
   and measure the gap (**fragmentation**).
2. Distinguish the ESP32-S3's **separate memory pools** — internal SRAM, PSRAM,
   DMA-capable — and say why "which pool" beats "how much".
3. Read FreeRTOS **task statistics**: per-task CPU share, core affinity, and
   **stack low-water marks**.
4. Write a **C++ class in a header/implementation pair**, and say what
   `constexpr`, `private:`, `const` methods and a `Print &` reference buy you.

---

## Module 1 — The C++ side: a real class, at last

Up to Stage 14 this project was *good C*: global variables and free functions in
one enormous file. Stage 15 introduces two proper modules.

**`include/board_pins.h`** — every pin, converted from `#define` to `constexpr`.

A `#define` is a **preprocessor** substitution. The compiler never sees the name,
only the text that replaced it: no type, no scope, invisible in a debugger, and
free to collide with any library header. A `constexpr` is a real typed variable
that happens to be known at compile time — identical zero cost at runtime, but
**typed, scoped and debuggable**. In modern C++ it is the default; `#define` is
reserved for what constexpr *cannot* do (conditional compilation, stringising).

The conversion required **no other changes anywhere** — 40-odd constants swapped
and the build stayed clean. That is the sign of a genuinely drop-in improvement.

**`src/ResourceMonitor.{h,cpp}`** — the monitor as a class.

- The **header** declares *what exists* (the interface); the **`.cpp`** defines
  *how it works*. Other files include only the header. Python has no equivalent:
  there, the definition *is* the declaration.
- `private:` state is protected **by the compiler**, not by a leading-underscore
  convention.
- `const` on a method is a **promise not to modify the object**, and the compiler
  enforces it. Note `reportTasks()` is deliberately *not* const — measuring CPU
  requires remembering the previous sample, so it genuinely mutates.
- `void reportMemory(Print &out) const` takes a **reference to an abstract base
  class**. `Serial` is a `Print` — but so is a `WiFiClient` or a `String` buffer.
  The class can therefore aim its reports anywhere without knowing where. That is
  polymorphism, and it is why `Serial` is not hard-coded inside.

Two design choices that are really engineering judgements:

- **A fixed array, not `std::vector`.** This class *measures allocation*. If it
  allocated to do its job it would perturb the number it reports — a voltmeter
  that draws current. Same observer-effect tension that kept the report on
  **serial** rather than redrawing the screen.
- **32-bit `size_t`.** A 64 GB card's byte count does not fit in a 32-bit
  `size_t`, so SD capacities stay `uint64_t` and never pass through the byte
  formatter. One of the few places on this chip where 64-bit maths is mandatory.

---

## Module 2 — Memory is not one number

At boot, with everything up:

```
heap      free   189.6 KB  of  229.9 KB   (18% used)
          low    153.5 KB              <- least free since boot
          max    136.0 KB              <- largest single alloc  (frag 29%)
psram     free    7.97 MB  of   8.00 MB   (frag 2%)
internal  free   189.6 KB   max 136.0 KB  <- on-chip SRAM only
dma-able  free   182.2 KB   max 136.0 KB  <- what the camera/SPI can use
```

Four different things worth tracking:

- **Free** — the total, and the number everyone quotes.
- **Largest block** — the biggest single `malloc()` that can actually succeed.
- **Low-water mark** — the least free *ever*. A snapshot lies; this tells you how
  close you came to dying.
- **Pool** — internal SRAM, PSRAM and DMA-capable are **separate**. PSRAM being
  free does not help a peripheral whose DMA engine cannot reach it.

**Fragmentation** is the gap between *free* and *largest block*, and it is the
number that ruins your day silently. 120 KB free with a 40 KB largest block means
a 50 KB request **fails** while the "free heap" reading looks perfectly healthy.

---

## Module 3 — The measurement: the heap shatters

Bracketing each radio with `mark()` / `reportDelta()` — the "trust the delta"
method from the Stage 5e and Stage 14 power benches, applied to bytes:

| State | heap free | largest block | frag |
|-------|----------:|--------------:|-----:|
| boot | 189.6 KB | **136 KB** | 29% |
| after a WiFi session | 171 KB | **95 KB** | 45% |
| after a BLE session | 171 KB | **59 KB** | 66% |

Free heap fell **19 KB**. The largest allocatable block fell **77 KB**.

The BLE row is the sharpest illustration: its heap delta was **+400 bytes** — BLE
returned every byte it borrowed — yet the largest block still collapsed from
95 KB to 59 KB. **It gave back the quantity but not the contiguity.**

Two refinements from repeated runs:

- **Fragmentation is not progressive.** Across three WiFi sessions the figures
  held rock-steady at `max 59K / frag 65%`. The heap shattered once during first
  bring-up and then stabilised.
- **It never recovers.** You are stuck with a 59 KB ceiling until reboot.

---

## Module 4 — What the radios cost in bytes (Stage 14 inverts)

Peak cost, read from the low-water-mark deltas:

| Radio | Power (Stage 14) | Peak RAM (Stage 15) |
|-------|-----------------:|--------------------:|
| WiFi AP | **+45 mA** | −17 KB |
| BLE | +9 mA | **−38 KB** |

**BLE is ~5× cheaper in power and more than 2× more expensive in peak RAM.**
"Cheap" is meaningless until you name the resource. Neither measurement alone
would have told you this.

**And the first WiFi session is special:**

| WiFi session | heap delta | low-water delta |
|--------------|-----------:|----------------:|
| 1st | −18,324 B | −17,144 B |
| 2nd | −280 B | −12 B |
| 3rd | −280 B | **+0 B** |

The ~18 KB is WiFi's **network stack initialising once** (lwIP, netif, the event
loop) and staying resident. Every later session costs 280 bytes and dips the heap
by nothing.

This settles a worry Stage 14 raised. Stage 14's rule was "duty-cycle the radio
hard, never leave the AP up." One might fear that cycling it churns memory. It
does not — **you pay the 18 KB once**. The power rule and the memory rule agree.

*(The residual 280 bytes is identical every session, so it is a deterministic
retained allocation rather than noise — tiny, but real.)*

---

## Module 5 — The task list: half the chip is asleep

```
name        core  prio   CPU%   min-free-stack
loopTask    1        1    2.7     3700 B
IDLE0       0        0   52.6      236 B
IDLE1       1        0   43.6      356 B
Tmr Svc     any      1    0.0     2428 B
ipc1        1       24    0.7      260 B
ipc0        0       24    0.4      252 B
esp_timer   0       22    0.0     7940 B
```

Three reveals:

1. **Arduino has been running 7 tasks all along.** `setup()`/`loop()` are not the
   program — they are one task called `loopTask`.
2. **All of your code is pinned to core 1**, using 2.7%. Core 0 is essentially
   idle. There is a whole second processor doing nothing.
3. **The system tasks have under ~300 bytes of stack headroom.** That is normal
   for ESP-IDF but it is a live tripwire: *never do real work in an idle hook or
   an IPC callback on this chip*. Meanwhile `esp_timer` is over-provisioned by
   ~8 KB. **Low-water marks are how you size a stack instead of guessing.**

CPU percentages are computed against the **sum of both cores'** run-time, so a
fully idle board reads ~96% idle split across `IDLE0` + `IDLE1`. And CPU usage is
a **rate**: one reading of a cumulative counter is meaningless, so the monitor
keeps the previous sample and reports the difference — the same two-sample
discipline as every power bench in this project.

---

## Module 6 — The finding that vindicates Stage 7

With the heap at 66% fragmentation and a 59 KB ceiling, the camera was still
re-initialised and run:

```
capture /IMG_0015.JPG: OK (5594 bytes)
viewfinder: 139 frames / 7167 ms = ~19 fps
```

Full frame rate on a badly fragmented heap. The reason is a decision made back in
Stage 7: **`CAMERA_FB_IN_PSRAM`**. The large frame buffers come from PSRAM — 2%
fragmented, ~8 MB free — so only small DMA descriptors need the shattered
internal SRAM.

That configuration choice quietly immunised the camera against exactly this
failure, and we only discovered *why it mattered* eight stages later. The lesson
generalises:

> **Which pool an allocation comes from matters more than how much total RAM is free.**

---

## What you built and learned

- ✅ A `ResourceMonitor` **C++ class** in a proper header/`.cpp` pair
- ✅ `constexpr` pins in `board_pins.h`, replacing 40-odd `#define`s
- ✅ Measured **fragmentation**: 136 KB → 59 KB largest block, at ~constant free heap
- ✅ Found the radios' **RAM** cost — and that it **inverts** their power ranking
- ✅ Proved WiFi's 18 KB is a **one-time** stack init, not a per-session leak
- ✅ Read **task stats**: 7 tasks, code pinned to core 1, core 0 idle
- ✅ Learned to size stacks from **low-water marks**

## Command cheat-sheet

```bash
pio run -t upload && pio device monitor
# boot prints a full RESOURCE REPORT; "[res]" one-liner every 10 s
# tap GPIO 12  -> WiFi AP  -> DELTA report on exit
# hold GPIO 12 -> BLE      -> DELTA report on exit
```

## Glossary

- **Fragmentation** — free memory split into pieces too small to be useful; the
  gap between *free* and *largest allocatable block*.
- **Low-water mark** — the minimum a resource ever reached; the honest measure of
  how close you came to failure.
- **DMA-capable memory** — RAM a peripheral's DMA engine can physically address.
- **Core affinity** — which core a task is pinned to (`tskNO_AFFINITY` = either).
- **Translation unit** — one `.cpp` plus everything it includes; the compiler's
  unit of work.

## Where this leaves the project

The board tour (Stages 1–14) is now joined by instrumentation of the **software**.
The recurring method has held for a fourth time: *hold a state, subtract a
baseline, trust only what is above the noise floor* — as true for bytes as it was
for milliamps, wake counts and frames per second.

The task list also points at the obvious next stage: **one core is doing nothing**,
and we now own the instruments (stack low-water marks, per-task CPU) needed to use
the second one safely.
