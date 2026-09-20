# Status

_Last updated: 2026-09-19, after the ratio-and-chord-field revision._

## Where the project stands

The application is **built, packaged and verified**. `./build.sh package` produces
`dist/ScreenMagnifier.exe` (1.3 MB, icon and manifest embedded, no runtime DLLs) and
`dist/ScreenMagnifier-1.0.3.zip` (0.5 MB, the executable plus a Chinese readme) — the second is what
gets sent to someone.

```
package:    ./build.sh package      -> dist/ScreenMagnifier.exe + .zip
build:      ./build.sh release
tests:      ./build.sh test
            python tests/verify_package.py
            python tests/verify_features.py
            python tests/verify_picker.py
            python tests/verify_runtime.py
            python tests/verify_visual.py
            python tests/measure_perf.py
            python tests/verify_stress.py
            python tests/profile_cpu.py
```

### Green, as of this revision

| Suite | Result |
|---|---|
| `build.sh test` | **3476 / 3476** core checks pass (245 the viewport, 155 the ratio lock, 406 the hotkey text) |
| `verify_features.py` | Chinese default, centred start, 10× cap, live slider, typed factor, restore defaults, survives off/on showing what it showed before, landscape + resizable + reflows when made taller, centre-the-window hotkey, size slider resizes in place, fit-to-source restores the whole region, **the ratio lock holds on a typed size and only while it is on**, editable presets, kept regions recalled intact, the language switch leaves nothing of the old one behind, clean exit |
| `verify_picker.py` | move-by-drag, on-screen confirm button, double-click confirm, Enter confirm |
| `verify_runtime.py` | **22 / 22** acceptance checks pass, including a chord another application owns, a chord the program itself owns being reassigned, **a second chord field clicked straight after another**, a bare key being refused with a word rather than in silence, and a mouse button being bound and fired |
| `verify_visual.py` | all four shapes correct; rectangle pixel-exact at 4× (mean abs diff **0.00**); resizing the window keeps the configured factor; every shape survives the window being shrunk to 406×305 |
| `measure_perf.py` | idle 37.3 MB / 0.00 % CPU; running **1.56 %** CPU; released to 2.7 MB when switched off |
| `verify_stress.py` | 15 show/hide cycles, frames climbing at 56 fps; `WM_DISPLAYCHANGE` acknowledged |
| `verify_package.py` | the shipped **.zip** unpacked into a folder named with Chinese characters and a space: starts, magnifies, quits 0, and every DLL it imports ships with Windows |
| `tools/check_resources.py` | 7 icons, group icon 1, manifest 1; the shell reports the icon |

## The ratio-and-chord-field revision (2026-09-19)

Two more faults from use, and one report that could not be reproduced at all — recorded here with
what was checked, because "works here" is only useful alongside the list of attempts.

### "Keep aspect" only locked the size slider

`apply_size_slider()` derived one axis from the other; nothing else did. Typing a width and pressing
Apply resized to exactly that width and left the height alone, and so did the arrow hotkeys and an
edge drag on the window — so the checkbox meant "keep the ratio when you use *this* control", which
is not what it says.

The lock now lives in `MagnificationController::with_ratio_lock()`, which **every** resize path goes
through: the typed size and Apply, `step_output_size()` (the arrow hotkeys), the size slider, and the
edge drag on the window itself. The axis the caller moved decides which one is authoritative; when
both moved — a corner drag, or two typed numbers — the width wins, since it is the axis people read
first. Two details worth keeping:

* **The derived side is clamped into the desktop, the moved side is not.** A drag that reaches the
  edge of the screen should stop growing, not make the whole resize silently do nothing; but a width
  that is genuinely out of range still has to throw, which is `resize_output()`'s documented
  contract. So the derived side is clamped, and if that clamp bites, the moved side follows it —
  unless the moved side was out of range itself, in which case it is left alone for the range check
  to refuse.
* **`fit_output_to_selection()` deliberately does not lock.** It has to be able to set the region at
  the factor exactly, or a region whose proportions differ from the window's could never be shown
  whole. The ratio is the *window's*, not the selection's.

**Checked** by 155 new core checks (one axis moves and the other follows; a both-axes change keeps the
width; a step and its undo land back where they started rather than drifting a pixel at a time; the
ceiling clamps instead of going lopsided; with the lock off one axis moves alone) and by a new step in
`verify_features.py` [11], which types 800 into 宽, presses Apply, requires the window to become
800×600 of the 960×720 it started at, then unticks the box and requires the same edit to move the
width alone.

