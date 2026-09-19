"""Prove the magnifier shows the right pixels at the right scale, in every shape.

Guessing whether a screenshot "looks magnified" is not a test. This one puts a
unique, known pattern on screen at a known physical position, points the
magnifier at exactly that rectangle through its own configuration file, and then
checks the magnifier window's pixels:

  * the applied scale equals the configured factor,
  * the centre of the window is the source region scaled -- pixel-exact for the
    rectangle case, where an integer factor lets nearest-neighbour sampling
    reproduce the source exactly, and
  * the corners are inside the mask for a rectangle but masked away for a
    circle, ellipse and rounded rectangle. "Masked away" means the window is
    transparent there, so the screenshot shows the desktop underneath instead.

Run from the project root:  python tests/verify_visual.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageGrab

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
CONFIG = Path(os.environ.get("APPDATA", "")) / "Magnifier" / "config.json"
OUT = ROOT / "build" / "visual"

WM_HOTKEY = 0x0312
BASE = 0x4000
TOGGLE_MAGNIFIER = 0

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")


def make_dpi_aware() -> None:
    """Match the app's coordinate space.

    The app is PerMonitorV2 aware, so every rectangle it reports is in physical
    pixels. Without this the test process is not, and Windows silently
    virtualises GetWindowRect and ImageGrab by the display scale factor -- on a
    125% display that made every measurement 0.8x too small, which looks
    exactly like an application bug and is not one.
    """
    try:
        if u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
            return
    except Exception:  # noqa: BLE001
        pass
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
    except Exception:  # noqa: BLE001
        pass


make_dpi_aware()

SRC_W, SRC_H = 320, 240
FACTOR = 4

# The geometry is derived from the desktop rather than fixed, so the test works
# on a landscape screen, a portrait one, or anything in between. The magnifier
# window must fit entirely on screen or its screenshot crop would run off the
# edge.
_desktop_w = u.GetSystemMetrics(0)
_desktop_h = u.GetSystemMetrics(1)
SRC_X, SRC_Y = 40, 40
WIN_W, WIN_H = SRC_W * FACTOR, SRC_H * FACTOR
WIN_X = max(SRC_X + SRC_W + 20, min(_desktop_w - WIN_W - 20, 700))
WIN_Y = max(SRC_Y + SRC_H + 20, min(_desktop_h - WIN_H - 20, 400))
if WIN_X < SRC_X + SRC_W + 20 or WIN_Y < SRC_Y + SRC_H + 20:
    print(f"warning: the desktop ({_desktop_w}x{_desktop_h}) is too small for a "
          f"{WIN_W}x{WIN_H} window beside the pattern")



def thread_ids(pid: int) -> list[int]:
    class TE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                    ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]
    snap = k.CreateToolhelp32Snapshot(0x4, 0)
    out: list[int] = []
    te = TE()
    te.dwSize = ctypes.sizeof(te)
    if k.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            te.dwSize = ctypes.sizeof(te)
            if not k.Thread32Next(snap, ctypes.byref(te)):
                break
    k.CloseHandle(snap)
    return out


def find(pid: int, cls: str):
    res = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(o))
        if o.value == pid:
            b = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(h, b, 256)
            if b.value == cls:
                r = wt.RECT()
                u.GetWindowRect(h, ctypes.byref(r))
                res.append((h, bool(u.IsWindowVisible(h)), (r.left, r.top, r.right, r.bottom)))
        return True

    u.EnumWindows(cb, 0)
    return res


def status_of(pid: int) -> str:
    out: list[str] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(4096)
        u.GetWindowTextW(h, b, 4096)
        if " fps" in b.value:
            out.append(b.value.split("\n")[0])
        return True

    for h, _, _ in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out[0] if out else "(no status)"


def press(pid: int, action: int) -> None:
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + action, 0)


def launch(timeout: float = 20.0):
    p = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.25)
        if p.poll() is not None:
            raise SystemExit(f"app exited during startup (code {p.returncode})")
        if find(p.pid, "MagControlWindow"):
            time.sleep(1.0)
            return p
    raise SystemExit("app never created its settings window")


def quit_app(p) -> None:
    if p.poll() is None:
        press(p.pid, 15)  # Quit (HotkeyAction::Quit)
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


def seed_config(shape: str) -> dict:
    """Let the app write its own config once, then point it at the pattern.

    Seeding through the app rather than hand-writing the file keeps the test
    honest about the schema: whatever the app writes is what it will read.
    """
    if CONFIG.exists():
        CONFIG.unlink()
    p = launch()
    press(p.pid, 3)  # ZoomIn, which triggers a save
    time.sleep(2.0)
    quit_app(p)
    if not CONFIG.exists():
        raise SystemExit("the app never wrote a config file")

    cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    cfg["selection_bounds_px"] = [SRC_X, SRC_Y, SRC_X + SRC_W, SRC_Y + SRC_H]
    cfg["selection_shape"] = shape
    cfg["selection_corner_radius_px"] = 48
    cfg["factor_q16"] = FACTOR * 65536
    cfg["output_size_px"] = [SRC_W * FACTOR, SRC_H * FACTOR]
    cfg["output_position_auto"] = False
    cfg["output_position_px"] = [WIN_X, WIN_Y]
    cfg["keep_aspect_ratio"] = True
    cfg["scale_filter"] = "Point"
    # The app excludes itself from capture by default so it can never mirror its
    # own output. That also hides it from this test's screenshot, so the check
    # turns the exclusion off -- safe here because the pattern and the magnifier
    # window do not overlap.
    cfg["exclude_self_from_capture"] = False
    CONFIG.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    return cfg


def make_pattern():
    """A small tkinter window whose pixels are unmistakable."""
    import tkinter as tk
    root = tk.Tk()
    root.overrideredirect(True)
    root.geometry(f"{SRC_W}x{SRC_H}+{SRC_X}+{SRC_Y}")
    root.attributes("-topmost", True)
    canvas = tk.Canvas(root, width=SRC_W, height=SRC_H, highlightthickness=0)
    canvas.pack()
    for colour, qx, qy in [("#e02020", 0, 0), ("#20c020", 1, 0),
                           ("#2040e0", 0, 1), ("#f0c000", 1, 1)]:
        canvas.create_rectangle(qx * SRC_W // 2, qy * SRC_H // 2,
                                (qx + 1) * SRC_W // 2, (qy + 1) * SRC_H // 2,
                                fill=colour, outline="")
    # Fine stripes give the comparison high-frequency detail, and the ring makes
    # a mirrored or rotated read unmistakable.
    for i in range(0, SRC_W // 2, 4):
        canvas.create_line(i, 0, i, SRC_H // 2, fill="#000000")
    canvas.create_oval(SRC_W // 4, SRC_H // 4, 3 * SRC_W // 4, 3 * SRC_H // 4,
                       outline="#ffffff", width=5)
    canvas.create_text(SRC_W // 2, SRC_H // 8, text="MAG", fill="#ffffff",
                       font=("Consolas", 22, "bold"))
    root.update()
    return root


def mean_abs_diff(a: Image.Image, b: Image.Image) -> float:
    """Average per-channel difference, sampled on a grid so it stays fast."""
    if a.size != b.size:
        b = b.resize(a.size)
    pa, pb = a.load(), b.load()
    step = max(1, min(a.width, a.height) // 48)
    total, n = 0, 0
    for y in range(0, a.height, step):
        for x in range(0, a.width, step):
            ca, cb = pa[x, y], pb[x, y]
            total += abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
            n += 3
    return total / max(n, 1)


def run_case(shape: str) -> bool:
    print(f"\n=== shape: {shape} ===")
    seed_config(shape)
    app = None
    try:
        app = launch()
        press(app.pid, TOGGLE_MAGNIFIER)
        time.sleep(3.5)

        ov = find(app.pid, "MagOverlayWindow")
        if not ov or not ov[0][1]:
            print("FAIL: the magnifier window is not visible")
            return False
        wx0, wy0, wx1, wy1 = ov[0][2]
        ww, wh = wx1 - wx0, wy1 - wy0
        print(f"window on screen   : {ww}x{wh} at ({wx0},{wy0})")

        # The app's own status line, so a blank window can be told apart from a
        # window showing the wrong region.
        print("app status:", status_of(app.pid))

        shot = ImageGrab.grab(all_screens=True).convert("RGB")
        source = shot.crop((SRC_X, SRC_Y, SRC_X + SRC_W, SRC_Y + SRC_H))
        # Saved so a mismatch can be looked at rather than guessed about.
        source.save(OUT / f"source_{shape}.png")
        window = shot.crop((wx0, wy0, wx1, wy1))

        scale = min(ww / SRC_W, wh / SRC_H)
        expected = source.resize((int(SRC_W * scale), int(SRC_H * scale)), Image.NEAREST)
        ox, oy = (ww - expected.width) // 2, (wh - expected.height) // 2
        actual = window.crop((ox, oy, ox + expected.width, oy + expected.height))
        at_zero = mean_abs_diff(expected, actual)

        def sample(img, fx, fy):
            x = max(0, min(img.width - 1, int(img.width * fx)))
            y = max(0, min(img.height - 1, int(img.height * fy)))
            return img.getpixel((x, y))

        centre_ok = True
        for fx, fy in ((0.5, 0.5), (0.45, 0.5), (0.5, 0.45)):
            e, a = sample(expected, fx, fy), sample(actual, fx, fy)
            if sum(abs(e[i] - a[i]) for i in range(3)) > 40:
                centre_ok = False

        corner_diff = 0
        for fx, fy in ((0.03, 0.03), (0.97, 0.03), (0.03, 0.97), (0.97, 0.97)):
            e, a = sample(expected, fx, fy), sample(actual, fx, fy)
            corner_diff += sum(abs(e[i] - a[i]) for i in range(3))

        scale_ok = abs(scale - FACTOR) < 0.05
        if shape == "Rectangle":
            corner_ok = corner_diff < 120      # corners are inside the mask
            corner_note = f"inside the mask (diff {corner_diff})"
            exact_ok = at_zero < 2.0
        else:
            corner_ok = corner_diff > 120      # corners are masked away
            corner_note = f"masked away (diff {corner_diff})"
            exact_ok = True

        print(f"applied scale      : {scale:.2f}x (configured {FACTOR}.00x)")
        print(f"mean abs diff      : {at_zero:.2f} per channel")
        print(f"centre             : {'shows the pattern' if centre_ok else 'WRONG'}")
        print(f"corners            : {corner_note}")
        window.save(OUT / f"window_{shape}.png")

        ok = scale_ok and centre_ok and corner_ok and exact_ok
        print(f"  scale matches the configured factor : {scale_ok}")
        print(f"  centre shows the source pattern     : {centre_ok}")
        print(f"  corners behave as the shape implies : {corner_ok}")
        if shape == "Rectangle":
            print(f"  output is pixel-exact               : {exact_ok}")
        print("  -> " + ("PASS" if ok else "FAIL"))
        return ok
    finally:
        if app is not None:
            quit_app(app)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"pattern [{SRC_X},{SRC_Y},{SRC_X+SRC_W},{SRC_Y+SRC_H}] at {FACTOR}x "
          f"-> window {SRC_W*FACTOR}x{SRC_H*FACTOR} at ({WIN_X},{WIN_Y})")
    root = make_pattern()
    results: dict[str, bool] = {}
    try:
        time.sleep(1.0)
        for shape in ("Rectangle", "Circle", "Ellipse", "RoundedRectangle"):
            results[shape] = run_case(shape)
    finally:
        try:
            root.destroy()
        except Exception:  # noqa: BLE001
            pass

    print("\n" + "=" * 58)
    for shape, ok in results.items():
        print(f"  {'PASS' if ok else 'FAIL'}  {shape}")
    print("=" * 58)
    return sum(1 for ok in results.values() if not ok)


if __name__ == "__main__":
    sys.exit(main())
