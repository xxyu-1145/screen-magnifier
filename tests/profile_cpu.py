"""Attribute the magnifier's CPU use to a component.

Measures the same process in three states -- idle, magnifying with the settings
window open, and magnifying with it hidden -- so a cost that belongs to the
settings window can be told apart from one that belongs to the capture and
render path.

Run from the project root:  python tests/profile_cpu.py
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

WM_HOTKEY = 0x0312
BASE = 0x4000
TOGGLE = 0
SW_HIDE = 0

u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


def windows(pid: int, cls: str):
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


def cpu_percent(handle, seconds: float) -> float:
    def used() -> float:
        cr, ex, ke, us = (wt.FILETIME() for _ in range(4))
        k.GetProcessTimes(handle, ctypes.byref(cr), ctypes.byref(ex), ctypes.byref(ke),
                          ctypes.byref(us))
        f = lambda ft: (ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7  # noqa: E731
        return f(ke) + f(us)
    a = used()
    t0 = time.perf_counter()
    time.sleep(seconds)
    b = used()
    return (b - a) / max(time.perf_counter() - t0, 1e-6) * 100.0


def press(pid: int, action: int) -> None:
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + action, 0)


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

    h = k.OpenProcess(0x0400 | 0x0010, False, p.pid)
    try:
        time.sleep(2.5)
        print(f"idle (magnifier off)                 : {cpu_percent(h, 6.0):6.2f}% of one core")

        press(p.pid, TOGGLE)
        time.sleep(3.0)
        print(f"magnifying, settings window visible  : {cpu_percent(h, 8.0):6.2f}% of one core")

        for hwnd in windows(p.pid, "MagControlWindow"):
            u.ShowWindow(hwnd, SW_HIDE)
        time.sleep(2.0)
        print(f"magnifying, settings window hidden   : {cpu_percent(h, 8.0):6.2f}% of one core")

        # And with capture running but nothing drawn, to separate capture from
        # presentation.
        if CONFIG.exists():
            cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
            print(f"   (config: target_fps={cfg.get('target_fps')}, "
                  f"filter={cfg.get('scale_filter')})")
        else:
            print("   (no config written yet; defaults are in force)")
        press(p.pid, TOGGLE)
        time.sleep(2.5)
        print(f"idle again (magnifier off)           : {cpu_percent(h, 6.0):6.2f}% of one core")
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        if h:
            k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