### A second chord field clicked straight after another was dead

`begin_chord_capture()` armed the field and *then* called `SetFocus()`. SetFocus delivers
`WM_KILLFOCUS` **synchronously** to the field being left behind, and its handler cancels the capture
— which resets `chord_capture_index_` to `-1`. The newly clicked field therefore showed
`请按新的组合键…` while nothing was armed, and every key after it was ignored. Changing one hotkey
and then clicking the next field made the second one dead, which reads as the program refusing the
key. Reproduced directly: with the old code, `ZoomIn` stayed at its shipped `Ctrl+Alt+OemPlus`
through an attempt to set `Ctrl+Alt+B`, and two more fields did the same.

The arming moved after the focus move, and `WM_KILLFOCUS` now only cancels when the field losing
focus is the one that is actually armed (`is_armed_field()`), so the two cannot interfere whatever
order the messages arrive in. `InputThread`'s suspend/resume also became idempotent: re-arming while
already suspended used to overwrite the "was the hook up" flag with `false`, which would have lost
the fallback hook for good the next time it resumed.

**Checked** by a new acceptance check: click the Taller field, click the Wider field straight after
it without touching the window in between, press `Ctrl+Alt+N`, and require `GrowWidth` to become
`[3, 78]`. It fails on the previous build, where the field keeps its shipped chord.

**And one chord cannot be recorded at all**: pressing `Win` opens the Start menu and takes the
focus, so the capture ends before the rest of the chord arrives. That is the shell's doing, not the
field's, and it is now documented in both readmes rather than left as a mystery. `Ctrl`, `Alt` and
`Shift` chords all record.

### "Save region does nothing" — not reproduced, and what was tried

Every mechanical path works, and each was checked against the real executable:

* framing a region with the picker, clicking 保存选区, framing another, clicking the numbered button
  and getting the first one back — bounds, shape and corner radius all restored;
* the write target following the lit button, and the lit button following the save;
* the saved slot reaching `config.json` and surviving the reload;
* every control being the window under its own centre at both the default size and the window's
  minimum, so nothing is covered or clipped;
* a real mouse click (not `BM_CLICK`) on the button, which is how a user would press it.

What could still read as "it does nothing" was the gesture itself: **the write target is the lit
button and it does not move when the region does**, so saving twice in a row overwrites the same slot
and produces no visible change at all. Asked directly, the report was "completely no reaction, no
visible change", with the magnifier off — which is exactly that case, since the only thing a save
used to change on screen was a light that was already on. The gesture now confirms itself:

* the button says `已保存 ✓` for a second and a half where the click landed, and
* the status line names the slot it went to (`已保存到选区 2`).

Both readmes explain that a different slot means clicking its number first. If the report meant
something else, the remaining suspects are a magnifier window covering the settings window (it is
topmost, and a click that lands on it drags it instead of pressing the button) and a saved region
that is no longer on the desktop after a display change.

## The shape-and-mouse revision (2026-09-19)

Two more faults reported from use, both found by running the thing rather than by reading it.

### Shrinking the window turned a circle back into a rectangle

The mask was the *selection's* extent, grown to the window when the window was the larger of the
two. That reads well — the window can show the desktop outside the region and still be shaped by the
region — but it fails in the other direction: make the window smaller than the shape and the mask
stays the shape's size while the window becomes a crop of its middle. A circle's middle is all
circle, so the window filled edge to edge and the shape was simply gone. Measured with the probe
below: a circle kept its shape down to 640px and was a plain rectangle at 400px.

The mask is now **the window itself**. The shape is the aperture the magnifier looks through, so it
shrinks with the window instead of being cropped by it; the selection still decides where the view
is centred, how big the window starts, and which shape and corner radius are drawn. At the natural
size — a window of exactly `region × factor` — the two definitions agree, which is why the four
original shape checks are unchanged.

**Checked** by extending `verify_visual.py`: after the natural-size measurement each shape drags the
size slider down to 406×305 and requires the corners to behave the same way they did at full size —
masked away for the circle, ellipse and rounded rectangle, showing content for the rectangle. On the
previous build the three shaped cases report content at the corners and fail.

### A mouse button could not be bound at all

`RegisterHotKey` is the registered path for every chord, and it has no mouse chords. Worse, it does
not say so: **it accepts a mouse virtual key and returns success**, and the system then never
delivers one, so asking would have produced a chord that looks registered and does nothing — the one
failure this whole subsystem exists to prevent. Mouse buttons now go straight past `RegisterHotKey`
into the fallback table, which is what the low-level hook is for, and a `WH_MOUSE_LL` hook joins the
keyboard one to serve them.

