# Lesson 13 — Bluetooth LE: the other half of the radio

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Exercise the radio's other half — **Bluetooth Low Energy**.
The board advertises as a BLE device and exposes the **standard Battery Service**,
fed by the SY6970 gauge from Stage 5a, so a phone can connect and **subscribe** to
live battery updates. Confirmed on hardware: the device screen read *Connected +
Notifying* and the level streamed to nRF Connect.

---

## Learning objectives

By the end of this lesson you can:

1. Explain how **BLE differs from a socket** — services, characteristics, GATT.
2. Expose a **standard service** (Battery, `0x180F`) so generic apps recognise it.
3. Push data with **notifications** (`notify()` + the CCCD), not polling.
4. **Diagnose a subscription problem from the device side** instead of guessing.
5. Put **two actions on one button** by press duration.

---

## Module 0 — BLE is not "Bluetooth serial"

First, a hardware fact: the **ESP32-S3 is Bluetooth Low Energy only** — no Classic
Bluetooth, so **no SPP** (the "serial over Bluetooth" some ESP32 tutorials use). BLE
is a different model entirely, and it's worth understanding before writing a line:

- A device advertises, then a client **connects**.
- The device exposes **services** — named containers (`0x180F` = Battery).
- Each service holds **characteristics** — the actual data slots (`0x2A19` = Battery
  Level), each with **properties** (READ, NOTIFY, WRITE…).
- The client **reads**, **writes**, or **subscribes** to a characteristic.

There's no stream of bytes; there are labelled values you read or get pushed. That's
GATT (the Generic Attribute profile).

---

## Module 1 — A standard service beats a custom one

You *can* invent your own 128-bit UUIDs, but for anything standard, **use the
SIG-assigned UUIDs** — then every generic BLE app understands you for free:

```cpp
BLEDevice::init("TDisplay-S3-Pro");
BLEServer *srv = BLEDevice::createServer();
BLEService *svc = srv->createService(BLEUUID((uint16_t)0x180F));   // Battery Service
BLECharacteristic *chr = svc->createCharacteristic(
    BLEUUID((uint16_t)0x2A19),                                     // Battery Level
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
```

Because `0x180F`/`0x2A19` are the standard Battery Service and Level, nRF Connect
**labels them "Battery Level" automatically** and the phone even knows the value is a
percentage — no custom app, no documentation. Speaking a standard is cheaper than
inventing one.

Advertising is the BLE "first light" — the moment `startAdvertising()` runs, the
device is discoverable:

```cpp
BLEAdvertising *adv = BLEDevice::getAdvertising();
adv->addServiceUUID(BLEUUID((uint16_t)0x180F));
BLEDevice::startAdvertising();
```

---

## Module 2 — Notifications: push, not poll

A client could **read** the level whenever it likes, but the efficient BLE idiom is
**notify**: the device pushes a new value to a subscribed client. Two pieces:

```cpp
BLE2902 *cccd = new BLE2902();   // the CCCD descriptor
chr->addDescriptor(cccd);        // this is WHERE the client subscribes
```

The **CCCD** (Client Characteristic Configuration Descriptor, `0x2902`) is the switch
the *client* flips to say "start sending me updates." Without it, a NOTIFY
characteristic has nowhere to record the subscription. Then, once a second:

```cpp
chr->setValue(&level, 1);
if (bleConnected) chr->notify();   // sends only to clients that subscribed
```

`notify()` is a no-op for clients that haven't subscribed — which is exactly what
made this stage's bug so confusing.

---

## Module 3 — The bug: diagnose from the device, not the app

**Symptom:** the device showed 74% but the app showed 80%, and updates weren't
arriving. The instinct is to poke at the phone. Better: **make the device report what
it knows.**

The 74-vs-80 gap was itself the clue — a *stale one-time Read* (80%, from earlier)
against a device that had drifted to 74%. If notifications were flowing they'd match
within a second. So the subscription almost certainly wasn't active — but "almost
certainly" isn't a diagnosis.

The device can **query its own CCCD**:

