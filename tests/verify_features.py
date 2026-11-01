"""Check the four things this revision changed, in one pass.

  1. Chinese is the shipped default.
  2. The magnifier window starts in the middle of the primary screen.
  3. The magnification ceiling is 10x.
  4. It still renders, and still quits cleanly.

Run from the project root:  python tests/verify_features.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
CONFIG = Path(os.environ.get("APPDATA", "")) / "Magnifier" / "config.json"
OUT = ROOT / "build" / "features"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

WM_HOTKEY = 0x0312
BASE = 0x4000
TOGGLE, ZOOM_IN, PRESET1, RESET_SELECTION, CENTER, QUIT = 0, 3, 5, 13, 14, 15
                                    # HotkeyAction indices; they move with the enum.
WM_HSCROLL = 0x0114

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


def thread_ids(pid):
    class TE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                    ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]
    snap = k.CreateToolhelp32Snapshot(0x4, 0)
    out = []
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


def child_texts(pid):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(4096)
        u.GetWindowTextW(h, b, 4096)
        if b.value:
            out.append(b.value.split("\n")[0])
        return True

    for h, _, _ in windows(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out


def status_line(pid):
    """The live status line, identified by its frame-rate readout."""
    for t in child_texts(pid):
        if " fps" in t:
            return t
    return ""


def saved_factor():
    """The factor the app last persisted, which is what it is really using."""
    try:
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
        return cfg["factor_q16"] / 65536.0
    except Exception:  # noqa: BLE001
        return 0.0


def press(pid, action, settle=1.4):
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + action, 0)
    time.sleep(settle)


def win(pid):
    res = windows(pid, "MagControlWindow")
    return res[0][0] if res else None


def find_child(pid, ctrl_id):
    """A child of the settings window, by control id."""
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        if u.GetDlgCtrlID(h) == ctrl_id:
            found.append(h)
        return True

    h = win(pid)
    if h:
        u.EnumChildWindows(h, cb, 0)
    return found[0] if found else None


def take_foreground(hwnd):
    """Bring a window forward the way a user does: by clicking it.

    SetForegroundWindow from a process that is not itself the foreground one is
    refused, and the refusal is silent -- the keystrokes then go to whatever
    window does hold the foreground, which reads as the program ignoring them.
    That is why the older checks here SKIPped every so often, and why a real
    click is worth the two extra lines: it is always granted.
    """
    r = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    u.SetCursorPos(r.left + 40, r.top + 8)
    time.sleep(0.2)
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.4)
    return u.GetForegroundWindow() == hwnd


def find_child_by_class(pid, cls):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(128)
        u.GetClassNameW(h, b, 128)
        if b.value == cls:
            found.append(h)
        return True

    h = win(pid)
    if h:
        u.EnumChildWindows(h, cb, 0)
    return found[0] if found else None



class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def tap(vk):
    """Type one key with SendInput, down then up.

    Real injected input, not a posted message: the preset fields only commit
    what the user changes, and a cross-process SetWindowText raises no EN_CHANGE
    at all.
    """
    for flags in (0, 2):
        one = INPUT()
        one.type = 1
        one.ki.wVk = vk
        one.ki.wScan = u.MapVirtualKeyW(vk, 0)
        one.ki.dwFlags = flags
        u.SendInput(1, ctypes.byref(one), ctypes.sizeof(INPUT))
        time.sleep(0.08)


def main() -> int:
    if CONFIG.exists():
        CONFIG.unlink()          # a clean default run
    failures = 0

    work = wt.RECT()
    u.SystemParametersInfoW(0x0030, 0, ctypes.byref(work), 0)   # SPI_GETWORKAREA
    print(f"primary work area: {work.right - work.left}x{work.bottom - work.top}")

    p = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is not None:
            print(f"app exited during startup ({p.returncode})")
            return 1
        if windows(p.pid, "MagControlWindow"):
            break
    else:
        print("app never started")
        return 1

    try:
        time.sleep(2.0)

        labels = child_texts(p.pid)
        chinese = sum(1 for t in labels if any('一' <= c <= '鿿' for c in t))
        print(f"\n[1] default language: {chinese} of {len(labels)} labels in Chinese")
        if chinese < 8:
            print("    FAIL: Chinese is not the default")
            failures += 1
        else:
            print("    PASS")

        press(p.pid, TOGGLE, settle=3.5)
        ov = windows(p.pid, "MagOverlayWindow")
        if not ov or not ov[0][1]:
            print("\n[2] FAIL: the magnifier window did not appear")
            return 1
        x0, y0, x1, y1 = ov[0][2]
        print(f"\n[2] magnifier window at ({x0},{y0}) {x1-x0}x{y1-y0}")
        dx = abs((x0 + x1) // 2 - (work.left + work.right) // 2)
        dy = abs((y0 + y1) // 2 - (work.top + work.bottom) // 2)
        print(f"    centre offset from the screen centre: {dx}px, {dy}px")
        if dx <= 2 and dy <= 2:
            print("    PASS: it starts in the middle of the screen")
        else:
            print("    FAIL: it does not start centred")
            failures += 1

        st = status_line(p.pid)
        print(f"    status: {st}")
        m = re.search(r"([0-9.]+) fps", st)
        fps = float(m.group(1)) if m else 0.0
        if fps < 5.0:
            print("    FAIL: it is not rendering")
            failures += 1
        else:
            print(f"    PASS: rendering at {fps:.0f} fps")

        for _ in range(14):
            press(p.pid, ZOOM_IN, settle=0.35)
        time.sleep(2.5)
        factor = saved_factor()
        print(f"\n[3] after 14 zoom-in steps the factor is {factor:.2f}x")
        if abs(factor - 10.0) < 0.01:
            print("    PASS: it stops at 10x")
        else:
            print("    FAIL: the ceiling is not 10x")
            failures += 1

        # --- 5. the zoom slider applies live ------------------------------
        print("\n[5] dragging the zoom slider applies the factor immediately")
        # The zoom slider is custom drawn, so it is found by control id and
        # driven with a synthetic press rather than a scroll-bar notification.
        bar = find_child(p.pid, 1010)
        if bar is None:
            print("    FAIL: the zoom slider was not found")
            failures += 1
        else:
            r = wt.RECT()
            u.GetClientRect(bar, ctypes.byref(r))
            thumb_r = r.bottom * 0.32
            # The control maps its track onto 100..1000 hundredths; 800 is 8.00x.
            t = (800 - 100) / 900.0
            x = int(thumb_r + t * (r.right - 2 * thumb_r))
            u.SendMessageW(bar, 0x0201, 0x0001, (r.bottom // 2) << 16 | (x & 0xFFFF))
            u.SendMessageW(bar, 0x0202, 0, (r.bottom // 2) << 16 | (x & 0xFFFF))
            time.sleep(2.0)
            slider_factor = saved_factor()
            print(f"    factor after dragging to 8.00x: {slider_factor:.2f}x")
            # The track is only a few hundred pixels wide for a 1.00-10.00 range,
            # so the integer click position the harness can ask for is worth a
            # couple of hundredths. What is being checked is that dragging
            # applies at all, not that a pixel maps to an exact factor.
            reach = 900.0 / max(1.0, r.right - 2 * thumb_r) / 100.0
            if abs(slider_factor - 8.0) <= max(0.05, 2 * reach):
                print("    PASS")
            else:
                print("    FAIL: the slider did not apply live")
                failures += 1

        # --- 6. restore defaults -------------------------------------------
        print("\n[6] the restore-defaults button resets everything")
        btn = find_child(p.pid, 1304)
        if btn is None:
            print("    FAIL: the restore-defaults button was not found")
            failures += 1
        else:
            u.SendMessageW(btn, 0x00F5, 0, 0)      # BM_CLICK
            time.sleep(2.5)
            cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
            factor = cfg["factor_q16"] / 65536.0
            sel = [int(v) for v in cfg["selection_bounds_px"]]
            print(f"    factor {factor:.2f}x, selection {sel}, "
                  f"shape {cfg['selection_shape']}, language {cfg['language']!r}")
            restored = (abs(factor - 4.0) < 0.01
                        and abs((sel[0] + sel[2]) // 2 - (work.left + work.right) // 2) <= 2
                        and abs((sel[1] + sel[3]) // 2 - (work.top + work.bottom) // 2) <= 2)
            if restored:
                print("    PASS: defaults restored, with the region centred again")
            else:
                print("    FAIL: the defaults were not restored")
                failures += 1

        # --- 7. the window survives being switched off and on ---------------
        #
        # With the shipped default the magnifier excludes itself from capture so
        # it cannot mirror itself, which means a screenshot of its own window is
        # *meant* to show whatever is behind it rather than the magnified image.
        # That makes "is it black?" the question worth asking rather than "does
        # it show the pattern?", and it needs a known backdrop to ask it: park a
        # solid colour exactly behind the window and compare the same crop
        # before and after the cycle. A window that comes back black fails, and
        # so does one that comes back showing anything other than what was there
        # before.
        print("\n[7] the magnifier window survives being switched off and on")
        try:
            from PIL import ImageGrab

            ov = windows(p.pid, "MagOverlayWindow")
            if not ov or not ov[0][1]:
                print("    FAIL: the magnifier window is not visible to test")
                failures += 1
            else:
                wx0, wy0, wx1, wy1 = ov[0][2]
                import tkinter as tk
                back = tk.Tk()
                back.overrideredirect(True)
                back.geometry(f"{wx1 - wx0}x{wy1 - wy0}+{wx0}+{wy0}")
                tk.Canvas(back, width=wx1 - wx0, height=wy1 - wy0,
                          highlightthickness=0, bg="#ff00ff").pack()
                back.update()
                time.sleep(1.5)

                def crop():
                    shot = ImageGrab.grab(all_screens=True).convert("RGB")
                    return shot.crop((wx0, wy0, wx1, wy1))

                def black_share(img):
                    px, step, n, dark = img.load(), None, 0, 0
                    step = max(1, min(img.width, img.height) // 60)
                    for y in range(0, img.height, step):
                        for x in range(0, img.width, step):
                            n += 1
                            if max(px[x, y]) <= 8:
                                dark += 1
                    return dark / max(n, 1)

                before = crop()
                press(p.pid, TOGGLE)          # off
                press(p.pid, TOGGLE)          # on again
                time.sleep(1.5)
                after = crop()
                back.destroy()
                # Kept so a mismatch can be looked at rather than guessed about.
                OUT.mkdir(parents=True, exist_ok=True)
                before.save(OUT / "offon_before.png")
                after.save(OUT / "offon_after.png")

                def mean_abs_diff(a, b):
                    pa, pb = a.load(), b.load()
                    step = max(1, min(a.width, a.height) // 48)
                    total, n = 0, 0
                    for y in range(0, a.height, step):
                        for x in range(0, a.width, step):
                            ca, cb = pa[x, y], pb[x, y]
                            total += (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1])
                                      + abs(ca[2] - cb[2]))
                            n += 3
                    return total / max(n, 1)

                dark = black_share(after)
                diff = mean_abs_diff(before, after)
                print(f"    before: {black_share(before)*100:.1f}% near-black; "
                      f"after: {dark*100:.1f}% near-black")
                print(f"    change across the cycle: {diff:.2f} per channel")
                if dark > 0.9:
                    print("    FAIL: the window came back black")
                    failures += 1
                elif diff > 12.0:
                    print("    FAIL: the window did not come back the same")
                    failures += 1
                else:
                    print("    PASS: it comes back showing what it showed before")
        except ImportError as ex:  # noqa: BLE001
            print(f"    SKIP: {ex}")

        # --- 8. the factor can be typed, not just dragged -------------------
        #
        # Driven with SendInput rather than by setting the text: the field only
        # commits what the user changes, and WM_SETTEXT from another process is
        # not that (it raises no EN_CHANGE). GetWindowText is no use for reading
        # the result either -- across processes it returns the window manager's
        # cached title, not an edit control's contents -- so the verdict comes
        # from the app's own state: it refits the window to the selection when
        # the factor changes, so a 320x240 region at 2.5x must read 800x600.
        print("\n[8] the magnification can be typed into its field")
        field = find_child(p.pid, 1012)
        settings = win(p.pid)
        if field is None or settings is None:
            print("    FAIL: the factor field was not found")
            failures += 1
        else:
            def output_size():
                m = re.search(r"输出\s+(\d+)x(\d+)", status_line(p.pid))
                return (int(m.group(1)), int(m.group(2))) if m else None

            # The keystrokes go wherever the focus is, so make sure that is the
            # field before typing into it and give up rather than type into
            # whatever else happens to be in front.
            take_foreground(settings)
            r = wt.RECT()
            u.GetClientRect(field, ctypes.byref(r))
            u.SendMessageW(field, 0x0201, 0x0001, 0)      # click to focus
            u.SendMessageW(field, 0x0202, 0, 0)
            time.sleep(0.5)

            if u.GetForegroundWindow() != settings:
                print("    SKIP: the settings window would not come to the front")
            else:
                base = output_size()
                print(f"    starting from output {base}")
                # A decimal, an out-of-range value that has to clamp to 1.00x,
                # and a bare integer with no decimal point.
                cases = [("2.5", (0x32, 0xBE, 0x35), (800, 600)),
                         ("0.2", (0x30, 0xBE, 0x32), (320, 240)),
                         ("3", (0x33,), (960, 720))]
                typed_ok = True
                for label, keys, want in cases:
                    u.SendMessageW(field, 0x00B1, 0, -1)          # EM_SETSEL: all
                    for vk in keys:
                        tap(vk)
                    # The field commits once typing pauses.
                    time.sleep(1.5)
                    got = output_size()
                    note = "" if got == want else f"   <- expected {want}"
                    print(f"    typed {label!r:7s} -> output {got}{note}")
                    if got != want:
                        typed_ok = False
                if typed_ok:
                    print("    PASS: a typed factor applies, and one out of range clamps")
                else:
                    print("    FAIL: the typed factor was not applied")
                    failures += 1

        # --- 9. the settings window is landscape and resizable --------------
        print("\n[9] the settings window is landscape and can be resized")
        cw = windows(p.pid, "MagControlWindow")
        if not cw:
            print("    FAIL: the settings window is missing")
            failures += 1
        else:
            ch, _, (cx0, cy0, cx1, cy1) = cw[0]
            style = u.GetWindowLongW(ch, -16)
            wide, tall = cx1 - cx0, cy1 - cy0
            resizable = bool(style & 0x00040000)              # WS_THICKFRAME
            # The previous arrangement was a single column that stood 1377px tall
            # on a 1380px work area, so "landscape" is a real claim here.
            print(f"    {wide}x{tall} on a {work.right - work.left}x"
                  f"{work.bottom - work.top} work area; WS_THICKFRAME: {resizable}")
            if wide <= tall:
                print("    FAIL: the window is not landscape")
                failures += 1
            elif tall > (work.bottom - work.top) * 3 // 4:
                print("    FAIL: the window is still nearly as tall as the screen")
                failures += 1
            elif not resizable:
                print("    FAIL: the window has no resize frame")
                failures += 1
            else:
                print("    PASS: landscape and resizable")

            def escaped_controls():
                """Children that land outside the client area, i.e. clipped."""
                outside = []

                @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
                def cb(h, _):
                    cr = wt.RECT()
                    u.GetClientRect(ch, ctypes.byref(cr))
                    r = wt.RECT()
                    u.GetWindowRect(h, ctypes.byref(r))
                    tl, br = wt.POINT(r.left, r.top), wt.POINT(r.right, r.bottom)
                    u.ScreenToClient(ch, ctypes.byref(tl))
                    u.ScreenToClient(ch, ctypes.byref(br))
                    if (tl.x < -1 or tl.y < -1
                            or br.x > cr.right + 1 or br.y > cr.bottom + 1):
                        outside.append(u.GetDlgCtrlID(h))
                    return True

                u.EnumChildWindows(ch, cb, 0)
                return outside

            def lowest_control():
                """How far down the window the controls reach, in client pixels."""
                lowest = 0

                @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
                def cb(h, _):
                    nonlocal lowest
                    r = wt.RECT()
                    u.GetWindowRect(h, ctypes.byref(r))
                    br = wt.POINT(r.right, r.bottom)
                    u.ScreenToClient(ch, ctypes.byref(br))
                    lowest = max(lowest, br.y)
                    return True

                u.EnumChildWindows(ch, cb, 0)
                return lowest

            before_low = lowest_control()
            # Growing needs room the desktop may not have: on a portrait
            # screen the settings window can already be nearly work-area wide,
            # and the system clamps a larger request to the max track size.
            # Width is optional for this check -- a taller window alone still
            # exercises the reflow, and there is usually vertical room.
            room_w = (work.right - work.left) - wide
            room_h = (work.bottom - work.top) - tall
            if room_h >= 120:
                target_w = wide + 320 if room_w >= 360 else wide
                target_h = tall + (90 if room_w >= 360 else 240)
                direction = "grow"
            else:
                target_w, target_h = max(600, wide - 260), max(400, tall - 90)
                direction = "shrink"
            u.SetWindowPos(ch, None, cx0, cy0, target_w, target_h,
                           0x0004 | 0x0010 | 0x0002)          # NOZORDER|NOACTIVATE|NOMOVE
            time.sleep(1.5)
            after = wt.RECT()
            u.GetWindowRect(ch, ctypes.byref(after))
            took = (abs((after.right - after.left) - target_w) <= 8
                    and abs((after.bottom - after.top) - target_h) <= 8)
            spill = escaped_controls()
            after_low = lowest_control()
            print(f"    {direction} to {after.right-after.left}x{after.bottom-after.top}; "
                  f"controls outside the client area: {len(spill)}")
            print(f"    controls reached {before_low}px down before, {after_low}px after")
            if not took:
                print("    FAIL: the window did not take the new size")
                failures += 1
            elif spill:
                print(f"    FAIL: resizing pushed controls out of view: {spill[:4]}")
                failures += 1
            elif after_low == before_low:
                # The complaint this check exists for: the window could be
                # resized and nothing inside it moved. Shrinking counts too --
                # the footer walks up the client area.
                print("    FAIL: a resized window left the arrangement unchanged")
                failures += 1
            else:
                print("    PASS: it resizes and the layout reflows to fit")
            u.SetWindowPos(ch, None, cx0, cy0, wide, tall, 0x0004 | 0x0010 | 0x0002)
            time.sleep(0.5)

        # --- 10. the window can be sent back to the middle ------------------
        print("\n[10] a hotkey puts the magnifier window back in the middle")
        ov = windows(p.pid, "MagOverlayWindow")
        if not ov or not ov[0][1]:
            print("    FAIL: the magnifier window is not visible")
            failures += 1
        else:
            x0, y0, x1, y1 = ov[0][2]
            drag_x, drag_y = (x0 + x1) // 2, (y0 + y1) // 2
            # A drag that does nothing is either the window refusing to move or
            # the input never reaching it, and the status line says which: it
            # reports the state machine's state, and a click-through window is
            # the one case where a press at its middle goes to whatever is
            # underneath instead.
            print(f"    before the drag: {status_line(p.pid)}")
            u.SetCursorPos(drag_x, drag_y)
            time.sleep(0.4)
            u.mouse_event(0x0002, 0, 0, 0, 0)          # LEFTDOWN
            for i in range(1, 13):
                u.SetCursorPos(drag_x - 30 * i, drag_y - 18 * i)
                time.sleep(0.05)
            u.mouse_event(0x0004, 0, 0, 0, 0)          # LEFTUP
            time.sleep(1.5)

            moved = windows(p.pid, "MagOverlayWindow")[0][2]
            press(p.pid, CENTER)
            back = windows(p.pid, "MagOverlayWindow")[0][2]
            print(f"    dragged to ({moved[0]},{moved[1]}), "
                  f"back to ({back[0]},{back[1]}) after the hotkey")
            if moved[0] == x0 and moved[1] == y0:
                print("    FAIL: the window could not be dragged, so nothing was tested")
                failures += 1
            else:
                bx = abs((back[0] + back[2]) // 2 - (work.left + work.right) // 2)
                by = abs((back[1] + back[3]) // 2 - (work.top + work.bottom) // 2)
                print(f"    it is now {bx}px, {by}px from the middle of the screen")
                if bx <= 2 and by <= 2:
                    print("    PASS: the hotkey puts it back in the middle")
                else:
                    print("    FAIL: the hotkey did not centre it")
                    failures += 1

        # --- 11. the window size has a drag bar of its own ------------------
        #
        # And resizing it moves neither its middle nor the thing it is showing:
        # a window that grew from its corner would slide across the screen while
        # the slider was being dragged.
        print("\n[11] the size slider resizes the window around its middle")
        size_bar = find_child(p.pid, 1035)
        if size_bar is None:
            print("    FAIL: the size slider was not found")
            failures += 1
        else:
            def window_rect():
                o = windows(p.pid, "MagOverlayWindow")
                return o[0][2] if o and o[0][1] else None

            cr = wt.RECT()
            u.GetClientRect(size_bar, ctypes.byref(cr))
            thumb = int(cr.bottom * 0.32)
            vw, vh = u.GetSystemMetrics(78), u.GetSystemMetrics(79)   # virtual screen
            slider_ok = True
            for frac in (0.50, 0.25):
                before = window_rect()
                sx = int(thumb + frac * (cr.right - 2 * thumb))
                lp = ((cr.bottom // 2) << 16) | (sx & 0xFFFF)
                u.SendMessageW(size_bar, 0x0201, 0x0001, lp)
                u.SendMessageW(size_bar, 0x0202, 0, lp)
                time.sleep(1.5)
                after = window_rect()
                was_w, now_w = before[2] - before[0], after[2] - after[0]
                dcx = abs((after[0] + after[2]) // 2 - (before[0] + before[2]) // 2)
                dcy = abs((after[1] + after[3]) // 2 - (before[1] + before[3]) // 2)
                # A window that has grown to touch the edge of the desktop cannot
                # keep its middle as well; that is the clamp doing its job, not
                # the resize moving the window.
                against_edge = (after[0] <= 0 or after[1] <= 0
                                or after[2] >= vw or after[3] >= vh)
                print(f"    dragged to {frac:.2f}: {was_w}px wide -> {now_w}px, "
                      f"middle moved {dcx}px, {dcy}px"
                      f"{' (against the screen edge)' if against_edge else ''}")
                if now_w == was_w:
                    slider_ok = False
                if not against_edge and (dcx > 2 or dcy > 2):
                    slider_ok = False
            if slider_ok:
                print("    PASS: it resizes, and the middle stays put")
            else:
                print("    FAIL: the slider did not resize in place")
                failures += 1

            # And the way back: "fit to source" is the window shape that shows
            # the whole region and nothing else, so it is a size the factor
            # already implies. The button sends an empty size for it, which the
            # resize range check used to reject -- so it did nothing at all.
            fit_button = find_child(p.pid, 1033)
            if fit_button is None:
                print("    FAIL: the fit-to-source button was not found")
                failures += 1
            else:
                u.SendMessageW(fit_button, 0x00F5, 0, 0)      # BM_CLICK
                time.sleep(1.8)
                m = re.search(r"选区\s+(\d+)x(\d+)", status_line(p.pid))
                factor = saved_factor()
                vw, vh = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
                want = ((min(int(int(m.group(1)) * factor), vw),
                         min(int(int(m.group(2)) * factor), vh)) if m else None)
                got = window_rect()
                got_size = (got[2] - got[0], got[3] - got[1]) if got else None
                print(f"    适配选区 resizes the window to {got_size} (the region at "
                      f"{factor:g}x is {want})")
                if want is not None and got_size == want:
                    print("    PASS: it fits the window to the region at the current factor")
                else:
                    print("    FAIL: fit to source did not restore the full region")
                    failures += 1

            # --- keep the ratio: one axis moves, the other follows ----------
            #
            # 保持比例 is a lock on the window's shape, so it has to hold for
            # every way the size can change -- a typed number and Apply, an
            # arrow hotkey, the slider, or an edge drag. Typing into one box
            # used to resize by exactly what was typed and leave the other axis
            # alone, which is what "the ratio is locked" is supposed to prevent.
            keep_box = find_child(p.pid, 1034)
            width_box = find_child(p.pid, 1030)
            height_box = find_child(p.pid, 1031)
            apply_button = find_child(p.pid, 1032)
            if None in (keep_box, width_box, height_box, apply_button):
                print("    FAIL: the ratio lock or the size fields were not found")
                failures += 1
            else:
                def click(hwnd):
                    u.SendMessageW(hwnd, 0x0201, 0x0001, 0)
                    u.SendMessageW(hwnd, 0x0202, 0, 0)

                u.SendMessageW(apply_button, 0x00F5, 0, 0)   # start from the fitted size
                time.sleep(1.2)
                before = window_rect()
                start = (before[2] - before[0], before[3] - before[1])

                take_foreground(win(p.pid))
                click(width_box)
                u.SendMessageW(width_box, 0x00B1, 0, -1)    # EM_SETSEL: all
                for vk in (0x38, 0x30, 0x30):               # "800"
                    tap(vk)
                time.sleep(0.4)
                click(apply_button)
                time.sleep(1.6)
                after = window_rect()
                got = (after[2] - after[0], after[3] - after[1])
                # The height the lock asks for, from the size it started at.
                ratio = start[1] / start[0] if start[0] else 0
                wanted = (800, int(round(800 * ratio)))
                print(f"    {start[0]}x{start[1]} -> typed 800 in 宽 -> {got} "
                      f"(x {ratio:.3f} is {wanted})")

                unlocked_ok = True
                # And with the lock off, the same edit moves only that axis --
                # otherwise "keep aspect" would be a label that does nothing.
                # BM_CLICK rather than a synthetic button-down/up: the
                # owner-drawn checkbox occasionally ignored the synthesized
                # pair, which read as the lock doing nothing.
                u.SendMessageW(keep_box, 0x00F5, 0, 0)       # BM_CLICK
                time.sleep(0.5)
                click(width_box)
                u.SendMessageW(width_box, 0x00B1, 0, -1)
                for vk in (0x39, 0x30, 0x30):               # "900"
                    tap(vk)
                time.sleep(0.4)
                click(apply_button)
                time.sleep(1.6)
                free = window_rect()
                free_size = (free[2] - free[0], free[3] - free[1])
                print(f"    lock off, typed 900 -> {free_size} "
                      f"(one axis only wants ({900}, {got[1]}))")
                unlocked_ok = free_size == (900, got[1])
                u.SendMessageW(keep_box, 0x00F5, 0, 0)       # BM_CLICK: back on
                time.sleep(0.5)

                # An oversized typed size clamps to the desktop rather than
                # being silently dropped: the controller used to reject it
                # downstream and Apply did nothing at all. Both axes clamp,
                # so the expectation is the ratio-preserving size that fits
                # the desktop (either axis can be the binding one).
                click(width_box)
                u.SendMessageW(width_box, 0x00B1, 0, -1)
                for _ in range(5):
                    tap(0x39)                                # "99999"
                time.sleep(0.4)
                click(apply_button)
                time.sleep(1.6)
                huge = window_rect()
                huge_size = (huge[2] - huge[0], huge[3] - huge[1])
                desk_w = u.GetSystemMetrics(78)              # SM_CXVIRTUALSCREEN
                desk_h = u.GetSystemMetrics(79)              # SM_CYVIRTUALSCREEN
                cur = free_size if unlocked_ok else got
                scale = min(desk_w / cur[0], desk_h / cur[1]) if cur[0] and cur[1] else 0
                fit = (int(round(cur[0] * scale)), int(round(cur[1] * scale)))
                clamped = (abs(huge_size[0] - fit[0]) <= 2
                           and abs(huge_size[1] - fit[1]) <= 2)
                print(f"    typed 99999 -> {huge_size} "
                      f"(the desktop is {desk_w}x{desk_h}; the ratio asks {fit})")
                if not clamped:
                    print("    FAIL: an oversized typed size did not clamp into the desktop")
                    failures += 1
                else:
                    print("    PASS: an oversized typed size clamps into the desktop")

                if got == wanted and unlocked_ok and clamped:
                    print("    PASS: the ratio is kept on one axis, and only when it is on")
                else:
                    print("    FAIL: the ratio lock did not do what it says")
                    failures += 1

        # --- 12. the preset factors can be typed ---------------------------
        #
        # The button jumps to the preset; the field under it says what the
        # preset is. Both have to move together, and so does the chord that
        # selects the same preset.
        print("\n[12] the preset factors can be edited")
        preset_button = find_child(p.pid, 1020)
        preset_field = find_child(p.pid, 1024)
        if preset_button is None or preset_field is None:
            print("    FAIL: the preset button or its field was not found")
            failures += 1
        else:
            def button_text(h):
                b = ctypes.create_unicode_buffer(64)
                u.GetWindowTextW(h, b, 64)
                return b.value

            def presets():
                try:
                    return json.loads(CONFIG.read_text(encoding="utf-8"))["presets"]
                except Exception:  # noqa: BLE001
                    return []

            def click(hwnd):
                u.SendMessageW(hwnd, 0x0201, 0x0001, 0)
                u.SendMessageW(hwnd, 0x0202, 0, 0)

            was = button_text(preset_button)
            settings = win(p.pid)
            take_foreground(settings)
            click(preset_field)
            time.sleep(0.4)
            u.SendMessageW(preset_field, 0x00B1, 0, -1)     # EM_SETSEL: all
            tap(0x35)                                       # the digit 5
            time.sleep(1.6)
            now = button_text(preset_button)
            saved_first = presets()[0] / 65536.0 if presets() else 0.0
            print(f"    button was {was!r}, is {now!r}; stored preset 1 is {saved_first:.2f}x")

            # And the chord that selects preset 1 has to use the new value: the
            # window refits to the selection, so 5x a 320-wide region is 1600.
            press(p.pid, PRESET1)
            m = re.search(r"输出\s+(\d+)x(\d+)", status_line(p.pid))
            out = (int(m.group(1)), int(m.group(2))) if m else None
            sel = re.search(r"(\d+)x(\d+) @", status_line(p.pid))
            want = (int(sel.group(1)) * 5, int(sel.group(2)) * 5) if sel else None
            print(f"    the preset-1 chord gives output {out} (expected {want})")
            if now == "5x" and abs(saved_first - 5.0) < 0.01 and out == want:
                print("    PASS: editing a preset moves the button, the file and the chord")
            else:
                print("    FAIL: the edited preset did not take")
                failures += 1

        # --- 13. regions can be kept and recalled --------------------------
        print("\n[13] a region can be kept and recalled")
        save_button = find_child(p.pid, 1054)
        slot1 = find_child(p.pid, 1050)
        slot2 = find_child(p.pid, 1051)
        if save_button is None or slot1 is None or slot2 is None:
            print("    FAIL: the kept-region buttons were not found")
            failures += 1
        else:
            def selection():
                m = re.search(r"选区\s+(\d+x\d+ @ \(-?\d+, -?\d+\))", status_line(p.pid))
                return m.group(1) if m else None

            def stored_slots():
                try:
                    return json.loads(CONFIG.read_text(encoding="utf-8"))["selection_slots"]
                except Exception:  # noqa: BLE001
                    return []

            original = selection()
            # Reset moves the region somewhere else entirely, so there is a
            # difference worth keeping.
            press(p.pid, RESET_SELECTION)
            moved = selection()
            u.SendMessageW(save_button, 0x00F5, 0, 0)      # BM_CLICK
            time.sleep(1.6)
            kept = stored_slots()[0]["bounds_px"] if stored_slots() else None
            print(f"    region was {original}, reset to {moved}, saved slot 1 as {kept}")

            # The gesture has to say something: the write target is the lit
            # button and it does not move when the region does, so a save with
            # everything already in place changes nothing on screen and reads as
            # the button being broken.
            line = status_line(p.pid)
            said = ("已保存到选区" in line) or ("saved to region" in line)
            print(f"    the status line names the slot it went to: {said}")
            if not said:
                print("    FAIL: saving gave no feedback about where the region went")
                failures += 1

            # Slot 2 still holds the shipped region, so recalling it has to put
            # the original back; slot 1 has to bring the saved one back.
            u.SendMessageW(slot2, 0x00F5, 0, 0)
            time.sleep(1.8)
            recalled2 = selection()
            u.SendMessageW(slot1, 0x00F5, 0, 0)
            time.sleep(1.8)
            recalled1 = selection()
            print(f"    slot 2 gives {recalled2}, slot 1 gives {recalled1}")
            if recalled2 == original and recalled1 == moved and kept is not None:
                print("    PASS: both kept regions come back intact")
            else:
                print("    FAIL: recalling a kept region did not restore it")
                failures += 1

        # --- 14. the language switch repaints, leaving nothing behind -------
        #
        # Owner-drawn controls compose into a bitmap and blit it whole. One that
        # paints only what it draws leaves the previous pixels alone, so the new
        # label lands on top of the old one -- which is how the English
        # interface came up with its checkboxes reading English *and* Chinese at
        # once. A screenshot cannot tell "correct" from "correct with leftovers"
        # by itself, so the comparison is against a fresh paint of the same
        # thing: relaunching, which comes up in the language just chosen.
        print("\n[14] switching language leaves nothing of the old one behind")
        try:
            from PIL import ImageGrab
        except ImportError as ex:  # noqa: BLE001
            print(f"    SKIP: {ex}")
        else:
            if windows(p.pid, "MagOverlayWindow") and \
                    windows(p.pid, "MagOverlayWindow")[0][1]:
                press(p.pid, TOGGLE)                 # out of the way of the crop

            def row_box():
                """Screen rect covering the captions of the two checkbox rows.

                Anchored at each control's own left edge and cut to a fixed
                width: the caption starts there in both runs, while the control
                itself is only as wide as its label needs, and a switched layout
                need not hand it the same width a fresh one does.
                """
                rects = []
                for ctrl in (1220, 1221):            # exclude capture, show border
                    h = find_child(p.pid, ctrl)
                    if h is None:
                        return None
                    r = wt.RECT()
                    u.GetWindowRect(h, ctypes.byref(r))
                    rects.append((r.left, r.top, r.right, r.bottom))
                left = min(r[0] for r in rects)
                return (left, min(r[1] for r in rects), left + 300,
                        max(r[3] for r in rects))

            box = row_box()
            english = find_child(p.pid, 201)
            if box is None or english is None:
                print("    FAIL: the language toggle or the checkbox rows were not found")
                failures += 1
            else:
                u.SendMessageW(english, 0x00F5, 0, 0)   # BM_CLICK
                time.sleep(2.0)
                switched = child_texts(p.pid)
                left = [t for t in switched
                        if any('一' <= c <= '鿿' for c in t) and t != "中文"]
                after = ImageGrab.grab(bbox=box, all_screens=True).convert("RGB")
                spill = escaped_controls()
                print(f"    after the switch: {len(left)} labels still in Chinese {left[:2]}; "
                      f"controls outside the client area: {len(spill)}")

                # Restart: the app comes up in English and paints these rows once.
                press(p.pid, QUIT, settle=1.0)
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
                p = subprocess.Popen([str(EXE)], cwd=str(ROOT))
                for _ in range(80):
                    time.sleep(0.25)
                    if windows(p.pid, "MagControlWindow"):
                        break
                time.sleep(2.5)
                fresh_box = row_box()
                if fresh_box is None:
                    print("    FAIL: the checkbox rows were not found after the restart")
                    failures += 1
                else:
                    fresh = ImageGrab.grab(bbox=fresh_box, all_screens=True).convert("RGB")
                    if fresh.size != after.size:
                        fresh = fresh.resize(after.size)
                    # Kept so a mismatch can be looked at rather than guessed
                    # about: the switched window beside one painted in that
                    # language from the start.
                    OUT.mkdir(parents=True, exist_ok=True)
                    after.save(OUT / "language_switched.png")
                    fresh.save(OUT / "language_fresh.png")
                    pa, pb = after.load(), fresh.load()
                    step = max(1, min(after.width, after.height) // 40)
                    total = n = 0
                    for y in range(0, after.height, step):
                        for x in range(0, after.width, step):
                            ca, cb = pa[x, y], pb[x, y]
                            total += abs(ca[0]-cb[0]) + abs(ca[1]-cb[1]) + abs(ca[2]-cb[2])
                            n += 3
                    diff = total / max(n, 1)
                    print(f"    against a fresh paint of the same labels: "
                          f"{diff:.2f} per channel")
                    if len(left) > 0:
                        print("    FAIL: a label did not follow the language")
                        failures += 1
                    elif spill:
                        print(f"    FAIL: the switched layout pushed controls out of view: "
                              f"{spill[:4]}")
                        failures += 1
                    elif diff > 3.0:
                        # Leftover glyphs make the switched window differ from
                        # one that was painted in that language from the start.
                        print("    FAIL: the window kept pixels of the old language")
                        failures += 1
                    else:
                        print("    PASS: the interface is in one language, freshly painted")

        # --- 16. a settings window shorter than its content scrolls ------------
        #
        # A work area shorter than the natural layout used to strand the
        # footer's buttons below the fold, with WM_GETMINMAXINFO forbidding the
        # only height that fit. The window now scrolls as one unit, and its
        # minimum height is a floor rather than the natural layout.
        print("\n[16] a settings window shorter than its content scrolls to its buttons")

        class MINMAXINFO(ctypes.Structure):
            _fields_ = [("ptReserved", wt.POINT), ("ptMaxSize", wt.POINT),
                        ("ptMaxPosition", wt.POINT), ("ptMinTrackSize", wt.POINT),
                        ("ptMaxTrackSize", wt.POINT)]

        class SCROLLINFO(ctypes.Structure):
            _fields_ = [("cbSize", wt.DWORD), ("fMask", wt.DWORD), ("nMin", ctypes.c_int),
                        ("nMax", ctypes.c_int), ("nPage", wt.UINT), ("nPos", ctypes.c_int),
                        ("nTrackPos", ctypes.c_int)]

        settings_h = win(p.pid)
        start_btn = find_child(p.pid, 1301)          # kIdStartStop
        if settings_h is None or start_btn is None:
            print("    FAIL: the settings window or the start button was not found")
            failures += 1
        else:
            orig = wt.RECT()
            u.GetWindowRect(settings_h, ctypes.byref(orig))
            mmi = MINMAXINFO()
            u.SendMessageW(settings_h, 0x0024, 0, ctypes.byref(mmi))    # WM_GETMINMAXINFO
            floor = mmi.ptMinTrackSize.y
            # Shrink to the smallest frame the window allows: the layout is
            # taller than that, so a scrollbar must appear.
            u.SetWindowPos(settings_h, None, 0, 0, orig.right - orig.left, floor,
                           0x0002 | 0x0004 | 0x0010)
            time.sleep(1.2)
            si = SCROLLINFO()
            si.cbSize = ctypes.sizeof(si)
            si.fMask = 0x17                         # SIF_ALL
            have = u.GetScrollInfo(settings_h, 1, ctypes.byref(si))     # SB_VERT
            scrollable = bool(have) and si.nMax > si.nPage
            print(f"    shrunk to the {floor}px floor: scroll range {si.nMin}..{si.nMax}, "
                  f"page {si.nPage}")
            # The footer lives at the bottom of the content: scroll there and
            # the start button has to be inside the client area.
            u.SendMessageW(settings_h, 0x0115, 7, 0)                    # WM_VSCROLL SB_BOTTOM
            time.sleep(0.8)
            btn = wt.RECT()
            u.GetWindowRect(start_btn, ctypes.byref(btn))
            pt = wt.POINT(btn.left + 2, btn.top + 2)
            u.ScreenToClient(settings_h, ctypes.byref(pt))
            client = wt.RECT()
            u.GetClientRect(settings_h, ctypes.byref(client))
            reachable = 0 <= pt.x < client.right and 0 <= pt.y < client.bottom
            # And the button really works from all the way down there.
            u.SendMessageW(start_btn, 0x00F5, 0, 0)                     # BM_CLICK
            time.sleep(2.0)
            ov = windows(p.pid, "MagOverlayWindow")
            shown = bool(ov) and ov[0][1]
            press(p.pid, TOGGLE, settle=1.2)                            # back off
            # Put the window back the way [9] left it.
            u.SetWindowPos(settings_h, None, 0, 0, orig.right - orig.left,
                           orig.bottom - orig.top, 0x0002 | 0x0004 | 0x0010)
            time.sleep(0.5)
            if scrollable and reachable and shown:
                print("    PASS: the scrollbar appears, the buttons are reachable, and they work")
            else:
                print(f"    FAIL (scrollbar={scrollable} reachable={reachable} works={shown})")
                failures += 1

        press(p.pid, QUIT, settle=1.0)
        try:
            code = p.wait(timeout=10)
            print(f"\n[4] exit code {code}")
            if code != 0:
                print("    FAIL")
                failures += 1
            else:
                print("    PASS")
        except subprocess.TimeoutExpired:
            print("\n[4] FAIL: still running after the quit hotkey")
            failures += 1
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    # --- 15. a corrupt config file says so, and heals -----------------------
    #
    # ConfigStore::last_error() used to have no reader: a file that could not
    # be opened or parsed silently reverted every setting to its default,
    # which reads to a user as "my settings vanished". The status line now
    # names the failure, and the first save rewrites valid JSON over the
    # damage.
    print("\n[15] a corrupt config file says so, and heals")
    CONFIG.write_text("{ this is not json", encoding="utf-8")
    p2 = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    try:
        for _ in range(80):
            time.sleep(0.25)
            if p2.poll() is not None:
                break
            if windows(p2.pid, "MagControlWindow"):
                break
        time.sleep(2.0)
        st = status_line(p2.pid)
        said = "配置文件无法读取" in st or "config file unreadable" in st
        print(f"    the status line names the failure: {said}")
        press(p2.pid, ZOOM_IN, settle=1.5)     # a hotkey forces a config save
        time.sleep(1.0)
        healed = False
        try:
            json.loads(CONFIG.read_text(encoding="utf-8"))
            healed = True
        except Exception:  # noqa: BLE001
            pass
        print(f"    the first save rewrites valid JSON: {healed}")
        press(p2.pid, QUIT, settle=1.0)
        try:
            code = p2.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p2.terminate()
            code = -1
        if not said or not healed or code != 0:
            print(f"    FAIL (said={said} healed={healed} exit={code}; status: {st[:140]})")
            failures += 1
        else:
            print("    PASS")
    finally:
        if p2.poll() is None:
            p2.terminate()

    print("\n" + "=" * 50)
    print("PASS" if failures == 0 else f"FAIL: {failures} problem(s)")
    return failures


if __name__ == "__main__":
    sys.exit(main())