Details worth keeping:

* **The mouse hook is installed only while a mouse-button binding exists.** A `WH_MOUSE_LL` hook
  costs every mouse event in the session — moves included, hundreds a second — a trip to the
  program's input thread, and a keyboard chord another application owns does not need it. `apply_low_level()`
  asks `fallback_has_mouse_chord()` before installing it.
* **A bound mouse button is swallowed**, the same rule the keyboard chords follow: it stops reaching
  the window under the cursor. Binding the middle button stops middle-drag everywhere. That is
  documented in both readmes rather than softened, because the alternative — firing the action *and*
  letting the button through — silently changes what the button does in every other program.
* **A bare mouse button is allowed**, where a bare letter is not: nothing else uses a side button,
  and "Ctrl + back button" is not a gesture anyone reaches for.
* **The left button is deliberately not bindable.** Clicking the field with it is what starts the
  capture, so binding it would make the arming gesture end the capture instead of beginning it.
* `HotkeyChord` needed no new fields: `VK_XBUTTON1` and friends are virtual-key codes, so the model
  already had room for them. What it needed was names — `Mouse4`, `Mouse5`, `MouseMiddle`,
  `MouseRight`, plus `MouseBack`/`XButton1`-style aliases — so the field can show them and a
  hand-edited file can spell them.

**Checked** by two new pieces: 406 core checks that every shipped chord round-trips through its own
text and that the mouse names parse, describe and survive the configuration file; and a runtime
check that clicks a chord field, presses the back button, requires the app to store `[0, 5]` and to
say the binding is hook-served, then presses the button for real and requires the magnification to
change (1280×960 → 1600×1200). The status line's notice for this is its own string — a mouse chord
is not "owned by another application", and reporting it as a conflict would have been a lie.

## The viewport-and-rebind revision (2026-09-19)

Three faults reported from actual use. None of them was visible in the code that looked
responsible, and one of them was a design decision rather than a slip.

### 1. Resizing the window changed the magnification

The renderer scaled the source to *fit* the window, so the window size and the factor were two ways
of saying the same thing and the factor box was only ever right when the window happened to be
exactly `region × factor`. On the shipped defaults — a 320×240 region claiming 4× in a 640×480
window — the picture was really being drawn at 2× from the first frame, and every drag of an edge
or of the size slider moved the true scale while the read-out went on showing 4.00×.

`compute_viewport()` now derives the scale from the factor and nothing else, and the window becomes
what the README had been claiming it was: a viewport. It shows `window ÷ factor` worth of desktop
centred on the region, so a resize changes how much is in view and never how big it is. The shape
mask grows with the window to match, which is what lets a rectangle region show the desktop beside
it; a window *smaller* than the region at that factor crops instead, and because the mask keeps the
region's own size the circle stays a circle.

Two things fell out of it:

* **The shipped default window is now 1280×960**, which is the shipped region at the shipped
  factor. A shipped window that shows only part of the shipped region is a worse first run than one
  that is merely large. An existing config keeps its size, which now means "the viewport is this
  big" — `适配选区` / *Fit to source* puts it back to the whole region.
* **`keep_aspect_ratio` no longer selects a fit policy** (contain/cover cannot both exist when the
  scale is fixed). It still locks the window's proportions while the size slider is dragged, which
  is what `apply_size_slider()` and the edge-drag path already used it for. The README section that
  documented contain and cover is gone with it.

**Checked** by a new case in `verify_visual.py`: park a known pattern, drag the size slider, and
require the window's pixels to still be that pattern at exactly **4×** while the visible source
grows from 320×240 to 364×273. The same comparison at the scale a *fit* would have produced scores
48.6 against 4.2, so the check discriminates rather than agreeing with whatever is on screen. The
245 new core checks cover the viewport directly: the natural size is the region, a bigger window
shows more desktop centred on it, a smaller one crops it, and the scale is the factor for every
combination of five factors and five window sizes.

### 2. Switching language drew the new labels on top of the old ones

Look at a screenshot of the English interface before this: the checkboxes read "Exclude from
capture" *and* `捕获时排除自身`, in the same row, in the same pixels.

