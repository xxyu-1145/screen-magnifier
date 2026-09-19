"""Click the English toggle in the settings window and screenshot the result.

Verifies the language switch end to end: the button, the retranslation of every
control, and the persisted setting.
"""
import ctypes
import ctypes.wintypes as wt
import json
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
LANG_ENGLISH_ID = 201


def make_dpi_aware() -> None:
    try:
        if u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except Exception:  # noqa: BLE001
        pass
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
    except Exception:  # noqa: BLE001
        pass


make_dpi_aware()


def children(pid, parent_cls):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        out.append(h)
        return True

    for h in windows(pid, parent_cls):
        u.EnumChildWindows(h, cb, 0)
    return out


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
                res.append(h)
        return True

    u.EnumWindows(cb, 0)
    return res


def texts(hwnds):
    out = []
    for h in hwnds:
        b = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, b, 256)
        if b.value:
            out.append(b.value)
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    if CONFIG.exists():
        CONFIG.unlink()          # start from the shipped default (Chinese)
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
        hwnd = windows(p.pid, "MagControlWindow")[0]
        before = texts(children(p.pid, "MagControlWindow"))
        chinese = sum(1 for t in before if any('一' <= c <= '鿿' for c in t))
        print(f"default language: {chinese} of {len(before)} labels contain Chinese")
        if chinese < 10:
            print("  FAIL: Chinese is not the default")
            failures += 1

        r = wt.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                       all_screens=True).convert("RGB").save(OUT / "settings_zh.png")

        # Click the English toggle: id 201, a direct child of the settings window.
        target = None
        for h in children(p.pid, "MagControlWindow"):
            if u.GetDlgCtrlID(h) == LANG_ENGLISH_ID:
                target = h
                break
        if target is None:
            print("  FAIL: the English toggle was not found")
            return 1
        u.SendMessageW(target, BM_CLICK, 0, 0)
        time.sleep(1.5)

        after = texts(children(p.pid, "MagControlWindow"))
        chinese_after = sum(1 for t in after if any('一' <= c <= '鿿' for c in t))
        print(f"after the toggle: {chinese_after} of {len(after)} labels contain Chinese")
        # One is correct and expected: the 「中文」 toggle, whose label is the name
        # of the language it selects. Anything more is a label that did not
        # follow -- the start/stop button and the status line's notices were
        # both left behind when this was first written.
        if chinese_after > 1:
            print("  FAIL: the interface did not switch to English")
            failures += 1
        else:
            print("  PASS: the interface switched to English")

        u.GetWindowRect(hwnd, ctypes.byref(r))
        ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                       all_screens=True).convert("RGB").save(OUT / "settings_en.png")

        time.sleep(1.5)
        if CONFIG.exists():
            cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
            lang = cfg.get("language")
            print(f"persisted language: {lang!r}")
            if lang != "en":
                print("  FAIL: the language was not persisted")
                failures += 1
            else:
                print("  PASS: the language was persisted")
        else:
            print("  FAIL: no config was written")
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
