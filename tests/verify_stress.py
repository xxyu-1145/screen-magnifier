"""Stress the paths the adversarial review flagged.

1. Fifteen show/hide cycles. The review found a race where the render thread
   could die on the first hide because the capture session is torn down before
   the "off" snapshot is published, leaving a window in which acquire() rejects
   the call. The window is narrow, so one cycle proves nothing.

2. A WM_DISPLAYCHANGE broadcast, which must reach the host window and trigger a
   topology rebuild. The tray icon posts its menu selections through the same
   window and window procedure, so this also covers the path that made the tray
   menu inert.

Run from the project root:  python tests/verify_stress.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

WM_HOTKEY = 0x0312
WM_DISPLAYCHANGE = 0x007E
BASE = 0x4000
TOGGLE = 0
CYCLES = 15


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
                res.append(h)
        return True

    u.EnumWindows(cb, 0)
    return res


def status(pid: int) -> str:
    """The live status line, identified by its frame-rate readout.

    Located by the fps marker rather than an English prefix: the shipped
    interface is Chinese, so "state=" never appears and the counters that carry
    the verdict would be missed.
    """
    out: list[str] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(8192)
        u.GetWindowTextW(h, b, 8192)
        if " fps" in b.value:
            out.append(b.value.split("\n")[0])
        return True

    for h in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out[0] if out else ""


def frames_of(st: str) -> int:
    m = re.search(r"frames=(\d+)", st)
    return int(m.group(1)) if m else -1


def fps_of(st: str) -> float:
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)\s+fps", st)
    return float(m.group(1)) if m else 0.0


def press(pid: int, action: int) -> None:
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + action, 0)


def main() -> int:
    env = dict(os.environ)
    env["MAG_DIAG"] = "1"          # the counters live in the diagnostic suffix
    p = subprocess.Popen([str(EXE)], cwd=str(ROOT), env=env)
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is not None:
            print(f"app exited during startup ({p.returncode})")
            return 1
        if find(p.pid, "MagControlWindow"):
            break
    else:
        print("app never started")
        return 1

    failures = 0
    try:
        time.sleep(2.0)
        print(f"running {CYCLES} show/hide cycles...")
        last_frames = -1
        dead_at = None
        for cycle in range(1, CYCLES + 1):
            press(p.pid, TOGGLE)
            time.sleep(1.8)
            st = status(p.pid)
            frames = frames_of(st)
            fps = fps_of(st)
            if cycle == 1:
                last_frames = frames
            elif frames <= last_frames:
                print(f"  cycle {cycle:2d}: frames stalled at {frames} ({fps:.0f} fps)")
                dead_at = cycle
                break
            last_frames = frames
            press(p.pid, TOGGLE)     # hide again
            time.sleep(1.2)
            if cycle % 5 == 0:
                print(f"  cycle {cycle:2d}: frames={frames} fps={fps:.0f} still rendering")
        if dead_at is None:
            print(f"  the render loop survived all {CYCLES} cycles "
                  f"(frames {last_frames} and climbing)")
        else:
            print(f"  FAIL: the render loop died on cycle {dead_at}")
            print(f"        status: {status(p.pid)[:220]}")
            failures += 1

        # --- display change reaches the host window --------------------------
        press(p.pid, TOGGLE)
        time.sleep(1.5)
        hosts = find(p.pid, "MagHostPumpWindow")
        if not hosts:
            print("  FAIL: the host window is missing")
            failures += 1
        else:
            u.PostMessageW(hosts[0], WM_DISPLAYCHANGE, 32, 0)
            time.sleep(1.2)
            st = status(p.pid)
            reached = ("display configuration changed" in st
                       or "显示配置已更改" in st)
            print(f"  WM_DISPLAYCHANGE reached the host window: {reached}")
            if not reached:
                print(f"        status: {st[:220]}")
                failures += 1
        press(p.pid, 15)  # Quit (HotkeyAction::Quit == 15)
        time.sleep(1.0)
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    print("\n" + ("PASS: no failure" if failures == 0 else f"FAIL: {failures} problem(s)"))
    return failures


if __name__ == "__main__":
    sys.exit(main())