Every owner-drawn control composes into a bitmap and blits it whole, and a bitmap that starts
transparent leaves whatever was underneath it alone. The segmented buttons fill their whole
rectangle so they were fine; a **checkbox paints only a box and some text**, so the rest of its
rectangle stayed as it was — and nothing had repainted it, because an owner-drawn button never
erases its own background. The slider had already been given the fix (`fill the card colour
first`, with a comment saying why); the buttons had not.

`paint_button()` now fills with the colour of the surface it is actually sitting on, worked out by
the parent — flat white inside a card or the header, the page's own gradient everywhere else — so
the anti-aliased edges of a rounded button blend into the right backdrop too.

Two smaller ones in the same family were found while fixing it, both visible in the same
screenshot: the **start/stop button** never retranslated (it is labelled from the interaction state,
and `sync()` only rewrites it when the state changes), and the **status line's notices** were stored
as text captured when the event happened, so a hotkey conflict reported at startup stayed Chinese
after switching to English. The notice is now a string *id* plus its detail and is composed when the
line is drawn.

**Checked** by `verify_features.py` **[14]**: click English, then compare the checkbox rows against a
*fresh paint* of the same controls — relaunch the app, which comes up in the language just chosen —
and require them to be identical (0.00 per channel, where leftover glyphs are tens of units). A
screenshot cannot tell "correct" from "correct with leftovers" on its own; a second, independent
paint can. The same check requires no label to still be Chinese and no control to be pushed out of
the client area by the re-laid-out window. `screenshot_lang.py`'s tolerance dropped from three
Chinese labels to one — the `中文` toggle, whose label *is* the language it selects.

### 3. Some keys could not be set, and some did nothing

Two faults, stacked, and the first made the second hard to see.

**Pressing a modifier committed a chord of its own.** The Windows message for Ctrl arrives with Ctrl
already down, so `handle_chord_key()` — which had no idea that a modifier is not a key — built
`Ctrl + VK_11` from it and ended the capture there. The letter the user was actually reaching for
was never seen. Reproduced with a probe: `Ctrl+Alt+J` stored `[2, 17]`, Ctrl and the VK_CONTROL key,
and the field showed `Ctrl+VK_11`. Modifier keys are now ignored as keys, so the capture waits for
the key the chord ends on.

**A chord the program already owned could not be typed at all.** `RegisterHotKey` consumes the
combinations the program holds, so pressing `Ctrl+Alt+M` into a field fired Show/hide and the field
received nothing — and the chords most likely to be tried while reassigning are the ones already in
the table. `InputThread::set_registration_suspended()` now releases every chord *and the low-level
hook* while a field is armed, and puts both back afterwards; the suspend/resume shares the message
handshake the existing re-register and fallback paths use.

**A refused key now says so.** A bare letter is still refused — binding it would swallow that key in
every program — but the field used to ignore the press in silence, which is indistinguishable from
being broken. It now shows `需要配合 Ctrl / Alt / Shift / Win` and stays armed for the next attempt.
The function keys are still allowed on their own.

**Checked** by two new checks in `verify_runtime.py`. The first types `Ctrl+Alt+M` into the field for
"Narrower" and requires: the app owns the chord beforehand, does **not** own it while the field is
armed, stores `[3, 77]` afterwards (Ctrl+Alt+M, not Ctrl+VK_11), holds it again afterwards, and the
magnifier stayed hidden throughout — the old action must not fire while its chord is being given
away. The second presses a bare `J` and requires the field's text to change rather than stay silent.
Both fail on the previous build: the first on `released`, `rebound` and `still_hidden`.

### A harness trap this uncovered

`SetForegroundWindow` from a process that is not itself in the foreground is **refused silently**,
and attaching thread input only half fixes it. Every injected keystroke then lands in whatever window
does hold the foreground, which looks exactly like an application that ignores input: an earlier
version of the probe reported "every second rebind fails" and it was the probe. The checks that type
now click the window's title bar first — a real click is always granted — and `verify_features.py`'s
[8] and [12], which used to SKIP with "the settings window would not come to the front" whenever the
machine was busy, no longer do.

## The kept-regions revision (2026-09-19)

### Four regions you can keep, and four presets you can set

Two things the app made you redo by hand: dragging out a region you had already framed, and living
with the four shipped preset factors.

* **`AppConfig` gained `selection_slots` and `selection_slot`.** Four regions, each the whole
  `SelectionConfig` -- bounds, shape and corner radius -- so recalling one brings back the shape it
  was framed with. The shape card grew a row: four numbered buttons to recall, and a save button to
  write the region in force into whichever button is lit. The lit one is the write target and is
  remembered across restarts; all four start as the shipped region, so recalling one always goes
  somewhere rather than appearing to do nothing.
