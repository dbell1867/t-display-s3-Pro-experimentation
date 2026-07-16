# Lesson 03 — Your First Interactive App (a Tap Counter)

**Project:** Bringing up a LilyGO T-Display S3 Pro in C++
**Goal of this lesson:** Turn "draw where I touch" into a real **event-driven app
with state** — an on-screen counter with **TAP +1** and **RESET** buttons, each
with press feedback — and get the input handling *genuinely* right.

> Continues from [Lesson 02 — Bringing Touch to Life](lesson-02-touch.md). We reuse
> the touch stack and the hard-won lesson that **the touch IRQ is a pulse, not a
> level** (Module 9) — it turns out to be the key to correct buttons.

---

## Learning objectives

By the end of this lesson you can:

1. Bundle related data into a **`struct`** and write functions that work on *any*
   instance of it (the reusable `Button`).
2. Pass structs efficiently with a **`const` reference** (`const Button &b`).
3. Hold **application state** and reflect it on screen (the counter).
4. Do **edge detection** correctly on a flickering input — fire an action **once
   per press**, not once per frame — using a debounced press/release state.
5. Use a hardware **touch-up event** for a crisp release, with a timeout fallback.
6. Point a variable at "one of several things, or nothing" with a **pointer** and
   **`nullptr`** (`activeBtn`).

*No new hardware this lesson* — it's all software on the stack we already built.

---

## The app

```
        Tap Counter

            0            ← big live counter (state)

     ┌───────────────┐
     │    TAP +1     │   ← increments the counter
     └───────────────┘
     ┌───────────────┐
     │     RESET     │   ← zeroes it
     └───────────────┘
```

Tap **TAP +1** → the number goes up by one and the button highlights while held.
Tap **RESET** → back to zero. Taps outside a button do nothing.

---

## Module 1 — A reusable button with `struct`

In Lesson 02 the CLEAR button was hard-coded: four `#define`s and a bespoke
`insideClearButton()`. That doesn't scale to several buttons. So we bundle a
button's data into a **`struct`** — a named group of fields, like a Python
`@dataclass`:

```cpp
struct Button {
  int16_t x, y, w, h;    // rectangle: top-left corner + size
  const char *label;     // text drawn on it
  uint16_t color;        // normal (unpressed) colour
};

Button tapBtn   = { 11, 300, 200, 70, "TAP +1", RGB565_DODGERBLUE };
Button resetBtn = { 11, 390, 200, 70, "RESET",  RGB565_RED        };
```

Now **one** hit-test and **one** draw function serve *every* button:

```cpp
bool inside(const Button &b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w &&
         y >= b.y && y < b.y + b.h;
}
```

### New C++ concepts

- **`struct`** — a value type that groups fields. Access with a dot: `b.x`,
  `b.label`. (A C++ `class` is the same idea with more access control; a `struct`
  is the lightweight, everything-public version — perfect for plain data.)
- **`const Button &b`** — pass the button **by reference** (`&`) so we don't copy
  all its fields on every call, and **`const`** promises we won't modify it. In
  Python everything is already a reference; in C++ you *choose*, and stating
  `const` both documents intent and lets the compiler catch mistakes.
- **`const char *label`** — a pointer to text (a C string). We only ever read it.

---

## Module 2 — State, and drawing only what changed

The counter is **state** — a plain variable the whole UI revolves around:

```cpp
int counter = 0;
```

When it changes we redraw **just the number band**, not the whole screen, so the
buttons don't flicker:

```cpp
void drawCounter() {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", counter);      // int → text
  gfx->fillRect(0, 120, SCREEN_W, 110, RGB565_BLACK);   // clear only this band
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(10);
  int16_t tw = strlen(buf) * 60;                  // size-10 char ≈ 60 px wide
  gfx->setCursor((SCREEN_W - tw) / 2, 140);       // horizontally centred
  gfx->print(buf);
}
```

> **Immediate-mode habit:** because we draw straight to the panel, "update the
> number" means *clear its region, then redraw*. Confining the clear to one band
> keeps the rest of the screen rock-steady. (A framebuffer — Stage 3b — will let
> us redraw whole scenes cleanly instead; for now, targeted redraws.)

---

## Module 3 — Edge detection done right (the heart of the lesson)

A counter must increment **once per tap**. But `loop()` runs ~500×/second and a
touch is held for many frames — so "increment whenever touched" would rocket the
number upward. We need to fire only on the **press edge** (the transition from
not-touching to touching). This turned out to be the whole challenge, and it came
in three acts.

### Act 1 — the naive flag doesn't work (we already knew why)

The obvious trick — "remember last frame's pressed flag, act when it goes from
false→true" — **fails on this hardware**, for the exact reason from Lesson 02
Module 9: `touch.isPressed()` reads the **IRQ line, which only pulses per frame**.
Between frames — even mid-touch — it reads false. So the flag flickers false
constantly and every frame looks like a fresh edge. We can't trust it.

### Act 2 — a debounced press/release state

Instead of trusting the instantaneous flag, we track a **debounced** state and
detect *release* by absence of frames:

