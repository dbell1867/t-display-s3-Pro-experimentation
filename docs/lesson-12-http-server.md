# Lesson 12 — the capstone: an HTTP server for your photos

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** The finale. The board is a hotspot (Stage 11); now give the
phone something to fetch. An **HTTP server** at `http://192.168.4.1/` lists the SD
card's `/IMG_NNNN.JPG` captures and serves them — so you browse the photos you shot
in Stages 8–9 from your phone. Camera, SD and radio, finally in one request.

---

## Learning objectives

By the end of this lesson you can:

1. Stand up a synchronous **`WebServer`** and route requests.
2. Build an HTML page **in chunks** instead of one big buffer.
3. Serve a file from SD with `streamFile()` and the right **Content-Type**.
4. Explain why the AP **loop** finally stops being a placeholder.

---

## Module 1 — The whole thing, in one screen

Everything the project built now meets in a single HTTP request:

```
phone browser  --HTTP-->  radio (Stage 10/11)
                              |
                          WebServer (this lesson)
                              |
             +----------------+----------------+
          GET /                             GET /IMG_0001.JPG
          list the card                     read the file
             |                                 |
          SD (Stage 6)                      SD (Stage 6)  --> the JPEG
          the camera wrote (Stages 8–9)
```

Two subsystems we brought up **separately** — SD over SPI, the radio — now run in one
request: `streamFile()` reads a JPEG off the card and pushes it over Wi-Fi.

---

## Module 2 — Routing: two handlers

The bundled **`WebServer`** is synchronous — you register handlers, then pump it from
your loop. It's file-scope so the handlers (which take no arguments) can reach it:

```cpp
#include <WebServer.h>       // bundled — no lib_deps
static WebServer wifiServer(80);

wifiServer.on("/", handleIndex);        // the gallery page
wifiServer.onNotFound(handleFile);      // anything else = a file lookup
wifiServer.begin();
```

`on("/")` handles exactly the root; **`onNotFound`** is a catch-all — every other
path (`/IMG_0001.JPG`, `/favicon.ico`, …) lands there, and we decide whether it names
a real file. Two handlers cover the whole site.

---

## Module 3 — Build the page in chunks, not a buffer

The gallery could have any number of photos, so building one giant `String` risks
running the heap out. Instead, **stream it in chunks** with an unknown content
length:

```cpp
wifiServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
wifiServer.send(200, "text/html", "");        // begin a chunked response
wifiServer.sendContent("<!DOCTYPE html>…<h1>Captures</h1>");

File dir = SD.open("/");
while ((e = dir.openNextFile())) {
  String n = baseName(String(e.name()));
  if (n.endsWith(".JPG")) {
    String href = "/" + n;
    wifiServer.sendContent("<a href=\"" + href + "\"><img src=\"" + href + "\"></a>");
  }
}
wifiServer.sendContent("</body></html>");
wifiServer.sendContent("");                   // empty chunk = end of response
```

This is the same instinct as `fb_return` and the chunked frame pushes: **don't hold
the whole thing in memory when you can emit it piece by piece.** Each `<img>` tag
points back at *this* server, so the browser turns around and fetches each JPEG —
which lands in the second handler.

> **`baseName()` guards a portability trap:** `File::name()` returns a bare filename
> on some core versions and a full `/path` on others. Normalising to the basename
> means the links work either way.

---

## Module 4 — Content-Type is the whole trick

Serving the file is four lines:

```cpp
String path = "/" + baseName(wifiServer.uri());
if (path.endsWith(".JPG") && SD.exists(path)) {
  File f = SD.open(path, FILE_READ);
  wifiServer.streamFile(f, "image/jpeg");   // <-- the type is everything
  f.close();
} else {
  wifiServer.send(404, "text/plain", "not found");
}
```

`streamFile()` chunks the file over the socket for us. The one thing *we* supply is
the **`Content-Type`**, and it does all the work:

- `text/html` → the browser **renders** the gallery.
- `image/jpeg` → the browser shows a **photo**.
- get it wrong (say `text/plain`) → the browser downloads gibberish.

**The bytes are identical; the header decides what they mean.** That single line is
the difference between "a photo" and "a file download." Same lesson as the camera's
RGB565 byte order (Stage 8): the data is only as useful as the label you put on it.

---

## Module 5 — The loop finally does real work

Every "mode" before this — viewfinder, scan, AP — was a blocking loop that mostly
*waited*. This one earns its keep:

```cpp
while (true) {
  wifiServer.handleClient();          // service any pending request (non-blocking)
  /* … live client count, tap-to-exit … */
  delay(5);                           // tight: HTTP wants prompt servicing
}
```

`handleClient()` checks for a waiting request and dispatches it to a handler. It must
be called often — hence `delay(5)` instead of the AP screen's old `delay(100)`. And
it cures Stage 11's loose end: a phone drops a *no-internet* Wi-Fi network, but once
there's a **server answering**, the phone has a reason to stay. The web page is what
keeps the connection alive.

On exit we shut it down cleanly, in order: `wifiServer.stop()` (close the socket) →
`WiFi.softAPdisconnect(true)` → `WiFi.mode(WIFI_OFF)`.

---

## Module 6 — Board & build notes

- **Flash 44% → 45%** — `WebServer` is a thin layer on the WiFi/lwIP stack already
  linked; the big cost was `WiFi.h` back in Stage 10.
- **No new bus contention.** SD (SPI) and radio are serviced sequentially in one
  single-threaded loop — no concurrency, so no locking. The panel text and the SD
  reads never overlap.
- **Security is by proximity.** The catch-all serves any `.JPG` that exists on the
  card; it's an isolated, password-gated hotspot that only exists while you hold the
  screen open, so the threat model is "who is in Wi-Fi range right now." Fine here;
  a public-facing server would need path-sanitising and auth.
- The first page load can be a beat slow while the phone commits to the network;
  reload once if it stalls.

---

## What you built and learned

- ✅ An **HTTP server** serving the SD card's captures to a phone — the capstone
- ✅ Routed with `on("/")` + `onNotFound`; two handlers cover the site
- ✅ Built the gallery **in chunks** — no giant buffer, any number of photos
- ✅ Served files with `streamFile()` and learned **Content-Type is the meaning**
- ✅ Saw the AP **loop do real work** (`handleClient`), which keeps the phone connected

## Command cheat-sheet

```bash
pio run -t upload -t monitor         # watch "HTTP GET / -> N images" per request
# phone: join TDisplay-S3-Pro / tdisplay123, open http://192.168.4.1/
```

## Glossary

- **`WebServer`** — bundled synchronous HTTP server; pumped by `handleClient()`.
- **`onNotFound`** — catch-all handler for any unrouted path.
- **Chunked response** — `setContentLength(CONTENT_LENGTH_UNKNOWN)` + `sendContent()`.
- **`streamFile(File, type)`** — send a file's bytes with a Content-Type.
- **Content-Type** — the header that tells the browser what the bytes *are*.

## The project, end to end

Twelve stages from a blank board to a working web app:

**first light → touch → LVGL → battery/PMU → the power ladder (light/deep sleep,
button wake) → auto-brightness → SD → camera detect → live viewfinder → capture to
SD → WiFi scan → hotspot → this server.**

Press GPIO 16 to shoot photos; press GPIO 12 to serve them to your phone. The board
is fully lit. Anything from here — higher-res stills, a full-screen viewfinder,
gauge-logging to CSV, the LVGL flex/grid refactor, BLE — is refinement on a complete
tour.