* **The preset fields are editable.** The four factors were a constant; they are now a field under
  each preset button, sharing the factor box's parser and its 500 ms debounce, so the button, the
  field and the `Ctrl+Alt+1..4` chord cannot disagree -- all three read the same array.
  `parse_factor_text` was pulled out of `commit_factor_edit` for the two of them to share.

Two bugs the new checks found, both of which the eye would have missed:

* **`restore_defaults()` seeded the slots with a region in the corner.** It resets the config and
  then re-centres *the selection*, and `validate()` can only fill an empty slot from the rectangle
  it is handed -- which at that moment is the shipped 0,0 320x240. Check [13] recalls a slot and
  found it at (0,0).
* **The preset button kept its old label.** `commit_preset_edit` writes the new value into the
  window's own copy of the config before firing the callback, so the sync that follows compares
  equal values, sees no change and skips the refresh. It now refreshes the labels itself.

## The hotkey-fallback revision (2026-09-19)

### A hotkey another application owns now works

This was gap 1, and the one most likely to make someone conclude the program is broken: press the
chord, nothing happens, and the only trace is a line in the status bar. `RegisterHotKey` fails when
the combination is already taken, and by far the most common owners of `Ctrl+Alt+M` on a Chinese
desktop are QQ, WeChat, an input method or a driver utility.

Everything needed was already there and switched off. `register_all()` had been building the
fallback table correctly all along -- **only the chords that failed**, with a comment explaining that
a chord both paths handled would publish twice and swallow a key the shell had already delivered --
and `set_low_level_fallback()` had been implementing the hook install. What was missing was the one
line that turns it on: a search of the whole tree found `set_low_level_fallback` called exactly once,
with `false`. The header comment ("only installed when the user turns strict mode off") described an
intent the code never had, and has been corrected.

All four places that register chords -- startup, a rebind, the strict-mode toggle and restore-defaults
-- now go through one `reregister_hotkeys()`, which settles the fallback and the notice together.
Turning strict mode on used to leave the input thread's own flag stale, so a later rebind could have
re-installed the hook; `InputThread::set_strict_compat()` fixes that, and it also takes the hook down
rather than only refusing to put it up.

Verified end to end, which is possible because **`SendInput` does reach a `WH_KEYBOARD_LL` hook even
though it does not trigger `RegisterHotKey` here** -- the opposite of the trap that shapes the rest
of the harnesses. `verify_runtime.py` now takes `Ctrl+Alt+M` for itself before launching the app,
types it, and requires the magnifier window to appear; with strict compatibility mode on it requires
the opposite.

## The centring revision (2026-09-19)

### Resizing no longer walks the window across the screen

`resize_output()`, `step_output_size()` and `fit_output_to_selection()` all changed the size and
left the top-left corner alone, so every resize dragged the window across the desktop by half the
growth — and zooming did the same. All three now hold the **centre**: the new top-left is derived
from the old rectangle, so the window grows and shrinks in place. Where the clamp then pushes it
back inside the desktop is the one case that cannot hold, and the test says so rather than failing.

### A hotkey and a drag bar for the window

* **Ctrl+Alt+C** puts the magnifier window back in the middle of the primary screen, the same
  gesture `Ctrl+Alt+R` has always done for the selection. The selection had a reset; the window,
  which is the thing that actually wanders, did not.
* The 放大窗口 card gained a **size slider**, between the width and height boxes and the buttons
  that commit them, so all three ways of setting the size sit together. It drives the output width
  directly and scales the height with it while the ratio is locked, which means dragging it changes
  how much screen the window covers without changing how much of the selection is in view.

Adding the action moved `HotkeyAction::Quit` from 14 to 15, which broke four harnesses that post
the index by number and one that has a named table. `verify_runtime.py`'s table is the better
pattern and now carries a comment saying why it has to move with the enum; the others fail loudly
when their index is stale, which is how this was caught.

Two checks were added: **[10]** drags the window away from the middle and asserts the hotkey brings
it back to within 2px, and **[11]** drags the size slider and asserts the window's middle does not
move (0px both ways). The core tests gained eight checks for the same property at the geometry
level, where it is exact rather than approximate.

## The packaging revision (2026-09-19)

### The archive is verified as an archive

