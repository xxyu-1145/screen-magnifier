"""Drive the magnifier through its own hotkey path and report what happened.

Input injection (SendInput/keybd_event) does not trigger RegisterHotKey on this
machine, so the OS detection step is replaced by posting WM_HOTKEY straight to
the app's input thread. Everything downstream of that -- the input thread's
handler, the event bus, the control pump, the state machine, the overlay window,
capture and the renderer -- is the real production path.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

EXE = r"D:\claude code\magnifier\build\magnifier.exe"
WM_HOTKEY = 0x0312
BASE = 0x4000
ACTIONS = {"ToggleMagnifier": 0, "TogglePassThrough": 1, "CycleShape": 2,
           "ZoomIn": 3, "ZoomOut": 4, "Preset1": 5, "Preset4": 8,
           "ResetSelection": 17, "Quit": 18}


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


def thread_ids(pid):
    snap = k.CreateToolhelp32Snapshot(0x4, 0)
    out = []
    if snap == -1:
        return out
    te = THREADENTRY32()
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
                r = wt.RECT()
                u.GetWindowRect(h, ctypes.byref(r))
                res.append((h, bool(u.IsWindowVisible(h)), (r.left, r.top, r.right, r.bottom),
                            u.GetWindowLongW(h, -20) & 0xFFFFFFFF))
        return True

    u.EnumWindows(cb, 0)
    return res


def texts(pid):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(2048)
        u.GetWindowTextW(h, b, 2048)
        if b.value:
            out.append(b.value)
        return True

    for h, _, _, _ in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out


def status(pid):
    for t in texts(pid):
        if t.startswith("state="):
            return t.split("\n")[0]
    return "(no status)"


def launch():
    p = subprocess.Popen([EXE])
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is not None:
            break
        if find(p.pid, "MagControlWindow"):
            time.sleep(0.6)
            return p
    raise SystemExit(f"app did not come up (pid={p.pid}, poll={p.poll()})")


def press(pid, name):
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + ACTIONS[name], 0)
    time.sleep(1.4)


def main():
    p = launch()
    print(f"pid={p.pid}  threads={len(thread_ids(p.pid))}")
    try:
        print("start :", status(p.pid))
        press(p.pid, "ToggleMagnifier")
        ov = find(p.pid, "MagOverlayWindow")
        vis = ov[0][1] if ov else None
        print(f"toggle: overlay visible={vis} rect={ov[0][2] if ov else None}")
        print("      :", status(p.pid))
        press(p.pid, "ZoomIn")
        print("zoomin:", status(p.pid))
        press(p.pid, "CycleShape")
        print("shape :", status(p.pid))
        press(p.pid, "ZoomOut")
        print("zoomou:", status(p.pid))
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
