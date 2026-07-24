// ============================================================================
//  ResourceMonitor.cpp — the implementation behind ResourceMonitor.h
// ============================================================================

#include "ResourceMonitor.h"

#include "esp_heap_caps.h"        // heap_caps_*: the per-capability heap API
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"        // uxTaskGetSystemState, TaskStatus_t
#include <SD.h>

// ---------------------------------------------------------------------------
//  Fragmentation
//
//  "How much of my free space is unusable as one piece?"
//  0   = the free space is one contiguous run (ideal)
//  100 = free space exists but nothing large can be allocated
// ---------------------------------------------------------------------------
uint32_t ResourceMonitor::Memory::heapFragPct() const {
  if (heapFree == 0) return 0;
  return (uint32_t)(100 - (heapLargest * 100 / heapFree));
}

uint32_t ResourceMonitor::Memory::psramFragPct() const {
  if (psramFree == 0) return 0;
  return (uint32_t)(100 - (psramLargest * 100 / psramFree));
}

// ---------------------------------------------------------------------------
void ResourceMonitor::printBytes(Print &out, size_t bytes) {
  if (bytes >= 1024UL * 1024UL) out.printf("%7.2f MB", bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024UL)     out.printf("%7.1f KB", bytes / 1024.0);
  else                          out.printf("%7u B ", (unsigned)bytes);
}

// ---------------------------------------------------------------------------
//  Snapshot. Note these are all cheap register/bookkeeping reads — taking a
//  snapshot does not itself allocate, which is what makes it trustworthy.
// ---------------------------------------------------------------------------
ResourceMonitor::Memory ResourceMonitor::memory() const {
  Memory m;

  m.heapTotal    = ESP.getHeapSize();
  m.heapFree     = ESP.getFreeHeap();
  m.heapMinFree  = ESP.getMinFreeHeap();
  m.heapLargest  = ESP.getMaxAllocHeap();

  m.psramTotal   = ESP.getPsramSize();
  m.psramFree    = ESP.getFreePsram();
  m.psramMinFree = ESP.getMinFreePsram();
  m.psramLargest = ESP.getMaxAllocPsram();

  // MALLOC_CAP_INTERNAL = on-chip SRAM only (excludes PSRAM).
  // MALLOC_CAP_DMA      = memory a peripheral's DMA engine can actually reach.
  m.internalFree    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  m.dmaFree         = heap_caps_get_free_size(MALLOC_CAP_DMA);
  m.dmaLargest      = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

  return m;   // returned by value; the compiler elides the copy
}

void ResourceMonitor::begin() {
  mark("boot");
}

void ResourceMonitor::mark(const char *label) {
  _baseline = memory();
  snprintf(_baselineLabel, sizeof(_baselineLabel), "%s", label ? label : "?");
  _haveBaseline = true;
}

// ---------------------------------------------------------------------------
void ResourceMonitor::reportMemory(Print &out) const {
  Memory m = memory();

  out.println(F("--- MEMORY ---------------------------------------------"));

  out.print(F("  heap      free "));   printBytes(out, m.heapFree);
  out.print(F("  of "));               printBytes(out, m.heapTotal);
  out.printf("  (%u%% used)\n",
             m.heapTotal ? (unsigned)(100 - m.heapFree * 100 / m.heapTotal) : 0);

  out.print(F("            low  "));   printBytes(out, m.heapMinFree);
  out.println(F("   <- least free since boot"));

  out.print(F("            max  "));   printBytes(out, m.heapLargest);
  out.printf("   <- largest single alloc  (frag %u%%)\n", m.heapFragPct());

  out.print(F("  psram     free "));   printBytes(out, m.psramFree);
  out.print(F("  of "));               printBytes(out, m.psramTotal);
  out.printf("  (frag %u%%)\n", m.psramFragPct());

  out.print(F("            low  "));   printBytes(out, m.psramMinFree);
  out.println();

  out.print(F("  internal  free "));   printBytes(out, m.internalFree);
  out.print(F("   max "));             printBytes(out, m.internalLargest);
  out.println(F("   <- on-chip SRAM only"));

  out.print(F("  dma-able  free "));   printBytes(out, m.dmaFree);
  out.print(F("   max "));             printBytes(out, m.dmaLargest);
  out.println(F("   <- what the camera/SPI can use"));
}

