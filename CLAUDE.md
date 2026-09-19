# Screen Magnifier — working notes

A Windows screen magnifier: C++20, Win32, DXGI Desktop Duplication for capture, DirectComposition
for per-pixel-alpha presentation. Chinese by default with an in-app English switch.

## Build and test

There is **no MSVC and no Windows SDK on this machine**. The only Visual Studio is 2010, which
predates C++20 and ships no `windows.h`. Build with the clang bundled in the `ziglang` package:

```bash
./build.sh package      # dist/ScreenMagnifier.exe + the .zip  (the shippable artifacts)
./build.sh release      # build/magnifier.exe
./build.sh test         # 2809 core unit checks
./build.sh console      # same as release, keeps a console attached
```

Acceptance harnesses, each of which launches the real executable:

```bash
python tests/verify_package.py    # the shipped .zip: unpack it, run it, magnify something
python tests/verify_features.py   # shipped defaults: language, centring, 10x cap, slider, reset
python tests/verify_picker.py     # region picker: move, confirm button, double click, Enter
python tests/verify_runtime.py    # 17 black-box acceptance checks
python tests/verify_visual.py     # pixel-exact magnification, all four shapes
python tests/verify_stress.py     # show/hide races and WM_DISPLAYCHANGE
python tests/measure_perf.py      # the resource budgets
python tests/profile_cpu.py       # attributes CPU to window / capture / renderer
python tools/check_resources.py   # what the shell sees in a built executable
```

## Two traps that produce convincing false failures

1. **`SendInput`/`keybd_event` do not trigger `RegisterHotKey` here.** A Python self-test that
   registers its own hotkey and injects the matching chord receives nothing, so injecting keystrokes
   tests the harness rather than the product. The harnesses verify OS registration separately by
   requiring `ERROR_HOTKEY_ALREADY_REGISTERED`, then drive everything downstream by posting
   `WM_HOTKEY` to the app's input thread.
   `RegisterHotKey` is the *only* thing injected keys miss: they reach a `WH_KEYBOARD_LL` hook
   normally, which is why `verify_runtime.py` can test the hotkey fallback end to end — it takes
   `Ctrl+Alt+M` for itself before launching the app, then types it.
2. **A Python harness must call `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`**
   (`ctypes.c_void_p(-4)`). Otherwise Windows silently virtualises `GetWindowRect` and `ImageGrab`
   by the display scale factor, and every measurement comes back wrong by that factor — which looks
   exactly like an application bug and is not one.

## Before changing anything

- `README.md` explains the design and the reasoning behind it.
- `docs/STATUS.md` is the current state: what was fixed and why, and the list of known gaps.
- **The display on this machine is rotated (portrait).** Anything that samples a captured frame must
  honour `GpuFrame::rotation`; a regression there is invisible on a landscape screen and total here.
- The settings window paints through GDI+ (`src/app/ui_draw.*`). Do not put `WS_EX_COMPOSITED` back:
  it recomposites the whole window and every child on any invalidation, and cost ~8 % of a core on
  its own. `WS_CLIPCHILDREN` plus a diff-based `sync()` are what actually prevent flicker.
- A change counts as verified only when a test that *runs the app* exercises it. Prefer extending
  `tests/verify_features.py` or `tests/verify_visual.py` over asserting from the source.
