"""Pixel-exact magnification under whatever display orientation is live.

Desktop Duplication hands back frames in the monitor's *native* orientation, so
on a pivoted display every pixel comes from the wrong place unless the rotation
compensation (GpuFrame::rotation) runs -- a defect invisible on a landscape
screen and total on a pivoted one. Rotation 0 is covered by every other pixel
harness; 90 was covered once, incidentally, while an external monitor happened
to be off; 180 and 270 had never run at all until this harness met a pivoted
panel.

It measures the live orientation, then does one pixel-exact check at 2x
(the same methodology as verify_visual.py: a known pattern on screen, the app
pointed at it through its own config, the window's pixels compared against the
pattern scaled by exactly the factor). On an ordinary landscape screen it is a
small repeat of verify_visual's rectangle case and passes; on a pivoted panel
it is the rotation proof. The verdict line always names the orientation it ran
under -- a green run is only rotation coverage if the orientation says so.

Run from the project root:  python tests/verify_rotation.py
With --sweep it first rotates the live display through the other portrait
orientations (the same kind of real display change verify_resolutions.py makes)
and checks each, restoring the original orientation afterwards. On this
machine's internal panel the driver refuses 180 (DISP_CHANGE_FAILED), so the
sweep covers 90 only; 180 stays unverified until hardware that accepts it.

Exit code is 0 on pass or skip, 1 on failure.
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_resolutions as vr  # noqa: E402

FACTOR = 2

ORIENTATION_NAMES = {0: "0 (landscape)", 1: "90", 2: "180", 3: "270"}

# DM_DISPLAYORIENTATION in DEVMODE.dmFields. The driver of the internal panel
# does not enumerate rotated modes, so a rotation is requested by editing the
# live mode directly; 180 of it is refused with DISP_CHANGE_FAILED.
DM_DISPLAYORIENTATION = 0x00000080


def orientation_mode(want: int, like) -> "vr.DEVMODEW":
    dm = vr.DEVMODEW()
    vr.ctypes.memmove(vr.ctypes.byref(dm), vr.ctypes.byref(like), vr.ctypes.sizeof(dm))
    dm.dmFields |= DM_DISPLAYORIENTATION
    dm.u.dsp.dmDisplayOrientation = want
    return dm


def run_check() -> tuple[bool, str, int]:
    """One verification as a subprocess, so a crash cannot take the sweep down."""
    run = subprocess.run([sys.executable, str(Path(__file__).resolve())],
                         capture_output=True, text=True, timeout=180)
    lines = [ln for ln in run.stdout.strip().splitlines() if ln.strip()]
    print("\n".join(lines[-3:]))
    verdict = lines[-1] if lines else "(no output)"
    orientation = 0
    for ln in lines:
        if "orientation" in ln and "mode " in ln:
            try:
                orientation = int(ln.split("orientation")[1].split()[0].strip("()"))
            except (ValueError, IndexError):
                pass
    return run.returncode == 0, verdict, orientation


def sweep() -> int:
    original = vr.current_mode()
    ow, oh = original.dmPelsWidth, original.dmPelsHeight
    print(f"original: {ow}x{oh}, orientation "
          f"{ORIENTATION_NAMES.get(original.u.dsp.dmDisplayOrientation)}")
    ok_any = False
    restore_ok = True
    try:
        for target in (1, 2):            # 90 first; 180 is refused by this panel
            dm = orientation_mode(target, original)
            if not vr.set_mode(dm, dm.dmPelsWidth, dm.dmPelsHeight):
                print(f"orientation {ORIENTATION_NAMES[target]}: the driver refused the "
                      f"switch (known for 180 on this panel); skipped")
                continue
            time.sleep(1.0)
            print(f"--- orientation {ORIENTATION_NAMES[target]} ---")
            ok, verdict, seen = run_check()
            ok_any = ok_any or ok
            print(f"orientation {ORIENTATION_NAMES[target]}: {'PASS' if ok else 'FAIL'}")
    finally:
        restore_ok = vr.set_mode(original, ow, oh)
        if not restore_ok:
            print("!! could not restore the original orientation -- set it back by hand")
        else:
            print("restored the original orientation")
    return 0 if (restore_ok and ok_any) else 1


def main() -> int:
    print("=" * 74)
    print("Screen Magnifier - rotation-aware magnification check")
    print("=" * 74)

    if "--sweep" in sys.argv:
        return sweep()

    dm = vr.current_mode()
    mode_w, mode_h = dm.dmPelsWidth, dm.dmPelsHeight
    orientation = dm.u.dsp.dmDisplayOrientation
    screen_w, screen_h = vr.u.GetSystemMetrics(0), vr.u.GetSystemMetrics(1)
    print(f"mode {mode_w}x{mode_h} @ {dm.dmDisplayFrequency} Hz, "
          f"orientation {ORIENTATION_NAMES.get(orientation, orientation)}")
    print(f"desktop {screen_w}x{screen_h}")

    (win_x, win_y), fits = vr.pixel_geometry(screen_w, screen_h, FACTOR)
    if not fits:
        # A screen narrower than the pattern plus a 2x window beside it cannot
        # host the check; say so rather than fudge the geometry.
        print(f"SKIP: the {screen_w}x{screen_h} desktop cannot hold the pattern "
              f"and a {vr.SRC_W * FACTOR}x{vr.SRC_H * FACTOR} window side by side")
        return 0

    vr.kill_leftovers()
    if vr.CONFIG.exists():
        vr.CONFIG.unlink()

    pattern = vr.make_pattern()
    app = None
    try:
        time.sleep(0.5)
        # Let the app write its own config once, then point it at the pattern
        # (schema-honest seeding, as in verify_visual.py -- quit first, or the
        # app's own shutdown save overwrites the seed).
        app = vr.launch(env_diag=True)
        vr.press(app.pid, "ZoomIn")
        time.sleep(1.5)
        vr.quit_app(app)
        app = None
        vr.seed_config(FACTOR, win_x, win_y)

        app = vr.launch()
        try:
            vr.press(app.pid, "ToggleMagnifier")
            time.sleep(3.5)
            ok, detail = vr.pixel_check(app.pid, FACTOR)
        finally:
            code = vr.quit_app(app)
            app = None
            if code != 0:
                print(f"note: exit code {code}")
                ok = False
    finally:
        if app is not None and app.poll() is None:
            app.terminate()
        try:
            pattern.destroy()
        except Exception:  # noqa: BLE001
            pass

    print(f"magnification is pixel-exact at {FACTOR}x under orientation "
          f"{ORIENTATION_NAMES.get(orientation, orientation)}: "
          f"{'PASS' if ok else 'FAIL'} -- {detail}")
    if ok and orientation == 0:
        print("note: this run was landscape, so it repeats verify_visual rather "
              "than covering the rotation path")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