// ---------------------------------------------------------------------------
void ResourceMonitor::reportDelta(Print &out) const {
  if (!_haveBaseline) { out.println(F("  (no baseline marked)")); return; }

  Memory now = memory();

  // Signed arithmetic on purpose: a NEGATIVE delta means memory was consumed.
  long dHeap  = (long)now.heapFree  - (long)_baseline.heapFree;
  long dPsram = (long)now.psramFree - (long)_baseline.psramFree;
  long dLow   = (long)now.heapMinFree - (long)_baseline.heapMinFree;

  out.printf("--- DELTA since \"%s\" -------------------------\n", _baselineLabel);
  out.printf("  heap  free   %+8ld bytes\n", dHeap);
  out.printf("  psram free   %+8ld bytes\n", dPsram);
  out.printf("  heap  low    %+8ld bytes   <- how much DEEPER the heap dipped\n", dLow);
  out.println(F("  (negative = consumed. A near-zero heap delta after a"));
  out.println(F("   subsystem shuts down means it gave its memory back.)"));
}

// ---------------------------------------------------------------------------
//  CPU + tasks.
//
//  FreeRTOS keeps a cumulative run-time counter per task. A single reading is
//  meaningless (it's "since boot"); usage is a RATE, so we need two samples and
//  the difference between them — exactly the delta discipline from the power
//  benches. The first call therefore only establishes a reference.
//
//  Percentages are computed against the SUM of all deltas rather than wall
//  clock, because this is a DUAL-CORE chip: two cores each accrue run-time, so
//  shares are of total CPU-time available across both cores.
// ---------------------------------------------------------------------------
void ResourceMonitor::reportTasks(Print &out) {
  TaskStatus_t status[kMaxTasks];
  uint32_t     totalRunTime = 0;

  UBaseType_t count = uxTaskGetSystemState(status, kMaxTasks, &totalRunTime);
  if (count == 0) { out.println(F("  (task stats unavailable)")); return; }

  // Work out each task's run-time since the PREVIOUS call.
  uint32_t deltas[kMaxTasks] = {0};
  uint32_t totalDelta = 0;

  for (UBaseType_t i = 0; i < count; i++) {
    uintptr_t h = (uintptr_t)status[i].xHandle;
    uint32_t  prevRt = 0;
    for (size_t j = 0; j < _prevCount; j++) {
      if (_prev[j].handle == h) { prevRt = _prev[j].runtime; break; }
    }
    // Guard against a counter that wrapped or a task seen for the first time.
    deltas[i] = (status[i].ulRunTimeCounter >= prevRt)
                  ? (status[i].ulRunTimeCounter - prevRt) : 0;
    totalDelta += deltas[i];
  }

  out.printf("--- TASKS (%u running) ----------------------------------\n",
             (unsigned)count);

  // On the first call there is no previous sample, so the "delta" is the whole
  // counter — i.e. the average since boot. That IS meaningful, just not recent.
  if (!_havePrev) {
    out.println(F("  first sample: CPU% is the AVERAGE SINCE BOOT"));
    out.println(F("  (call again for usage since the previous report)"));
  }

  out.println(F("  name              core  prio   CPU%   min-free-stack"));

  uint32_t idleDelta = 0;

  for (UBaseType_t i = 0; i < count; i++) {
    float pct = (totalDelta > 0) ? (100.0f * deltas[i] / totalDelta) : 0.0f;

    // The idle tasks' share is time the CPU had nothing to do.
    if (strncmp(status[i].pcTaskName, "IDLE", 4) == 0) idleDelta += deltas[i];

    // xCoreID is tskNO_AFFINITY for tasks that may run on either core.
    char core[5];
    if (status[i].xCoreID >= 2) snprintf(core, sizeof(core), "any");
    else                        snprintf(core, sizeof(core), "%d", (int)status[i].xCoreID);

    out.printf("  %-16s  %-4s  %4u  %5.1f   %6u B\n",
               status[i].pcTaskName,
               core,
               (unsigned)status[i].uxCurrentPriority,
               pct,
               (unsigned)status[i].usStackHighWaterMark);
  }

  if (_havePrev && totalDelta > 0) {
    float busy = 100.0f - (100.0f * idleDelta / totalDelta);
    out.printf("  => CPU busy %.1f%% of both cores combined (idle %.1f%%)\n",
               busy, 100.0f - busy);
  }
  out.println(F("  min-free-stack is the LOW-WATER mark: the closest that task"));
  out.println(F("  ever came to overflowing. Size stacks from this, not guesses."));

  // Save this sample as the reference for next time.
  _prevCount = (count < kMaxTasks) ? count : kMaxTasks;
  for (size_t i = 0; i < _prevCount; i++) {
    _prev[i].handle  = (uintptr_t)status[i].xHandle;
    _prev[i].runtime = status[i].ulRunTimeCounter;
  }
  _havePrev = true;
}