What goes to someone else is `dist/ScreenMagnifier-1.0.0.zip`, so `tests/verify_package.py` now
tests that and not the build tree: it unpacks the archive into an empty directory **named with
Chinese characters and a space in it** (where a Chinese recipient will actually put it, and this
toolchain links an ANSI entry point), runs the executable from there, checks the magnifier window
comes up showing something rather than black, and checks it exits 0.

The check that earns its keep is the **import table**. A build can pass every other test on this
machine and still be dead on the recipient's, because the loader finds a library here that will not
exist there. Nothing the executable imports is outside Windows — the C++ runtime is linked in
statically — and the check now says so. Listing the process's *loaded* modules does not answer this
question: every process on a machine with an audio or input-method add-on has that vendor's DLLs in
it, which is how the first version of this test came to accuse the app of needing Nahimic and WeType.

### A first run now says how to start

The program opens with the magnifier off. Nothing on screen said what to press, which is the first
thing a new user meets and the first place to conclude that it does not work. The status line now
reads `已关闭（按快捷键，或点下方「开启放大」）` in that state and drops the hint once it is running.

The readme shipped in the archive gained a troubleshooting section, ordered by what the status line
will be showing: a chord another program owns (very common — QQ, WeChat, input methods and driver
utilities all take this class of chord, and the app reports it rather than falling back), a capture
that has dropped to the GDI fallback, a black window, and an exclusive-fullscreen game. Its last
entry asks the recipient to send back that status line, because it carries everything needed to
place the fault.

## What this revision changed

### The settings window is a compact grid that answers a resize

The previous revision's three columns were sized by the chord fields: fifteen of them doubled up
inside one card made that card **806 px wide**, half the window, and the window came out at
**2071 × 734** — barely smaller in area than the single tall column it had replaced, and long and
thin. Making it taller changed nothing at all, because the cards were pinned to the top of their
columns.

Three changes, in the order they were worth:

* **The chords are split across two cards**, `快捷键 · 显示与倍率` and `快捷键 · 窗口与程序`, by what
  each block of actions does. No card is now much wider than the others, which is what lets the
  columns be equal — and equal columns are what make the window read as one grid rather than as
  three unrelated strips. A single chord row is the widest thing left in the window.
* **The column floor is what the cards actually need**, not a comfortable round number. Every card
  but the chords reflows inside about 290 px; the constant said 330, which set the whole window's
  width for no reason. Card heights do not depend on column width, so the arrangement is solved from
  a list of heights before anything is placed.
* **Columns are assigned, not packed.** A greedy shortest-column packer balanced them about 4 %
  better but put 放大倍率 in the bottom right, and it would shuffle the cards when the language
  changes. A fixed split keeps the magnification where the eye starts and is the same in both
  languages.

The result is **1268 × 800**. The footer is pinned to the bottom edge and the cards open up when the
window is stretched, so a resize finally does something: check [9] of `verify_features.py` measures
how far down the window the controls reach before and after (772 px → 862 px), which makes "a taller
window changed nothing" a failure rather than an impression.

One test had to change with the layout: the slider check asserted the dragged factor to ±0.01, which
a track a few hundred pixels wide cannot honour — one pixel of travel is worth about 0.03×. It now
allows for the pixel it asked for, since what it is really checking is that dragging applies at all.

Two mistakes worth recording from this pass, both caught by looking at a screenshot rather than by a
test: the rendering card lost its height budget when a row was added to it (it overflowed onto the
status line), and the chord field was narrowed to the point that `Ctrl+Alt+OemMinus` was clipped.
Neither harness would have failed. **Screenshot the window after a layout change.**

## The typed-factor revision (2026-09-19)

### The settings window is landscape now, and the factor can be typed

Two complaints, one cause: the window was a single column of cards and stood **1377 px tall on a
1380 px work area**, so it was effectively full-screen, and the only way to set the factor was to
drag the slider.

The layout is now **three columns**: the hotkeys card takes the middle one at exactly the width its
two chord columns need, and the other five cards share the outer two, which are the columns a resize
grows. That puts the window at **2071 × 734** — half the height — and it is resizable
(`WS_THICKFRAME`, plus a `WM_GETMINMAXINFO` minimum computed by `layout_controls()` so a resize can
never clip a control out of reach). Every card reflows inside whatever column it is given, so card
heights do not depend on the column width and the layout only has to be solved once.

