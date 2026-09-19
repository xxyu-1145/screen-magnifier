"""Drive the region picker: drag out a region, move it, and confirm it.

Opens the picker through the settings button, then checks the three gestures the
revision added or fixed:

  * dragging inside the region moves it rather than starting a new one,
  * the on-screen confirm button commits (this is the path that does not depend
    on the window holding keyboard focus), and
  * Enter still commits.

Run from the project root:  python tests/verify_picker.py
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

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
CONFIG = Path(os.environ.get("APPDATA", "")) / "Magnifier" / "config.json"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")
gdi = ctypes.WinDLL("gdi32")

BM_CLICK = 0x00F5
PICK_ID = 1300
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE = 0x0201, 0x0202, 0x0200
WM_LBUTTONDBLCLK = 0x0203
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_RETURN = 0x0D
MK_LBUTTON = 0x0001

u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


def windows(pid, cls, visible_only=False):
    res = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(o))
        if o.value == pid:
            b = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(h, b, 256)
            if b.value == cls:
                vis = bool(u.IsWindowVisible(h))
                if visible_only and not vis:
                    return True
                r = wt.RECT()
                u.GetWindowRect(h, ctypes.byref(r))
                res.append((h, vis, (r.left, r.top, r.right, r.bottom)))
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


def selection():
    try:
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
        return [int(v) for v in cfg["selection_bounds_px"]]
    except Exception:  # noqa: BLE001
        return None


def measure(text, face="Microsoft YaHei UI", height=-14):
    """Text extent, the same way the picker measures it."""
    dc = gdi.CreateCompatibleDC(None)
    font = gdi.CreateFontW(height, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0, face)
    old = gdi.SelectObject(dc, font)
    size = wt.SIZE()
    gdi.GetTextExtentPoint32W(dc, text, len(text), ctypes.byref(size))
    gdi.SelectObject(dc, old)
    gdi.DeleteObject(font)
    gdi.DeleteDC(dc)
    return size.cx, size.cy


def confirm_button_centre(client_w, client_h, hint, ok_text, cancel_text):
    """Where draw_hint_bar() puts the confirm button, in client coordinates."""
    hint_cx, hint_cy = measure(hint)
    ok_cx, _ = measure(ok_text)
    cancel_cx, _ = measure(cancel_text)

    pad, gap, button_pad = 16, 14, 22
    ok_w = max(84, ok_cx + 2 * button_pad)
    cancel_w = max(84, cancel_cx + 2 * button_pad)
    bar_h = max(48, hint_cy + 2 * pad)
    bar_w = pad + hint_cx + gap + ok_w + gap + cancel_w + pad
    bar_w = min(bar_w, client_w - 40)
    x = (client_w - bar_w) // 2
    y = client_h - bar_h - 36

    ok_right = x + bar_w - pad - cancel_w - gap
    ok_left = ok_right - ok_w
    button_y = y + 10
    button_h = bar_h - 2 * 10
    return (ok_left + ok_right) // 2, button_y + button_h // 2


def drag(hwnd, x0, y0, x1, y1, steps=6):
    u.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam(x0, y0))
    time.sleep(0.12)
    for i in range(1, steps + 1):
        mx = x0 + (x1 - x0) * i // steps
        my = y0 + (y1 - y0) * i // steps
        u.PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, lparam(mx, my))
        time.sleep(0.06)
    u.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam(x1, y1))
    time.sleep(0.5)


def open_picker(p, label):
    btn = find_child(p.pid, "MagControlWindow", PICK_ID)
    if btn is None:
        print(f"  FAIL: the pick-region button was not found ({label})")
        return None
    u.SendMessageW(btn, BM_CLICK, 0, 0)
    time.sleep(1.8)
    vis = windows(p.pid, "MagSelectionOverlayWindow", visible_only=True)
    if not vis:
        print(f"  FAIL: the picker did not open ({label})")
        return None
    return vis[0]


def main() -> int:
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

    failures = 0
    try:
        time.sleep(2.0)

        # --- 1. the confirm button commits --------------------------------
        print("[1] dragging a region and clicking the on-screen confirm button")
        entry = open_picker(p, "confirm")
        if entry is None:
            return 1
        ph, _, prect = entry
        cw, ch = prect[2] - prect[0], prect[3] - prect[1]
        drag(ph, 400, 300, 780, 620)
        bx, by = confirm_button_centre(cw, ch, "", "", "")
        # The hint string is only used for its width; measure the real one.
        bx, by = confirm_button_centre(
            cw, ch,
            "拖动框选 · 方向键微调 · 1-4 或 Tab 换形状 · Enter 确认 · Esc 取消",
            "确定", "取消")
        print(f"    confirm button at client ({bx},{by}) of {cw}x{ch}")
        u.PostMessageW(ph, WM_LBUTTONDOWN, MK_LBUTTON, lparam(bx, by))
        u.PostMessageW(ph, WM_LBUTTONUP, 0, lparam(bx, by))
        time.sleep(2.0)
        after = selection()
        closed = not windows(p.pid, "MagSelectionOverlayWindow", visible_only=True)
        print(f"    selection now {after}, picker closed: {closed}")
        if after == [400, 300, 780, 620] and closed:
            print("    PASS")
        else:
            print("    FAIL: the confirm button did not commit")
            failures += 1

        # --- 2. dragging inside moves the region --------------------------
        print("[2] dragging inside the region moves it")
        entry = open_picker(p, "move")
        if entry is None:
            return 1
        ph = entry[0]
        drag(ph, 500, 400, 700, 500)          # grab inside and move by (+200,+100)
        time.sleep(0.4)
        u.PostMessageW(ph, WM_KEYDOWN, VK_RETURN, 0)
        u.PostMessageW(ph, WM_KEYUP, VK_RETURN, 0)
        time.sleep(2.0)
        moved = selection()
        print(f"    selection now {moved}")
        if moved == [600, 400, 980, 720]:
            print("    PASS: the region moved without being redrawn")
        else:
            print("    FAIL: expected [600, 400, 980, 720]")
            failures += 1

        # --- 3. a double click inside confirms -----------------------------
        print("[3] double clicking inside the region confirms it")
        entry = open_picker(p, "dblclick")
        if entry is None:
            return 1
        ph = entry[0]
        drag(ph, 200, 200, 500, 460)
        inside = ((200 + 500) // 2, (200 + 460) // 2)
        u.PostMessageW(ph, WM_LBUTTONDBLCLK, MK_LBUTTON, lparam(*inside))
        time.sleep(2.0)
        confirmed = selection()
        closed = not windows(p.pid, "MagSelectionOverlayWindow", visible_only=True)
        print(f"    selection now {confirmed}, picker closed: {closed}")
        if confirmed == [200, 200, 500, 460] and closed:
            print("    PASS")
        else:
            print("    FAIL: the double click did not confirm")
            failures += 1
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    print("\n" + ("PASS" if failures == 0 else f"FAIL ({failures})"))
    return failures


if __name__ == "__main__":
    sys.exit(main())
