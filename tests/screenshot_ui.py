"""Screenshot the settings window and the region picker."""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time
from pathlib import Path

from PIL import ImageGrab

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
OUT = ROOT / "build" / "ui"
CONFIG = Path(os.environ.get("APPDATA", "")) / "Magnifier" / "config.json"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

BM_CLICK = 0x00F5
PICK_ID = 1300

u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


def windows(pid, cls):
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


def find_child(pid, parent_cls, ctrl_id):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        if u.GetDlgCtrlID(h) == ctrl_id:
            found.append(h)
        return True

    for h, _, _ in windows(pid, parent_cls):
        u.EnumChildWindows(h, cb, 0)
    return found[0] if found else None


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    if CONFIG.exists():
        CONFIG.unlink()
    p = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is not None:
            print(f"app exited ({p.returncode})")
            return 1
        if windows(p.pid, "MagControlWindow"):
            break
    else:
        print("app never started")
        return 1

    try:
        time.sleep(2.0)
        hwnd = windows(p.pid, "MagControlWindow")[0][0]
        # Anything can be sitting on top of it; the capture is of the screen, so
        # the window has to actually be in front.
        u.SetForegroundWindow(hwnd)
        u.BringWindowToTop(hwnd)
        time.sleep(0.6)
        r = wt.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                       all_screens=True).convert("RGB").save(OUT / "settings.png")
        print("saved settings.png")

        # Open the picker and drag a region so the hint bar has company.
        btn = find_child(p.pid, "MagControlWindow", PICK_ID)
        if btn:
            u.SendMessageW(btn, BM_CLICK, 0, 0)
            time.sleep(1.5)
            picker = windows(p.pid, "MagSelectionOverlayWindow")
            if picker and picker[0][1]:
                ph = picker[0][0]
                u.PostMessageW(ph, 0x0201, 0x0001, lparam(700, 380))
                time.sleep(0.2)
                u.PostMessageW(ph, 0x0200, 0x0001, lparam(1250, 830))
                time.sleep(0.4)
                u.PostMessageW(ph, 0x0202, 0, lparam(1250, 830))
                time.sleep(0.8)
                shot = ImageGrab.grab(all_screens=True).convert("RGB")
                shot.save(OUT / "picker.png")
                print("saved picker.png")
            else:
                print("picker did not open")
        time.sleep(0.3)
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