// ---------------------------------------------------------------------------
void ResourceMonitor::reportSystem(Print &out) const {
  out.println(F("--- SYSTEM ---------------------------------------------"));
  out.printf("  chip        %s rev%d, %d core(s)\n",
             ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  out.printf("  cpu freq    %u MHz\n", (unsigned)getCpuFrequencyMhz());
  out.printf("  temperature %.1f C\n", temperatureRead());
  out.printf("  uptime      %lu s\n", (unsigned long)(millis() / 1000));
  out.printf("  tasks       %u\n", (unsigned)uxTaskGetNumberOfTasks());

  out.print(F("  flash chip  "));      printBytes(out, ESP.getFlashChipSize());
  out.println();
  out.print(F("  sketch uses "));      printBytes(out, ESP.getSketchSize());
  out.print(F("   free for OTA "));    printBytes(out, ESP.getFreeSketchSpace());
  out.println();
  // CPU frequency ties straight back to Stage 5's power work: halving the clock
  // roughly halves the CPU's share of the current draw.
}

// ---------------------------------------------------------------------------
void ResourceMonitor::reportStorage(Print &out) const {
  out.println(F("--- STORAGE --------------------------------------------"));

  if (SD.cardType() == CARD_NONE) {
    out.println(F("  sd          not mounted"));
    return;
  }

  uint64_t total = SD.totalBytes();
  uint64_t used  = SD.usedBytes();

  // NOTE: these stay uint64_t and are never passed through printBytes(), which
  // takes a size_t. On a 32-bit MCU size_t is 32 bits and tops out at 4 GB — a
  // 64 GB card's byte count simply does not fit. Card capacities are one of the
  // few places on this chip where 64-bit arithmetic is mandatory.
  out.printf("  sd card     %.2f GB (raw capacity)\n",
             SD.cardSize() / (1024.0 * 1024.0 * 1024.0));
  out.printf("  filesystem  %.2f MB used of %.2f GB  (%.1f%%)\n",
             used / (1024.0 * 1024.0),
             total / (1024.0 * 1024.0 * 1024.0),
             total ? (100.0 * used / total) : 0.0);
}

// ---------------------------------------------------------------------------
void ResourceMonitor::reportAll(Print &out) {
  out.println();
  out.println(F("========== RESOURCE REPORT ============================="));
  reportSystem(out);
  reportMemory(out);
  reportStorage(out);
  reportTasks(out);
  if (_haveBaseline) reportDelta(out);
  out.println(F("========================================================"));
  out.println();
}

// ---------------------------------------------------------------------------
void ResourceMonitor::line(Print &out) const {
  Memory m = memory();
  out.printf("[res] heap %uK (low %uK, max %uK, frag %u%%)  psram %uK  temp %.0fC\n",
             (unsigned)(m.heapFree / 1024),
             (unsigned)(m.heapMinFree / 1024),
             (unsigned)(m.heapLargest / 1024),
             m.heapFragPct(),
             (unsigned)(m.psramFree / 1024),
             temperatureRead());
}
