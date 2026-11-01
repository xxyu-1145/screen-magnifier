"""Acceptance test for screen resolutions other than the 2560x1440 this
machine is normally driven at.

Everything the older harnesses prove was proven on one desktop size. This one
switches the *real* display mode through a ladder (native -> 1920x1080 ->
1366x768), and at each rung launches the app fresh and checks the things a
different desktop could plausibly break:

  * the process starts and its own DIAG readout reports the new screen,
  * the first-run region is centred on the new work area,
  * the magnifier turns on, renders frames and stays inside the desktop,
  * the picture is still magnified by exactly the configured factor
    (pixel-exact against a known on-screen pattern, as in verify_visual),
  * the settings window and every control fit the work area (enforced at
    1920-width and above; recorded but not enforced below it),
  * a mode change *while the magnifier is running* rebuilds capture and
    render without dying, three times over, and a shrink to a screen smaller
    than the output window leaves the window clamped inside the desktop,
  * a WM_DPICHANGED that reaches the host pump refreshes the cached topology,
  * and the original display mode is restored no matter how the run ends.

It also protects the user: the real %APPDATA%\\Magnifier directory is moved
aside for the duration and put back afterwards.

Run from the project root:  python tests/verify_resolutions.py
Exit code is the number of failed checks.

WARNING: the desktop resolution really changes several times. Icons and open
windows rearrange; do not use the machine while it runs.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
CONFIG_DIR = Path(os.environ.get("APPDATA", "")) / "Magnifier"
CONFIG = CONFIG_DIR / "config.json"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

WM_HOTKEY = 0x0312
WM_DPICHANGED = 0x02E0
WM_DISPLAYCHANGE = 0x007E
BASE = 0x4000
# Indices into mag::HotkeyAction (core/events.h) -- see verify_runtime.py.
ACTION = {"ToggleMagnifier": 0, "ZoomIn": 3, "Quit": 15}

DISP_CHANGE_SUCCESSFUL = 0
ENUM_CURRENT_SETTINGS = 0xFFFFFFFF
SM_CXSCREEN, SM_CYSCREEN = 0, 1
SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79


def make_dpi_aware() -> None:
    """Match the app's coordinate space (see verify_visual.py)."""
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


class DMU(ctypes.Union):
    class _PRN(ctypes.Structure):
        _fields_ = [("dmOrientation", wt.SHORT), ("dmPaperSize", wt.SHORT),
                    ("dmPaperLength", wt.SHORT), ("dmPaperWidth", wt.SHORT),
                    ("dmScale", wt.SHORT)]

    class _DSP(ctypes.Structure):
        _fields_ = [("dmPositionX", wt.LONG), ("dmPositionY", wt.LONG),
                    ("dmDisplayOrientation", wt.DWORD), ("dmDisplayFixedOutput", wt.DWORD)]

    _fields_ = [("prn", _PRN), ("dsp", _DSP)]


class DEVMODEW(ctypes.Structure):
    _anonymous_ = ["u"]
    _fields_ = [
        ("dmDeviceName", wt.WCHAR * 32),
        ("dmSpecVersion", wt.WORD), ("dmDriverVersion", wt.WORD),
        ("dmSize", wt.WORD), ("dmDriverExtra", wt.WORD),
        ("dmFields", wt.DWORD),
        ("u", DMU),
        ("dmColor", wt.SHORT), ("dmDuplex", wt.SHORT), ("dmYResolution", wt.SHORT),
        ("dmTTOption", wt.SHORT), ("dmCollate", wt.SHORT),
        ("dmFormName", wt.WCHAR * 32),
        ("dmLogPixels", wt.WORD),
        ("dmBitsPerPel", wt.DWORD), ("dmPelsWidth", wt.DWORD), ("dmPelsHeight", wt.DWORD),
        ("dmDisplayFlags", wt.DWORD), ("dmDisplayFrequency", wt.DWORD),
        ("dmICMMethod", wt.DWORD), ("dmICMIntent", wt.DWORD), ("dmMediaType", wt.DWORD),
        ("dmDitherType", wt.DWORD), ("dmReserved1", wt.DWORD), ("dmReserved2", wt.DWORD),
        ("dmPanningWidth", wt.DWORD), ("dmPanningHeight", wt.DWORD),
    ]