The zoom read-out (control 1012) is an **edit field** now, keeping its id and its place next to the
slider. It commits itself rather than waiting for a button: `EN_CHANGE` arms a 500 ms timer that is
restarted on every keystroke, so a number takes effect when typing pauses and "10" never applies 1
on the way. `EN_KILLFOCUS` commits too and puts the canonical `4.00x` text back. Anything that
parses is clamped into 1–10×; anything that does not is left alone until the field is done with.
`writing_factor` marks the window's own writes so their `EN_CHANGE` is not mistaken for typing —
without it the periodic sync, which rewrites the box several times a second, killed and re-armed the
timer so that it never once fired.

### Two traps found while testing it

Both cost real time and neither is the app's fault, so they are worth writing down.

* **`SetWindowText` from another process does not raise `EN_CHANGE`.** A harness cannot type into
  the field by setting its text; it produces no notification and nothing commits. The check in
  `verify_features.py` drives it with `SendInput` instead.
* **`GetWindowText` cannot read an edit control's contents across processes.** It returns the window
  manager's *cached* title, so the box appeared to still read `4.00x` long after the app had written
  `2.50x` and read it back correctly. The test judges by the app's own state — the output size in the
  status line — rather than by the box.

## The black-window revision (2026-09-19)

### The magnifier came back black after being switched off and on again

Reported as: switch the magnifier on, drag its window, and switch it on again — or just switch it
off and on — and the magnifier window shows pure black and never recovers.

