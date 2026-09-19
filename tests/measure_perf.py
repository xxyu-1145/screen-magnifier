"""Measure the magnifier against the design document's performance gates.

Reports idle and running working set, CPU, thread/handle counts and present
frame rate. Idle is measured with the magnifier switched off, which is the
state the 50 MB budget actually refers to.

Run from the project root:  python tests/measure_perf.py
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")
psapi = ctypes.WinDLL("psapi")

EXE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "build", "magnifier.exe")
WM_HOTKEY = 0x0312
BASE = 0x4000
TOGGLE_MAGNIFIER = 0


class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


class TE(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


def thread_ids(pid):
    s = k.CreateToolhelp32Snapshot(0x4, 0)
    out = []
    te = TE()
    te.dwSize = ctypes.sizeof(te)
    if k.Thread32First(s, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            te.dwSize = ctypes.sizeof(te)
            if not k.Thread32Next(s, ctypes.byref(te)):
                break
    k.CloseHandle(s)
    return out


def mem(handle):
    c = PMC()
    c.cb = ctypes.sizeof(c)
    if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
        return 0.0, 0.0
    return c.WorkingSetSize / 1048576.0, c.PagefileUsage / 1048576.0


def cpu(handle):
    def t():
        cr, ex, ke, us = (wt.FILETIME() for _ in range(4))
        k.GetProcessTimes(handle, ctypes.byref(cr), ctypes.byref(ex),
                          ctypes.byref(ke), ctypes.byref(us))
        f = lambda ft: (ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7  # noqa: E731
        return f(ke) + f(us)
    return t


def handles(handle):
    c = wt.DWORD()
    k.GetProcessHandleCount(handle, ctypes.byref(c))
    return c.value


def find(pid, cls):
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


def status_text(pid):
    """The live status line, identified by its frame-rate readout.

    Located by the fps marker rather than an English prefix: the shipped
    interface is Chinese, so "state=" never appears.
    """
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(2048)
        u.GetWindowTextW(h, b, 2048)
        if " fps" in b.value:
            out.append(b.value.split("\n")[0])
        return True

    for h in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out[0] if out else ""


def sample(handle, seconds=4.0):
    """Return (cpu-percent-of-one-core, cpu-percent-of-all-cores, wall)."""
    t = cpu(handle)
    a = t()
    w0 = time.perf_counter()
    time.sleep(seconds)
    b = t()
    w1 = time.perf_counter()
    wall = max(w1 - w0, 1e-6)
    one = (b - a) / wall * 100.0
    return one, one / (os.cpu_count() or 1)


def main():
    p = subprocess.Popen([EXE])
    for _ in range(60):
        time.sleep(0.25)
        if find(p.pid, "MagControlWindow"):
            break
    else:
        print("app did not start")
        return 1
    h = k.OpenProcess(0x0400 | 0x1000, False, p.pid)
    time.sleep(3)

    rows = []
    idle_ws, idle_pv = mem(h)
    idle_one, idle_all = sample(h, 3.0)
    idle_thr, idle_hnd = len(thread_ids(p.pid)), handles(h)
    print(f"IDLE   ws={idle_ws:6.1f}MB  private={idle_pv:6.1f}MB  "
          f"cpu={idle_one:5.2f}% of one core ({idle_all:.2f}% total)  "
          f"threads={idle_thr}  handles={idle_hnd}")
    rows.append(("idle working set < 50 MB", idle_ws < 50.0, f"{idle_ws:.1f} MB"))
    rows.append(("idle CPU < 10% of one core", idle_one < 10.0, f"{idle_one:.2f}%"))

    for tid in thread_ids(p.pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + TOGGLE_MAGNIFIER, 0)
    time.sleep(3)

    run_ws, run_pv = mem(h)
    run_one, run_all = sample(h, 5.0)
    run_thr, run_hnd = len(thread_ids(p.pid)), handles(h)
    print(f"RUN    ws={run_ws:6.1f}MB  private={run_pv:6.1f}MB  "
          f"cpu={run_one:5.2f}% of one core ({run_all:.2f}% total)  "
          f"threads={run_thr}  handles={run_hnd}")
    print(f"status: {status_text(p.pid)}")
    rows.append(("running CPU < 10% of one core", run_one < 10.0, f"{run_one:.2f}%"))

    st = status_text(p.pid)
    fps = 0.0
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)\s+fps", st)
    if m:
        fps = float(m.group(1))
    print(f"       fps={fps:.0f}")

    for tid in thread_ids(p.pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + TOGGLE_MAGNIFIER, 0)
    time.sleep(3)
    off_ws, _ = mem(h)
    print(f"OFF    ws={off_ws:6.1f}MB  threads={len(thread_ids(p.pid))}")
    rows.append(("memory is released again when switched off", off_ws < 50.0, f"{off_ws:.1f} MB"))

    print()
    failed = 0
    for name, ok, detail in rows:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}  --  {detail}")
        failed += 0 if ok else 1

    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    return failed


if __name__ == "__main__":
    sys.exit(main())
