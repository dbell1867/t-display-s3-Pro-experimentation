// ============================================================================
//  ResourceMonitor.h — Stage 15: making the system's own resources visible
// ----------------------------------------------------------------------------
//  Every stage of this project went well when we made invisible state visible.
//  This turns that idea on the firmware itself: how much memory is left, how
//  fragmented it is, what the CPU is actually doing, and what's on the SD card.
//
//  This is also the project's first real C++ CLASS in its own module. Up to now
//  main.cpp has been good *C*: global variables and free functions. The things
//  to notice here, coming from Python:
//
//   * HEADER vs .cpp — the header declares WHAT exists (the interface); the .cpp
//     defines HOW it works. Other files #include only the header. Python has no
//     equivalent: there, the definition *is* the declaration.
//
//   * `class` groups the data and the functions that act on it, and `private:`
//     means the outside world genuinely cannot touch that state — enforced by
//     the compiler, not by a leading-underscore convention.
//
//   * `const` on a method ("...(...) const") is a PROMISE that calling it will
//     not modify the object. The compiler enforces the promise. Note that
//     reportTasks() is deliberately NOT const: measuring CPU requires
//     remembering the previous sample, so it really does mutate the object.
//
//   * `Print &out` is a REFERENCE to an abstract base class. `Serial` is a
//     Print, and so is a WiFiClient or a String buffer — so every report can be
//     aimed anywhere without this class knowing where. That is polymorphism,
//     and it's why we don't hard-code Serial inside.
// ============================================================================

#pragma once

#include <Arduino.h>

class ResourceMonitor {
public:
  // A plain value type holding one instant's memory picture. Returned BY VALUE:
  // small structs are cheap to copy, and a copy can't be invalidated later.
  struct Memory {
    // "Heap" here = the default malloc() pool the Arduino core hands you.
    size_t heapTotal    = 0;
    size_t heapFree     = 0;
    size_t heapMinFree  = 0;   // low-water mark: the least free EVER since boot
    size_t heapLargest  = 0;   // biggest single block you could allocate right now

    // PSRAM: the 8MB external chip. Big buffers (camera frames) live here.
    size_t psramTotal   = 0;
    size_t psramFree    = 0;
    size_t psramMinFree = 0;
    size_t psramLargest = 0;

    // Internal SRAM and DMA-capable memory are SEPARATE POOLS. Hardware that
    // does DMA (the camera, SPI) can only use DMA-capable memory, so PSRAM
    // being free does not help it. Tracking these separately is the difference
    // between "I have loads of RAM" and "I have the RIGHT KIND of RAM".
    size_t internalFree    = 0;
    size_t internalLargest = 0;
    size_t dmaFree         = 0;
    size_t dmaLargest      = 0;

    // FRAGMENTATION: the single most useful derived number here.
    // If 120 KB is free but the largest block is 40 KB, the free space is cut
    // into pieces by live allocations and a 50 KB request FAILS despite the
    // "free" figure looking healthy. Returns 0 (perfect) to 100 (shattered).
    uint32_t heapFragPct() const;
    uint32_t psramFragPct() const;
  };

  // Capture the boot baseline. Call once, late in setup().
  void begin();

  // Take a fresh snapshot without printing.
  Memory memory() const;

  // Remember the current memory state under a label, so a later reportDelta()
  // can show what some operation COST. Same "trust the delta" method the power
  // benches used in Stage 5e and Stage 14 — different axis, same discipline.
  void mark(const char *label);

  // --- Reports. All take a destination, none assume Serial. ---
  void reportMemory(Print &out) const;
  void reportSystem(Print &out) const;
  void reportStorage(Print &out) const;
  void reportTasks(Print &out);            // non-const: keeps CPU sampling state
  void reportDelta(Print &out) const;      // vs the last mark()
  void reportAll(Print &out);              // everything, in order

  // One compact line, for logging periodically without flooding the console.
  void line(Print &out) const;

private:
  // A fixed cap instead of a std::vector, and the reason matters: this class
  // MEASURES allocation. If it allocated to do its job it would perturb the
  // very number it reports — the software equivalent of a voltmeter that draws
  // current. Same observer-effect tension as drawing the report on-screen.
  static constexpr size_t kMaxTasks = 32;

  struct TaskSample {
    uintptr_t handle  = 0;   // identifies the task between samples
    uint32_t  runtime = 0;   // cumulative run-time counter at that sample
  };

  TaskSample _prev[kMaxTasks] = {};
  size_t     _prevCount       = 0;
  bool       _havePrev        = false;

  Memory _baseline           = {};
  char   _baselineLabel[24]  = "boot";
  bool   _haveBaseline       = false;

  // Helper: print a byte count as a human-friendly value (e.g. "142.3 KB").
  static void printBytes(Print &out, size_t bytes);
};