It was the self-exclusion. `exclude_self_from_capture` applies
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` so the magnifier cannot mirror itself, and that
setting does not survive a teardown of the window's DirectComposition target. Switching the
magnifier off releases the whole GPU session — device, composition target, swap chain — and coming
back rebuilds it, after which the window **still reads back as `WDA_EXCLUDEFROMCAPTURE` from
`GetWindowDisplayAffinity`** while DWM composites it opaque black and no longer excludes it. Nothing
revisited the affinity, so it stayed black.

Three findings made the fix findable, and each ruled out the obvious guess:

* The renderer was never at fault. Clearing the exclusion from the settings window brought the
  magnified picture straight back, so the capture pipeline had been producing correct content the
  whole time — the render loop reported 56 fps with no device resets and no capture-lost notice.
* Re-applying the affinity *before* the rebuilt target presented did nothing, and neither did
  re-applying the same value afterwards: **DWM only reacts to a transition**. It has to be cleared
  while the target is rebuilt and set again once a frame has gone out.
* Dragging or moving the window never triggered it, on its own or afterwards — only the off/on
  cycle, which is the only thing that tears the composition down.

`RenderService` now clears the affinity in `create_composition()` and restores the configured value
after the first `Present()` that follows (`reassert_affinity`). `verify_features.py` gained check
**[7]**, which parks a known colour behind the window and compares the same crop before and after an
off/on cycle — it reports "the window came back black" on the old code and passes on the new.

A second, smaller defect was found while fixing this: `RenderService::Config` is constructed in two
places, and only one was given the new field, so at startup it took its default of `true` and the app
excluded itself even with the setting turned off. `verify_visual.py` caught it — the window stopped
compositing entirely and all four shapes failed. Both construction sites now set it.

## The archive revision (2026-09-18)

The application was packaged as a single executable with its icon and manifest embedded, an archive
was added for handing to someone else, and a CPU regression was found and fixed along the way.

* **`dist/ScreenMagnifier-1.0.0.zip`** holds the executable and `README.txt`, built by
  `tools/make_zip.py`. The readme exists because the executable cannot explain itself: that Windows
  will warn about a downloaded, unsigned program and how to get past it, the default hotkeys, where
  the settings live, and how to remove it. It is written with a UTF-8 BOM so the Chinese renders in
  any editor. Verified by extracting into an empty folder and running it from there.
* **The icon and manifest are embedded after linking.** The toolchain has no resource compiler and
  its linker rejects a `.res` file, so `tools/embed_resources.py` builds the resource directory and
  appends it as a `.rsrc` section. `tools/check_resources.py` verifies the result the way the shell
  sees it. `./build.sh package` produces `dist/ScreenMagnifier.exe`.
* **The embedded manifest is now the runtime one**: PerMonitorV2, the comctl32 v6 dependency, and
  `asInvoker`. The app no longer writes a temporary manifest at startup for `CreateActCtx`.
* **The app uses its embedded icon** rather than drawing one, so the window, the tray and Explorer
  cannot disagree.

### The CPU regression

`verify_runtime.py` failed its CPU gate at 11.56 % of one core, against a 10 % budget. Attributing
the cost with `tests/profile_cpu.py` showed where it actually was:

| State | Before | After |
|---|---:|---:|
| Magnifying, settings window visible | 9.96 % | **1.95 %** |
| Magnifying, settings window hidden | 1.95 % | 3.91 % (noise) |
| Idle | 4.42 % | 0.00 % |

The capture and render path was only ever ~2 %. The rest was `WS_EX_COMPOSITED` on the settings
window: that style makes the system recomposite the whole window and all fifty children on *any*
invalidation, which cost roughly 8 % of a core just for the window existing. It had been added early
to stop flicker, but the two changes that actually fixed the flicker were `WS_CLIPCHILDREN` and the
diff-based `sync()`, so the style was pure cost by then.

A second, smaller change went with it: a hidden settings window is no longer refreshed. The status
line changes on every refresh, so an invisible window was reformatting and re-setting text several
times a second for nothing.

## Fixes carried over from earlier revisions

* **The display's rotation is undone when sampling.** Desktop Duplication returns frames in the
  monitor's native orientation, so on a pivoted display every pixel came from the wrong place. The
  constant buffer carries the tile's UV origin *and its per-axis derivatives* — a min/max UV
  rectangle cannot express a quarter turn.
* **The interface is painted with GDI+** — anti-aliased rounded paths, gradients, soft shadows, a
  self-painting slider and owner-drawn checkboxes — rather than flat fills behind a one-pixel pen.
* **The renderer could die permanently after hiding** — `acquire()` throws while a session is being
  torn down and the loop only caught `DeviceRemoved`.
* **`ps_bicubic` AddRef leak**; **`RenderStats` data race**; **`ErrorReported::text` was a bare
  `const char*`** crossing a thread boundary.
* **The tray icon was inert**; **display and DPI changes were never observed**.
* **Edge-dwell recovery was unreachable**; **the edge-dwell field recursed into itself** through
  `EN_UPDATE` until the stack ran out.
* **The region picker had no way to confirm with the mouse**, and a preset hotkey did not persist.
* **The hotkey labels were clipped** by a feedback loop: the label column was measured from the
  control whose width the layout itself set.

## Known remaining gaps

1. **`validate()` does not clamp an out-of-range selection** — a span wider than `INT32_MAX` in a
   hand-edited config overflows `width_of()`.
2. **`ConfigStore::last_error()` has no reader**, so a corrupt config silently reverts to defaults.
3. The JSON reader bounds the file's *bytes* (16 MB) rather than the parsed object graph.
4. Cross-monitor selection is implemented but has never run against real multi-monitor hardware.
5. Only rotations 0 and 90 are verified; 180 and 270 come from the same table but are untested.
6. The executable is unsigned. The design document calls for Authenticode signing; no certificate
   was available, and the embedder recomputes no checksum, so signing would have to happen after
   packaging.

## Verification method caveat

On this machine `SendInput`/`keybd_event` do **not** trigger `RegisterHotKey`. The harnesses verify
OS registration separately (by requiring `ERROR_HOTKEY_ALREADY_REGISTERED`) and drive the pipeline by
posting `WM_HOTKEY` to the input thread. Any Python harness must also call
`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` or every measurement comes back scaled by the
display factor. Both traps are documented in `README.md`.

A third one is specific to testing the magnifier window's own pixels. It excludes itself from capture
by default, so with the shipped settings **a screenshot of that window is not the magnified image at
all** — it is whatever is behind it, and a window that has gone wrong reads as opaque black. A test
that crops the window therefore cannot tell "rendering correctly" from "not rendering", and one that
turns the exclusion off to look inside stops reproducing anything that depends on it. Park a known
solid colour behind the window and compare the same crop across the transition instead: the colour
means the window is composited and properly excluded, black means it is broken, and either way the
comparison is what has teeth. `verify_features.py` check [7] does this.

A fourth: a second instance of the app exits immediately with code 0, so one left running from an
earlier harness makes the next launch look like it failed to start. Check for a stray `magnifier.exe`
before trusting a harness failure.

A fifth: `SetForegroundWindow` from a process that is not itself in the foreground is refused, and
it is refused *silently*. Any keystroke the harness injects afterwards goes to whichever window does
hold the foreground, so a check that types into a field reports the program ignoring input when the
program never saw the keys. Click the target window's title bar instead — a real click is always
granted — and `take_foreground()` in `verify_features.py` / `verify_runtime.py` does that. This one
cost an afternoon: it produced a convincing "every second rebind is lost" that was entirely the
probe.