```cpp
bool sub = cccd->getNotifications();   // did the client actually subscribe?
```

Surfacing that on screen turned a guessing game into a fact:

| Screen | Meaning |
|---|---|
| `Advertising...` | nobody connected |
| `Connected - enable Notify` | connected, **not** subscribed ← the bug, made visible |
| `Connected + Notifying` | subscription live, updates flowing |

The moment the app's subscribe control was found, the line flipped to green. The fix
was in the app (find the triple-down-arrow **notify** icon on the characteristic), but
the *diagnosis* came from the firmware.

> **When a two-sided interaction misbehaves, instrument the side you control.** We
> couldn't see inside nRF Connect, but the ESP32 knew whether its CCCD was enabled —
> so we asked it, instead of debugging a black box by trial and error. Same lesson as
> the boot-log and power-bench stages: make the invisible state visible.

---

## Module 4 — One button, two radios

We were out of buttons, so **GPIO 12 does both radios by press duration** — the same
tap/hold trick as the viewfinder's save/exit (Stage 9):

```cpp
if (g12Prev && !g12)                              g12Down = millis();     // press
else if (!g12 && millis()-g12Down >= 700)      { runBleService(); … }    // HOLD -> BLE
else if (!g12Prev && g12 && held < 700)        { runWifiAP();     … }    // TAP  -> WiFi
```

**Tap for WiFi, hold for Bluetooth.** The hold fires the instant it crosses 700 ms
(no need to release); a tap is decided on release. One physical input, two whole
subsystems — the button we'd wrongly written off three stages ago now drives the
entire radio.

---

## Module 5 — Board & build notes

- **BLE is heavy:** adding the Bluedroid stack took flash **45% → 53%**. WiFi *and*
  BLE are now both linked (~1.76 MB); still comfortable in the 3.3 MB app partition,
  but the two radios together are the biggest single cost in the project.
- **They don't run at once.** WiFi and BLE are separate modals; each `deinit`s on
  exit (`BLEDevice::deinit(true)` frees the controller), so only one radio stack is
  live at a time. Coexistence *is* possible on the ESP32 but we don't need it.
- **On reconnect, restart advertising.** BLE stops advertising while a client is
  connected; the disconnect callback calls `startAdvertising()` again so the next
  phone can find the board.
- **The value only visibly moves on battery.** On USB the % is stable, so a working
  subscription shows *fresh timestamps* rather than a changing number — watch the log,
  not just the digits.

---

## What you built and learned

- ✅ Advertised over **BLE** and exposed the **standard Battery Service**
- ✅ Pushed live updates with **notifications** + the **CCCD**, not polling
- ✅ Learned **BLE ≠ Bluetooth Classic** (no SPP on the S3) — services/characteristics
- ✅ **Diagnosed a subscription bug from the device** by reading its own CCCD state
- ✅ Split **one button across two radios** by press duration

## Command cheat-sheet

```bash
pio run -t upload -t monitor     # hold GPIO 12; serial: "BLE notify 74% (subscribed=1)"
# phone: nRF Connect -> scan -> TDisplay-S3-Pro -> Connect ->
#        Battery Level -> tap the triple-down-arrow (Notify)
```

## Glossary

- **GATT** — the attribute model: services → characteristics → descriptors.
- **Characteristic** — a data slot with properties (READ / NOTIFY / WRITE).
- **CCCD (`0x2902`, BLE2902)** — the descriptor a client writes to subscribe.
- **`notify()`** — push a value to subscribed clients (no-op if none subscribed).
- **Standard UUID** — SIG-assigned (`0x180F`, `0x2A19`), recognised by generic apps.

## Where this leaves the project

Both halves of the radio are now exercised: **WiFi** (scan → hotspot → HTTP photo
server) and **Bluetooth LE** (Battery Service). Every subsystem of the board has been
brought up, from first light to two working wireless apps. Anything further —
serving live frames over HTTP, a custom BLE service, higher-res stills, the LVGL
flex/grid refactor — is refinement on a complete tour.
