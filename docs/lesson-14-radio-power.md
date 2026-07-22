# Lesson 14 — what the radios cost: measuring WiFi vs BLE power

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** The radios are the hungriest thing on this board — you can
see it on the inline meter. This lesson **measures** that, turning "significantly
more" into numbers, so future low-power apps can make informed choices. The method is
the Stage 5e power bench again: hold each radio state steady, read the meter, trust
the **differences**.

---

## Learning objectives

By the end of this lesson you can:

1. Measure a subsystem's power by **holding a state and reading deltas**.
2. State the **WiFi vs BLE** power cost for this board, from data.
3. Explain why **an AP never sleeps** and why **BLE sips**.
4. Turn the numbers into **low-power design rules**.

---

## Module 1 — The bench, and why deltas

Same tool as Stage 5e: a modal that **holds one state still** so a slow meter can
settle, at **constant full brightness with charging disabled**, advancing on the
GPIO 16 button. Everything except the radio is held constant between states — screen,
CPU, backlight, no charge current — so the **difference** between two readings is the
radio and nothing else. The absolute numbers still include the USB host link and the
rest of the board, but those are **common-mode** and cancel in the subtraction.

(Trigger: the GPIO 16 rocker-half, opposite the GPIO 12 WiFi/BLE half — *use* the
radios with 12, *measure* them with 16.)

---

## Module 2 — The measurements

Baseline (radios off, screen on, on USB): **158 mA**. Then:

| State | Reading | **Δ over baseline** |
|-------|--------:|--------------------:|
| WiFi AP, idle (advertising, no client) | 203 mA | **+45 mA** |
| WiFi AP, phone connected | 204 mA | **+46 mA** |
| BLE, advertising | 167 mA | **+9 mA** |
| BLE, connected + subscribed | 167 mA | **+9 mA** |

All deltas are well above the setup's ~2 mA drift, so they're trustworthy (Stage 5e
established ≥10 mA is solid). The **+45 mA** and **+9 mA** are the numbers to carry
forward.

---

## Module 3 — What the numbers mean

**WiFi is ~5× BLE.** +45 mA vs +9 mA. If a job only needs to move a little data
occasionally, that ratio is the whole argument for BLE.

**The cost is being ON, not being connected.** WiFi idle (203) and WiFi with a client
(204) are the same within the meter's resolution; BLE advertising and BLE connected
are *identical* (167). Powering the radio up is the expense — associating a client
adds nothing measurable at idle. (Sustained *traffic* — streaming, a busy HTTP
transfer — would add TX bursts on top; we measured idle links, not throughput.)

**An AP has no modem-sleep.** +45 mA *continuous*, client or not, because an access
point must stay awake to answer any client at any instant — it can't nap between
beacons. This is the single most important low-power fact about the WiFi on this
board, and it's a direct consequence of the Stage 11 choice: **AP mode is the
power-hungry WiFi mode.** A *station* (STA) joined to a router can use DTIM /
modem-sleep to idle between beacons and would measure much lower — a real trade-off
against the AP's no-router convenience.

**BLE sips by design.** +9 mA even advertising is low because BLE advertises in brief
bursts and sleeps the radio between them; a connection at the default interval is
similarly low-duty. Sipping is the entire reason BLE exists — this measurement is
that design philosophy showing up on the meter.

---

## Module 4 — Low-power design rules (earned from the data)

1. **The radio being on is the dominant cost — so duty-cycle it hard.** Off by
   default; on only long enough to transmit, then off. The firmware already does the
   right thing on every modal exit (`WiFi.mode(WIFI_OFF)`, `BLEDevice::deinit(true)`)
   — the +45/+9 mA vanish the instant you leave the screen, back to the 158 mA
   baseline.
2. **Prefer BLE for small, infrequent data — it's 5× cheaper.** Reserve WiFi for when
   you genuinely need throughput or IP (the photo server).
3. **Never leave a WiFi AP running "just in case."** You pay the full +45 mA from the
   moment `softAP()` runs, before anyone connects. Bring it up on demand, tear it down
   when done.
4. **If you must have WiFi and care about power, use STA + modem-sleep, not AP.** The
   convenience of AP mode (no router) costs you the modem-sleep you'd get as a station.
5. **Remember the baseline is separate.** These deltas are the radio *hardware* cost,
   roughly independent of the rest. In a real low-power app the screen would be off
   (Stage 5c) and the baseline far below 158 mA — which makes the radio's relative
   share even bigger. On a sleeping board, +45 mA of WiFi can dwarf everything else.

---

## Module 5 — Board & method notes

- **Charging disabled during the bench** (`PPM.disableCharge()`), re-enabled on exit —
  charge current contaminated readings in Stage 5e and would here too.
- **Deltas, not absolutes.** The 158 mA baseline is inflated by the USB host link and
  full-brightness screen; none of that matters because it's constant across states.
- **We measured idle links, not throughput.** Active TX (a live HTTP transfer, a
  high-rate BLE stream) draws more in bursts — bursts a slow averaging meter smears,
  so a clean number needs a faster instrument. The idle-link costs are the honest,
  measurable floor.
- The bench holds each state indefinitely, so there's time to connect a phone mid-
  state for the "connected" readings.

---

## What you built and learned

- ✅ Measured **WiFi ≈ +45 mA, BLE ≈ +9 mA** on this board — WiFi is ~5× BLE
- ✅ Learned the cost is **being ON, not connected**; an **AP never modem-sleeps**
- ✅ Turned the numbers into **duty-cycle / prefer-BLE / STA-over-AP** design rules
- ✅ Reused the **hold-a-state, trust-the-delta** method from Stage 5e

## Command cheat-sheet

```bash
# press GPIO 16 -> radio bench; step with GPIO 16, read the inline meter each state
# serial logs each state: ">>> RADIO BENCH: 1 WiFi AP idle"
```

## Glossary

- **Modem-sleep / DTIM** — a WiFi *station* idling its radio between router beacons;
  unavailable to an *access point*, which must stay awake for clients.
- **Advertising interval** — how often BLE broadcasts; longer = lower average power.
- **Duty-cycle** — run a load only a fraction of the time to cut average draw.
- **Common-mode** — a quantity equal in two readings, so it cancels in the difference.

## Where this leaves the project

The board tour (Stages 1–13) plus this power characterisation give a complete picture:
every subsystem brought up, *and* the numbers to budget them against a battery. The
recurring method — **hold a state, subtract a baseline, trust only what's above the
noise floor** — is the most portable thing here, as true for a radio as it was for a
sleeping CPU.