```cpp
#define TOUCH_RELEASE_MS 60
uint32_t lastTouchMs = 0;
bool touchActive = false;      // debounced "finger is down"
```

- Every accepted touch frame refreshes `lastTouchMs`.
- If no frame arrives for `TOUCH_RELEASE_MS`, the finger has lifted → clear
  `touchActive`.
- The action fires on the **rising edge** — when we see a touch while
  `touchActive` is still false:

```cpp
if (!touchActive) {            // rising edge: a fresh press
  touchActive = true;
  if (inside(tapBtn, x, y)) { counter++; /* ... */ }
}
```

`TOUCH_RELEASE_MS` must sit **above** the ~25 ms frame interval (so a held finger,
whose frames arrive every ~25 ms, never looks "released") but low enough that fast
tapping still registers. This worked — but fast taps still occasionally slipped
through, because you can't tap again until the 60 ms release window has elapsed.

### Act 3 — use the controller's touch-up event

The crisp fix: most capacitive controllers, the CST226SE included, **pulse the IRQ
once more when you lift**, delivering a frame with **zero points**. That's an
explicit "finger up" — far better than waiting out a timeout. We were ignoring
those frames; now we treat `count == 0` as an immediate release:

```cpp
if (touch.isPressed()) {
  uint8_t count = touch.getPoint(touchX, touchY, 5);
  if (count > 0) {
    lastTouchMs = millis();
    if (!touchActive) { /* rising edge — act once */ }
  } else {
    releaseTouch();      // touch-up event: release NOW, don't wait for timeout
  }
}
```

With this, `touchActive` drops the instant you lift, so the next tap always lands
on a rising edge. **Verified on hardware:** every fast tap counts, and holds stay
solid (the controller doesn't emit stray empties mid-touch — if yours does, gate
the release on two consecutive empties). The timeout stays as a **fallback** for
controllers that don't send an up-event.

> **The pattern:** prefer an explicit hardware event when the chip offers one;
> keep a timeout as a safety net. Two mechanisms, one robust result.

---

## Module 4 — Feedback: highlight while held (and a pointer)

Good buttons show they're being pressed. Rather than a blocking flash, we keep a
button highlighted **for as long as it's held** and restore it on release. To do
that we must remember *which* button is currently down — "one of these, or none":

```cpp
Button *activeBtn = nullptr;    // the held button, or nullptr for none
```

On a fresh press inside a button we point `activeBtn` at it and draw it pressed;
on release we redraw it normal and clear the pointer:

```cpp
drawButton(tapBtn, true);  activeBtn = &tapBtn;   // &tapBtn = "the address of tapBtn"
// ... later, on release ...
void releaseTouch() {
  if (!touchActive) return;
  touchActive = false;
  if (activeBtn) { drawButton(*activeBtn, false); activeBtn = nullptr; }
}
```

### New C++ concepts

- **Pointer to a chosen object.** `Button *activeBtn` can point at `tapBtn`,
  `resetBtn`, or **`nullptr`** ("nothing"). `&tapBtn` takes its address;
  `*activeBtn` follows the pointer back to the button. (This is the same `*`/`&`
  machinery as the display `gfx` pointer — here we use it to hold a *changeable*
  reference, something a plain reference can't do.)
- **`nullptr` as "none".** Checking `if (activeBtn)` asks "is a button held?" — a
  clean way to represent absence, like Python's `None`.

---

## What you built and learned

- ✅ A reusable **`Button` struct** + generic `inside()` / `drawButton()`
- ✅ Passed structs by **`const` reference**; read a C-string label
- ✅ Held **state** (`counter`) and redrew only the changed region
- ✅ **Edge detection** that fires once per tap despite a flickering IRQ
- ✅ Used the controller's **touch-up event** for crisp release; timeout fallback
- ✅ **Pressed-while-held** feedback via a `Button *` / `nullptr`
- ✅ A complete, satisfying interactive app on the stack from Lessons 01–02

## Command cheat-sheet

```bash
pio run                 # build only
pio run -t upload       # build + flash
pio device monitor      # serial console @ 115200 (Ctrl-C to quit)
```

## Glossary

- **`struct`** — a named bundle of data fields (like a Python dataclass).
- **Reference (`&`)** — an alias for an existing object; `const &` passes it without copying and forbids modification.
- **Pointer / `nullptr`** — a variable holding an object's address (or "nothing"); `&x` takes an address, `*p` follows one.
- **State** — data the program remembers between events (the counter).
- **Edge detection** — reacting to a *change* (press edge) rather than the ongoing state (finger held).
- **Debounce** — smoothing a noisy/flickering input into a stable on/off state.
- **Touch-up event** — a zero-point frame the controller sends when a finger lifts; an explicit "release" signal.

## Next lesson

Two good directions from here (see `docs/PLAN.md`):

- **Stage 3b — Retained-mode framebuffer** (`Arduino_Canvas` in PSRAM): redraw
  whole scenes without flicker, and unlock the fading/expiring-dot trail we
  deferred in Lesson 02.
- **Stage 4 — LVGL:** trade hand-drawn buttons for a real widget toolkit; our
  `Button`/hit-test/edge work is exactly what LVGL automates.