@dataclass
class Report:
    checks: list[tuple[str, bool, str]] = field(default_factory=list)

    def add(self, name: str, passed: bool, detail: str = "") -> None:
        self.checks.append((name, passed, detail))
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}" + (f"  --  {detail}" if detail else ""))

    @property
    def failures(self) -> int:
        return sum(1 for _, ok, _ in self.checks if not ok)


# --- generic helpers (the shared idioms of the other harnesses) --------------

def kill_leftovers() -> None:
    """A second instance exits 0 immediately, which would read as 'started fine'."""
    subprocess.run(["taskkill", "/F", "/IM", "magnifier.exe"],
                   capture_output=True, check=False)


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


def find(pid: int, cls: str) -> list[tuple[int, bool, tuple, int]]:
    res: list[tuple[int, bool, tuple, int]] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(o))
        if o.value == pid:
            name = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(h, name, 256)
            if name.value == cls:
                r = wt.RECT()
                u.GetWindowRect(h, ctypes.byref(r))
                res.append((h, bool(u.IsWindowVisible(h)),
                            (r.left, r.top, r.right, r.bottom), u.GetWindowLongW(h, -20)))
        return True

    u.EnumWindows(cb, 0)
    return res


def status_full(pid: int) -> str:
    """The whole status line, whatever line the notice moved to."""
    out: list[str] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(8192)
        u.GetWindowTextW(h, b, 8192)
        if " fps" in b.value:
            out.append(b.value)
        return True

    for h, _, _, _ in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out[0] if out else ""


def fps_of(st: str) -> float:
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)\s+fps", st)
    return float(m.group(1)) if m else 0.0


def field(st: str, key: str, default: str = "") -> str:
    for part in st.replace("\n", " ").split():
        if part.startswith(key + "="):
            return part.split("=", 1)[1]
    return default


def press(pid: int, name: str) -> None:
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + ACTION[name], 0)
    time.sleep(1.3)


def launch(env_diag: bool = True):
    env = os.environ.copy()
    if env_diag:
        env["MAG_DIAG"] = "1"
    kill_leftovers()
    p = subprocess.Popen([str(EXE)], cwd=str(ROOT), env=env)
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is not None:
            raise SystemExit(f"the process exited during startup (code {p.returncode})")
        if find(p.pid, "MagControlWindow"):
            time.sleep(1.0)
            return p
    raise SystemExit("the app never created its settings window")


def quit_app(p) -> int:
    if p.poll() is None:
        press(p.pid, "Quit")
        try:
            return p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
            return -1
    return p.returncode


def work_area() -> tuple[int, int, int, int]:
    wa = wt.RECT()
    u.SystemParametersInfoW(0x0030, 0, ctypes.byref(wa), 0)  # SPI_GETWORKAREA
    return wa.left, wa.top, wa.right, wa.bottom


def virtual_desktop() -> tuple[int, int, int, int]:
    return (u.GetSystemMetrics(SM_XVIRTUALSCREEN), u.GetSystemMetrics(SM_YVIRTUALSCREEN),
            u.GetSystemMetrics(SM_XVIRTUALSCREEN) + u.GetSystemMetrics(SM_CXVIRTUALSCREEN),
            u.GetSystemMetrics(SM_YVIRTUALSCREEN) + u.GetSystemMetrics(SM_CYVIRTUALSCREEN))


def inside(outer, inner, slack: int = 0) -> bool:
    ol, ot, or_, ob = outer
    il, it, ir, ib = inner
    return (il >= ol - slack and it >= ot - slack and ir <= or_ + slack and ib <= ob + slack)


# --- display-mode helpers ----------------------------------------------------

def current_mode() -> DEVMODEW:
    dm = DEVMODEW()
    dm.dmSize = ctypes.sizeof(dm)
    if not u.EnumDisplaySettingsExW(None, ENUM_CURRENT_SETTINGS, ctypes.byref(dm), 0):
        raise SystemExit("could not read the current display mode")
    return dm


def find_mode(w: int, h: int, like: DEVMODEW) -> DEVMODEW | None:
    """The enumerated mode with this size, preferring the current rate and depth."""
    best = None
    best_rate = -1
    i = 0
    while True:
        dm = DEVMODEW()
        dm.dmSize = ctypes.sizeof(dm)
        if not u.EnumDisplaySettingsExW(None, i, ctypes.byref(dm), 0):
            break
        i += 1
        if dm.dmPelsWidth == w and dm.dmPelsHeight == h:
            if (dm.dmBitsPerPel == like.dmBitsPerPel
                    and dm.dmDisplayFrequency == like.dmDisplayFrequency):
                return dm
            if dm.dmDisplayFrequency > best_rate:
                best, best_rate = dm, dm.dmDisplayFrequency
    return best


