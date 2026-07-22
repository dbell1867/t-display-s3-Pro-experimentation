# Lesson 11 — WiFi connect: the board becomes an access point

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Move from *proving the radio* (Stage 10's scan) to a real
connection — but instead of joining a network, the board **becomes** one. In **AP
mode** the ESP32 is its own Wi-Fi hotspot; a phone joins it directly. We show the
SSID/password/IP on screen and a live client count, and watch it go **0 → 1** when a
phone associates. No server yet — that's Stage 12.

---

## Learning objectives

By the end of this lesson you can:

1. Explain **AP mode vs STA mode**, and when each is the right choice.
2. Bring up a `softAP` and read its **IP** and **client count**.
3. Understand why an AP's credentials are **not a secret** (so hard-coding is fine).
4. Anticipate the **"no internet"** phone behaviour and why it's expected.

---

## Module 1 — AP vs STA: two opposite roles

Wi-Fi has two roles, and they're mirror images:

| | **STA (station)** | **AP (access point)** |
|---|---|---|
| Who joins whom | board joins an existing router | phone joins the **board** |
| Needs credentials | yes — the router's password | no — you *set* the board's |
| Needs a router | yes | **no** |
| Internet | yes (via the router) | no (it's just a local link) |
| IP | assigned by the router (DHCP) | **fixed & known** (192.168.4.1) |

For a camera whose job is to hand photos to a phone, **AP mode wins**: no router to
depend on (works in a field), nothing to store from your home network, and a
**predictable IP** so Stage 12's URL is known in advance. STA would let the board
reach the internet, but that's not what this device needs.

---

## Module 2 — Bringing up the hotspot

```cpp
WiFi.mode(WIFI_AP);
bool ok = WiFi.softAP(AP_SSID, AP_PASS);   // WPA2 needs a password >= 8 chars
IPAddress ip = WiFi.softAPIP();            // 192.168.4.1 by default
```

That's the whole connection. `softAP()` starts the radio as an access point, brings
up a DHCP server, and starts beaconing the SSID — a phone can see it immediately.
`softAPIP()` returns the fixed gateway address the phone will talk to.

```cpp
WiFi.softAPdisconnect(true);   // on exit: stop the AP AND power the radio down
WiFi.mode(WIFI_OFF);
```

The `true` argument powers the radio off, not just the AP — the same
protect-the-power-budget discipline as Stage 10 (an idle radio is tens of mA).

---

## Module 3 — The credentials are public *by design*

Back in Stage 10 I flagged that a home Wi-Fi password shouldn't be hard-coded — it'd
end up in git. AP mode **inverts** that:

```cpp
static const char *AP_SSID = "TDisplay-S3-Pro";
static const char *AP_PASS = "tdisplay123";   // shown on screen — not a secret
```

An access point's own SSID and password are things you **hand out** — we literally
print them on the display so you can type them into a phone. So hard-coding them is
correct, not a leak. The security question isn't "who can see the password" (everyone
can) but "who's in Wi-Fi range of a hotspot that only exists while you hold the
button." That's a very different threat model from your home network.

> **The same act — hard-coding a Wi-Fi credential — is wrong in STA mode and right in
> AP mode.** Context decides. A password you *display* isn't a secret.

---

## Module 4 — Proving association: the client count

This stage proves *connection*, not *serving*. The proof is the live client count:

```cpp
int clients = WiFi.softAPgetStationNum();   // devices currently associated
```

Poll it, redraw only when it changes, and watch it go **0 → 1** the moment a phone
joins. Green when someone's on. Confirmed on hardware: joining from a phone flipped
it to 1.

Two behaviours to expect, both normal for a no-internet AP:

- **The phone warns "no internet" and asks whether to stay connected.** Correct —
  this AP has no upstream router, so there's genuinely no internet. Choose stay.
- **Some phones silently drop off** a network with no internet after a while, so the
  count may bounce 1 → 0. That's the phone being clever, not a bug. In Stage 12 the
  web page gives the phone a *reason* to stay.

---

## Module 5 — Board & build notes

- **Flash didn't grow** from Stage 10 — the WiFi stack was already linked by the scan;
  AP mode is the same library, different call. The ~600 KB was a one-time cost.
- **Fixed IP 192.168.4.1** is the ESP32 softAP default; Stage 12's server binds there.
- Still a **blocking modal** (like the viewfinder and scan): the gauge is frozen while
  the AP screen is up. Stage 12's HTTP server will live in this same loop — that's
  where the loop stops being a placeholder and starts doing work.
- Triggered by **GPIO 12**, which has now carried the whole WiFi feature from scan to
  hotspot — the button we'd wrongly written off two stages ago.

---

## What you built and learned

- ✅ Brought the board up as a **Wi-Fi access point** a phone can join
- ✅ Read the AP's **IP** and **live client count**; saw association 0 → 1
- ✅ Learned **AP vs STA**, and why AP suits a photo-handing device
- ✅ Learned that AP credentials are **public by design** — hard-coding is correct
- ✅ Recognised the **"no internet" / auto-drop** phone behaviour as expected

## Command cheat-sheet

```bash
pio run -t upload -t monitor     # press GPIO 12; serial prints "AP clients: N"
# on the phone: Wi-Fi settings -> join "TDisplay-S3-Pro" / "tdisplay123"
```

## Glossary

- **AP (softAP)** — the board acts as a Wi-Fi access point others join.
- **STA** — the board joins someone else's access point.
- **`softAPIP()`** — the AP's own address (default 192.168.4.1); the phone's gateway.
- **`softAPgetStationNum()`** — count of currently-associated client devices.

## Next lesson

The phone can *reach* the board; now give it something to fetch. **Stage 12 — an
HTTP server** at `http://192.168.4.1/` that lists the SD card's `/IMG_NNNN.JPG`
captures and serves them — camera, SD, and radio finally in one loop, and the reason
the phone will stay connected.
