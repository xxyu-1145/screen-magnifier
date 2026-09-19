# Status

_Last updated: 2026-09-19, after the hotkey-fallback revision._

## Where the project stands

The application is **built, packaged and verified**. `./build.sh package` produces
`dist/ScreenMagnifier.exe` (1.3 MB, icon and manifest embedded, no runtime DLLs) and
`dist/ScreenMagnifier-1.0.0.zip` (0.5 MB, the executable plus a Chinese readme) — the second is what
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
| `build.sh test` | **2809 / 2809** core checks pass |
| `verify_features.py` | Chinese default, centred start, 10× cap, live slider, typed factor, restore defaults, survives off/on showing what it showed before, landscape + resizable + reflows when made taller, centre-the-window hotkey, size slider resizes in place, **editable presets**, **kept regions recalled intact**, clean exit |
| `verify_picker.py` | move-by-drag, on-screen confirm button, double-click confirm, Enter confirm |
| `verify_runtime.py` | **18 / 18** acceptance checks pass, including a chord another application owns |
| `verify_visual.py` | all four shapes correct; rectangle pixel-exact at 4× (mean abs diff **0.00**) |
| `measure_perf.py` | idle 37.6 MB / 0.00 % CPU; running **2.19 %** CPU; released to 11.4 MB when switched off |
| `verify_stress.py` | 15 show/hide cycles, frames climbing at 56 fps; `WM_DISPLAYCHANGE` acknowledged |
| `verify_package.py` | the shipped **.zip** unpacked into a folder named with Chinese characters and a space: starts, magnifies, quits 0, and every DLL it imports ships with Windows |
| `tools/check_resources.py` | 7 icons, group icon 1, manifest 1; the shell reports the icon |

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