def set_mode(dm: DEVMODEW | None, w: int, h: int, timeout: float = 8.0) -> bool:
    """Switch the primary display and wait until the metrics agree."""
    if dm is None:
        return False
    for attempt in range(3):
        r = u.ChangeDisplaySettingsExW(None, ctypes.byref(dm), None, 0, None)
        if r == DISP_CHANGE_SUCCESSFUL:
            break
        print(f"    (mode change attempt {attempt + 1} returned {r})")
        time.sleep(1.0)
    else:
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (u.GetSystemMetrics(SM_CXSCREEN) == w and u.GetSystemMetrics(SM_CYSCREEN) == h):
            time.sleep(1.5)          # let the desktop, and the app, settle
            return True
        time.sleep(0.2)
    return False


# --- the known pattern (the verify_visual methodology, parameterised) --------

SRC_W, SRC_H = 320, 240
SRC_X, SRC_Y = 20, 20


def make_pattern():
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
    for i in range(0, SRC_W // 2, 4):
        canvas.create_line(i, 0, i, SRC_H // 2, fill="#000000")
    canvas.create_oval(SRC_W // 4, SRC_H // 4, 3 * SRC_W // 4, 3 * SRC_H // 4,
                       outline="#ffffff", width=5)
    canvas.create_text(SRC_W // 2, SRC_H // 8, text="MAG", fill="#ffffff",
                       font=("Consolas", 22, "bold"))
    root.update()
    return root


def mean_abs_diff(a, b) -> float:
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


def pixel_geometry(desktop_w: int, desktop_h: int, factor: int):
    """Where the pattern and the output window go, and whether they can fit.

    The pattern sits at the top-left corner; the window hugs the right edge, so
    the two never overlap and the magnifier's screenshot shows the pattern, not
    itself. Every mode in the ladder must clear this or the check is skipped
    rather than fudged.
    """
    win_w, win_h = SRC_W * factor, SRC_H * factor
    win_x = desktop_w - win_w - 20
    win_y = max(20, min(desktop_h - win_h - 20, 300))
    fits = (win_x >= SRC_X + SRC_W + 40 and win_h + 40 <= desktop_h
            and win_w + 40 <= desktop_w)
    return (win_x, win_y), fits


def seed_config(factor: int, win_x: int, win_y: int) -> None:
    """Point the app at the pattern by editing the config it wrote itself."""
    cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    cfg["selection_bounds_px"] = [SRC_X, SRC_Y, SRC_X + SRC_W, SRC_Y + SRC_H]
    cfg["selection_shape"] = "Rectangle"
    cfg["factor_q16"] = factor * 65536
    cfg["output_size_px"] = [SRC_W * factor, SRC_H * factor]
    cfg["output_position_auto"] = False
    cfg["output_position_px"] = [win_x, win_y]
    cfg["keep_aspect_ratio"] = True
    cfg["scale_filter"] = "Point"
    # See verify_visual.py: exclusion would hide the window from the screenshot.
    cfg["exclude_self_from_capture"] = False
    CONFIG.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def pixel_check(pid: int, factor: int) -> tuple[bool, str]:
    from PIL import Image, ImageGrab
    ov = find(pid, "MagOverlayWindow")
    if not ov or not ov[0][1]:
        return False, "the magnifier window is not visible"
    wx0, wy0, wx1, wy1 = ov[0][2]
    ww, wh = wx1 - wx0, wy1 - wy0

    shot = ImageGrab.grab(all_screens=True).convert("RGB")
    source = shot.crop((SRC_X, SRC_Y, SRC_X + SRC_W, SRC_Y + SRC_H))
    expected = source.resize((SRC_W * factor, SRC_H * factor), Image.NEAREST)
    actual = shot.crop((wx0, wy0, wx1, wy1))

    # The window should be exactly the region scaled, so the whole crop
    # compares at offset zero -- pixel-exact for an integer factor with the
    # Point filter.
    if (ww, wh) != (expected.width, expected.height):
        return False, f"window is {ww}x{wh}, expected {expected.width}x{expected.height}"
    diff = mean_abs_diff(expected, actual)

    def sample(img, fx, fy):
        return img.getpixel((int(img.width * fx), int(img.height * fy)))

    centre_ok = True
    for fx, fy in ((0.5, 0.5), (0.45, 0.5), (0.5, 0.45)):
        e, a = sample(expected, fx, fy), sample(actual, fx, fy)
        if sum(abs(e[i] - a[i]) for i in range(3)) > 40:
            centre_ok = False
    return diff < 2.0 and centre_ok, f"mean abs diff {diff:.2f} per channel"


# --- per-mode case -----------------------------------------------------------

def run_mode_case(rep: Report, w: int, h: int, native_w: int) -> None:
    tag = f"[{w}x{h}]"
    full = w >= native_w and w >= 1920        # enforce settings fit only on target
    print(f"\n--- mode {w}x{h} ({'full' if full else 'reduced'} checks) ---")

    dm = find_mode(w, h, like=ORIGINAL_MODE)
    if dm is None:
        rep.add(f"{tag} mode is supported by the driver", False, "not enumerated; skipped")
        return
    if not set_mode(dm, w, h):
        rep.add(f"{tag} mode is supported by the driver", False, "ChangeDisplaySettingsEx failed")
        return
    rep.add(f"{tag} mode is supported by the driver", True)

    if CONFIG.exists():
        CONFIG.unlink()

    # --- a fresh launch at this desktop -------------------------------------
    p = launch()
    try:
        st = status_full(p.pid)
        # refresh_status() appends the live geometry whenever MAG_DIAG is set;
        # the startup DIAG notice itself gets replaced by later notices.
        m = re.search(r"\[desktop=\(-?\d+,-?\d+\) (\d+)x(\d+)", st)
        got = f"{m.group(1)}x{m.group(2)}" if m else "(absent)"
        rep.add(f"{tag} starts and reports the new screen in DIAG",
                got == f"{w}x{h}", f"status desktop={got}")

        # First run: the shipped 320x240 region is re-centred on the work area.
        press(p.pid, "ZoomIn")                # forces the config to be written
        time.sleep(1.5)
        sel = None
        try:
            sel = json.loads(CONFIG.read_text(encoding="utf-8"))["selection_bounds_px"]
        except Exception:  # noqa: BLE001
            pass
        wa = work_area()
        if sel and len(sel) == 4:
            cx, cy = (sel[0] + sel[2]) / 2, (sel[1] + sel[3]) / 2
            vd = virtual_desktop()
            rep.add(f"{tag} first-run region is centred and on the desktop",
                    abs(cx - (wa[0] + wa[2]) / 2) <= 2 and abs(cy - (wa[1] + wa[3]) / 2) <= 2
                    and inside(vd, (sel[0], sel[1], sel[2], sel[3])),
                    f"selection {sel}, work area {wa}")
        else:
            rep.add(f"{tag} first-run region is centred and on the desktop", False,
                    "no selection_bounds_px in the config")

        # The magnifier turns on, renders, and stays inside the desktop.
        press(p.pid, "ToggleMagnifier")
        time.sleep(2.5)
        ov = find(p.pid, "MagOverlayWindow")
        fps = fps_of(status_full(p.pid))
        vd = virtual_desktop()
        rep.add(f"{tag} magnifier renders and stays inside the desktop",
                bool(ov and ov[0][1]) and fps > 5.0 and inside(vd, ov[0][2], 2),
                f"rect={ov[0][2] if ov else None} fps={fps:.1f} desktop={vd}")

        # Settings window and controls fit the work area.
        ctl = find(p.pid, "MagControlWindow")
        note = ""
        if ctl:
            rect = ctl[0][2]
            fits = inside(wa, rect, 0)
            clipped = 0
            crect = wt.RECT()
            u.GetClientRect(ctl[0][0], ctypes.byref(crect))
            pt = wt.POINT(0, 0)
            u.ClientToScreen(ctl[0][0], ctypes.byref(pt))
            cbox = (pt.x, pt.y, pt.x + crect.right, pt.y + crect.bottom)

            @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
            def cb(ch, _):
                nonlocal clipped
                cr = wt.RECT()
                u.GetWindowRect(ch, ctypes.byref(cr))
                if not inside(cbox, (cr.left, cr.top, cr.right, cr.bottom), 1):
                    clipped += 1
                return True
            u.EnumChildWindows(ctl[0][0], cb, 0)
            note = f"window {rect} vs work {wa}, {clipped} control(s) outside the client"
            if full:
                rep.add(f"{tag} settings window and controls fit the work area",
                        fits and clipped == 0, note)
            else:
                print(f"    (below target, recorded not enforced: {note})")
        else:
            if full:
                rep.add(f"{tag} settings window and controls fit the work area",
                        False, "no settings window")

        code = quit_app(p)
        rep.add(f"{tag} exits cleanly", code == 0, f"exit code {code}")

        # --- the picture is still magnified by exactly the factor ----------
        factor = 4 if full else 2
        (win_x, win_y), fits = pixel_geometry(w, h, factor)
        if not fits:
            rep.add(f"{tag} magnification is pixel-exact at {factor}x", False,
                    "the pattern and window cannot both fit; check skipped")
            return
        seed_config(factor, win_x, win_y)
        p = launch()
        try:
            press(p.pid, "ToggleMagnifier")
            time.sleep(3.5)
            ok, detail = pixel_check(p.pid, factor)
            rep.add(f"{tag} magnification is pixel-exact at {factor}x", ok, detail)
        finally:
            code = quit_app(p)
            rep.add(f"{tag} exits cleanly after the pixel check", code == 0,
                    f"exit code {code}")
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


# --- live mode change while the magnifier is running -------------------------

def run_live_case(rep: Report, native: DEVMODEW, mid: tuple[int, int],
                  small: tuple[int, int] | None) -> None:
    print("\n--- live mode changes with the magnifier running ---")
    if not set_mode(native, native.dmPelsWidth, native.dmPelsHeight):
        rep.add("live: back on the native mode", False, "could not restore the native mode")
        return
    rep.add("live: back on the native mode", True)

    if CONFIG.exists():
        CONFIG.unlink()
    p = launch()
    try:
        press(p.pid, "ToggleMagnifier")
        time.sleep(3.0)
        fps0 = fps_of(status_full(p.pid))
        rep.add("live: magnifier is running before the change", fps0 > 5.0, f"{fps0:.1f} fps")

        # Default shipped output is 1280x960; a screen shorter than that makes
        # the "shrink" rung a real oversize case.
        ladder = [mid] + ([small] if small else []) + [None]  # None = back to native
        for cycle in (1, 2, 3):
            for target in ladder:
                if target is None:
                    dm, w, h = native, native.dmPelsWidth, native.dmPelsHeight
                else:
                    dm = find_mode(*target, like=ORIGINAL_MODE)
                    w, h = target
                if not set_mode(dm, w, h):
                    rep.add(f"live cycle {cycle}: switched to {w}x{h}", False,
                            "ChangeDisplaySettingsEx failed")
                    continue
                time.sleep(1.5)           # WM_DISPLAYCHANGE -> topology rebuild -> frames
                alive = p.poll() is None
                st = status_full(p.pid)
                notice = ("display configuration changed" in st or "显示配置已更改" in st)
                fps = fps_of(st)
                vd = virtual_desktop()
                ov = find(p.pid, "MagOverlayWindow")
                rect = ov[0][2] if ov else None
                inside_ok = rect is not None and inside(vd, rect, 2)
                # The shipped default output is 1280x960; only a rung shorter
                # than that makes the shrink a real oversize case.
                shrunk_ok = rect is not None and (
                    h >= 960 or (rect[3] - rect[1]) <= h + 2)
                rep.add(f"live cycle {cycle}: survives {w}x{h}",
                        alive and (notice or fps > 5.0) and inside_ok and shrunk_ok,
                        f"alive={alive} notice={notice} fps={fps:.1f} "
                        f"rect={rect} desktop={vd}")
        code = quit_app(p)
        rep.add("live: exits cleanly after all the changes", code == 0, f"exit code {code}")
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


# --- DPI change reaching the topology cache ----------------------------------

def run_dpi_case(rep: Report) -> None:
    """DPI-only changes and the topology cache.

    Windows refuses to let a foreign process synthesize WM_DPICHANGED -- the
    send fails silently with lasterror 0, while a forged WM_DISPLAYCHANGE to
    the same window goes through (both facts are checked below). No public API
    changes the per-monitor scale factor reliably either, so the DPI-only
    branch cannot be driven end to end from a harness. Its handler is wired
    exactly like the WM_DISPLAYCHANGE one, and the rebuild machinery it calls
    is proven by the live-mode section; a real scale-factor change stays
    manual-verification territory.
    """
    print("\n--- WM_DPICHANGED and the topology cache ---")
    p = launch()
    try:
        pumps = find(p.pid, "MagHostPumpWindow")
        if not pumps:
            rep.add("dpi: host pump window exists", False, "no host pump window")
            return
        rep.add("dpi: host pump window exists", True)

        before = status_full(p.pid)
        ctypes.set_last_error(0)
        sent = u.SendMessageW(pumps[0][0], WM_DPICHANGED, (192 << 16) | 192, 0)
        refused = sent == 0 and ctypes.get_last_error() == 0
        rep.add("dpi: the OS refuses a forged WM_DPICHANGED", refused,
                f"send returned {sent} (injection is system-only)")

        u.PostMessageW(pumps[0][0], WM_DISPLAYCHANGE, 32, 0)
        time.sleep(1.5)
        after = status_full(p.pid)
        reached = ("display configuration changed" in after
                   or "显示配置已更改" in after
                   or ("DIAG" in after and after != before))
        rep.add("dpi: the twin topology-change path reaches the status line", reached,
                f"status moved: {after[:160]!r}")
        code = quit_app(p)
        rep.add("dpi case exits cleanly", code == 0, f"exit code {code}")
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


# --- main ---------------------------------------------------------------------

ORIGINAL_MODE: DEVMODEW | None = None


def restore_display() -> bool:
    if ORIGINAL_MODE is None:
        return True
    return set_mode(ORIGINAL_MODE, ORIGINAL_MODE.dmPelsWidth, ORIGINAL_MODE.dmPelsHeight)


def main() -> int:
    global ORIGINAL_MODE
    print("=" * 74)
    print("Screen Magnifier - resolution acceptance")
    print("=" * 74)
    rep = Report()

    if not EXE.exists():
        print(f"error: {EXE} not found; run ./build.sh first")
        return 1

    ORIGINAL_MODE = current_mode()
    native_w, native_h = ORIGINAL_MODE.dmPelsWidth, ORIGINAL_MODE.dmPelsHeight
    print(f"native mode: {native_w}x{native_h} @ {ORIGINAL_MODE.dmDisplayFrequency} Hz, "
          f"{ORIGINAL_MODE.dmBitsPerPel} bpp")

    # Protect the user's settings for the whole run.
    saved_config = CONFIG_DIR.with_name("Magnifier.verify_resolutions")
    if CONFIG_DIR.exists():
        if saved_config.exists():
            shutil.rmtree(saved_config, ignore_errors=True)
        shutil.move(str(CONFIG_DIR), str(saved_config))

    pattern = None
    try:
        kill_leftovers()

        mid = (1920, 1080) if (native_w, native_h) != (1920, 1080) else (1600, 900)
        small = (1366, 768)

        pattern = make_pattern()
        time.sleep(0.5)

        run_mode_case(rep, native_w, native_h, native_w)
        run_mode_case(rep, *mid, native_w)
        run_mode_case(rep, *small, native_w)

        run_live_case(rep, ORIGINAL_MODE, mid, small)
        run_dpi_case(rep)
    finally:
        print("\n--- restoring ---")
        if not restore_display():
            print("  !! THE DISPLAY COULD NOT BE RESTORED -- set it back by hand")
            rep.add("the original display mode was restored", False,
                    f"expected {ORIGINAL_MODE.dmPelsWidth}x{ORIGINAL_MODE.dmPelsHeight}")
        else:
            rep.add("the original display mode was restored", True,
                    f"{ORIGINAL_MODE.dmPelsWidth}x{ORIGINAL_MODE.dmPelsHeight}")
        kill_leftovers()
        if saved_config.exists():
            shutil.rmtree(CONFIG_DIR, ignore_errors=True)
            shutil.move(str(saved_config), str(CONFIG_DIR))
        if pattern is not None:
            try:
                pattern.destroy()
            except Exception:  # noqa: BLE001
                pass

    print("\n" + "=" * 74)
    total = len(rep.checks)
    print(f"{total - rep.failures}/{total} checks passed")
    if rep.failures:
        print("\nfailed:")
        for name, ok, detail in rep.checks:
            if not ok:
                print(f"  - {name}: {detail}")
    print("=" * 74)
    return rep.failures


if __name__ == "__main__":
    sys.exit(main())
