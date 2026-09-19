"""End-to-end acceptance test for the screen magnifier.

It launches the real executable and checks it against the design document's
acceptance gates: the idle and running resource budgets, that the magnifier
window really appears and really renders, that each hotkey reaches the right
subsystem, that settings persist, and that it shuts down cleanly.

A note on how input is driven. On this machine SendInput/keybd_event do **not**
trigger RegisterHotKey -- a bare Python self-test registering its own hotkey and
injecting the matching chord receives nothing -- so injecting keystrokes would
test the harness, not the product. Instead:

  * OS-level registration is verified independently by trying to register the
    same chords from this process while the app runs: failure with
    ERROR_HOTKEY_ALREADY_REGISTERED (1409) proves the app owns them.
  * Everything downstream of WM_HOTKEY (the input thread's translation, the
    event bus, the control pump, the state machine, capture and the renderer) is
    driven by posting WM_HOTKEY to the app's input thread, which is the same
    message the OS would deliver.

Run from the project root:  python tests/verify_runtime.py
Exit code is the number of failed checks.
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
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "magnifier.exe"
CONFIG = Path(os.environ.get("APPDATA", "")) / "Magnifier" / "config.json"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")
psapi = ctypes.WinDLL("psapi")

WM_HOTKEY = 0x0312
BASE = 0x4000
MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_NOREPEAT = 0x1, 0x2, 0x4, 0x4000
ERROR_HOTKEY_ALREADY_REGISTERED = 1409

# Indices into mag::HotkeyAction (core/events.h). A harness that posts a stale
# index does not fail quietly -- it drives the wrong action -- so this table has
# to move whenever the enum does.
ACTION = {"ToggleMagnifier": 0, "TogglePassThrough": 1, "CycleShape": 2, "ZoomIn": 3,
          "ZoomOut": 4, "Preset1": 5, "Preset2": 6, "Preset3": 7, "Preset4": 8,
          "GrowWidth": 9, "ShrinkWidth": 10, "GrowHeight": 11, "ShrinkHeight": 12,
          "ResetSelection": 13, "CenterOutput": 14, "Quit": 15}

WS_EX_TOPMOST = 0x00000008


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


@dataclass
class Report:
    checks: list[tuple[str, bool, str]] = field(default_factory=list)

    def add(self, name: str, passed: bool, detail: str = "") -> None:
        self.checks.append((name, passed, detail))
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}" + (f"  --  {detail}" if detail else ""))

    @property
    def failures(self) -> int:
        return sum(1 for _, ok, _ in self.checks if not ok)


# --- helpers -----------------------------------------------------------------

def thread_ids(pid: int) -> list[int]:
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


def find(pid: int, cls: str | None = None) -> list[tuple[int, bool, tuple, int]]:
    res: list[tuple[int, bool, tuple, int]] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(o))
        if o.value == pid:
            name = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(h, name, 256)
            if cls is None or name.value == cls:
                r = wt.RECT()
                u.GetWindowRect(h, ctypes.byref(r))
                res.append((h, bool(u.IsWindowVisible(h)),
                            (r.left, r.top, r.right, r.bottom), u.GetWindowLongW(h, -20) & 0xFFFFFFFF))
        return True

    u.EnumWindows(cb, 0)
    return res


def status(pid: int) -> str:
    """The live status line, identified by its frame-rate readout."""
    out: list[str] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        b = ctypes.create_unicode_buffer(4096)
        u.GetWindowTextW(h, b, 4096)
        if " fps" in b.value:
            out.append(b.value.split("\n")[0])
        return True

    for h, _, _, _ in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return out[0] if out else ""


def find_child(pid: int, ctrl_id: int):
    """The settings window's child with this control id, whatever its class."""
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        if u.GetDlgCtrlID(h) == ctrl_id:
            found.append(h)
        return True

    for h, _, _, _ in find(pid, "MagControlWindow"):
        u.EnumChildWindows(h, cb, 0)
    return found[0] if found else None


def state_of(st: str) -> str:
    """The interaction state, whichever language the interface is in."""
    if "鼠标穿透" in st or "click-through" in st:
        return "PassThrough"
    if "已关闭" in st or st.lstrip().startswith("off"):
        return "Off"
    if "边缘唤回" in st or "armed" in st:
        return "EdgeArmed"
    if "已暂停" in st or "suspended" in st:
        return "Suspended"
    if "可操作" in st or "interactive" in st:
        return "Interactive"
    return "?"


def saved(key: str, default=None):
    """A field from the configuration the app last wrote."""
    try:
        return json.loads(CONFIG.read_text(encoding="utf-8")).get(key, default)
    except Exception:  # noqa: BLE001
        return default


def field(st: str, key: str, default: str = "") -> str:
    """Read a `key=value` token out of the status line."""
    for part in st.split():
        if part.startswith(key + "="):
            return part.split("=", 1)[1]
    return default


def fps_of(st: str) -> float:
    """The status line renders the rate as `56 fps`, not `fps=56`."""
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)\s+fps", st)
    return float(m.group(1)) if m else 0.0


def press(pid: int, name: str) -> None:
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + ACTION[name], 0)
    time.sleep(1.3)


def mem(handle) -> tuple[float, float]:
    c = PMC()
    c.cb = ctypes.sizeof(c)
    psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb)
    return c.WorkingSetSize / 1048576.0, c.PagefileUsage / 1048576.0


def cpu_sample(handle, seconds: float) -> float:
    def used() -> float:
        cr, ex, ke, us = (wt.FILETIME() for _ in range(4))
        k.GetProcessTimes(handle, ctypes.byref(cr), ctypes.byref(ex),
                          ctypes.byref(ke), ctypes.byref(us))
        f = lambda ft: (ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7  # noqa: E731
        return f(ke) + f(us)
    a = used()
    t0 = time.perf_counter()
    time.sleep(seconds)
    b = used()
    return (b - a) / max(time.perf_counter() - t0, 1e-6) * 100.0


def owns_hotkey(mods: int, vk: int) -> bool:
    """True when another process (the app) already owns this chord."""
    ctypes.set_last_error(0)
    if u.RegisterHotKey(None, 0x7F00, mods, vk):
        u.UnregisterHotKey(None, 0x7F00)
        return False
    return ctypes.get_last_error() == ERROR_HOTKEY_ALREADY_REGISTERED


def launch():
    p = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is not None:
            raise SystemExit(f"the process exited during startup (code {p.returncode})")
        if find(p.pid, "MagControlWindow"):
            time.sleep(0.8)
            return p
    raise SystemExit("the app never created its settings window")


def main() -> int:
    print("=" * 74)
    print("Screen Magnifier - runtime acceptance")
    print("=" * 74)
    rep = Report()

    if not EXE.exists():
        print(f"error: {EXE} not found; run ./build.sh first")
        return 1
    if CONFIG.exists():
        CONFIG.unlink()

    p = launch()
    print(f"\nlaunched pid={p.pid}")
    h = k.OpenProcess(0x0400 | 0x1000, False, p.pid)

    try:
        # --- OS-level hotkey registration -----------------------------------
        time.sleep(0.5)
        owned = all(owns_hotkey(MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, vk)
                    for vk in (0x4D, 0x51, 0x53))  # M, Q, S
        free = not owns_hotkey(MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 0x5A)  # never bound
        rep.add("app registers its global hotkeys with the OS", owned and free,
                "Ctrl+Alt+M/Q/S owned, unbound Ctrl+Alt+Z still free")

        # --- idle budget -----------------------------------------------------
        time.sleep(3)
        idle_ws, idle_private = mem(h)
        idle_cpu = cpu_sample(h, 3.0)
        rep.add("idle working set < 50 MB", 0 < idle_ws < 50.0,
                f"{idle_ws:.1f} MB (private {idle_private:.1f} MB)")
        rep.add("idle CPU < 10% of one core", idle_cpu < 10.0, f"{idle_cpu:.2f}%")

        # --- switch it on ----------------------------------------------------
        press(p.pid, "ToggleMagnifier")
        st = status(p.pid)
        ov = find(p.pid, "MagOverlayWindow")
        visible = bool(ov and ov[0][1])
        topmost = bool(ov and (ov[0][3] & WS_EX_TOPMOST))
        rep.add("Ctrl+Alt+M shows the magnifier window", visible,
                f"state={state_of(st)} rect={ov[0][2] if ov else None}")
        rep.add("magnifier window is topmost", topmost,
                f"exstyle={ov[0][3]:#010x}" if ov else "window missing")

        if ov:
            w = ov[0][2][2] - ov[0][2][0]
            hh = ov[0][2][3] - ov[0][2][1]
            rep.add("window honours the 120x120 minimum", w >= 120 and hh >= 120, f"{w}x{hh}")

        # --- is it actually rendering? ---------------------------------------
        time.sleep(3)
        st = status(p.pid)
        measured = fps_of(st)
        rep.add("renderer is presenting frames", measured > 5.0, f"{measured:.1f} fps")

        # --- magnification ---------------------------------------------------
        # The factor lives in the saved configuration, which is what the app is
        # actually driving the renderer with.
        before = saved("factor_q16", 0) / 65536.0
        press(p.pid, "ZoomIn")
        after = saved("factor_q16", 0) / 65536.0
        rep.add("Ctrl+Alt+Plus raises the magnification", after > before,
                f"{before:.2f}x -> {after:.2f}x")

        press(p.pid, "Preset1")
        at_preset = saved("factor_q16", 0) / 65536.0
        rep.add("preset hotkey selects a configured factor", abs(at_preset - 2.0) < 0.01,
                f"preset 1 -> {at_preset:.2f}x")

        # --- shape -----------------------------------------------------------
        shape0 = saved("selection_shape")
        size0 = saved("selection_bounds_px")
        press(p.pid, "CycleShape")
        shape1 = saved("selection_shape")
        size1 = saved("selection_bounds_px")
        rep.add("Ctrl+Alt+S cycles the selection shape",
                shape1 not in (None, shape0), f"{shape0} -> {shape1} (bounds {size0} -> {size1})")

        # --- output window resize --------------------------------------------
        w0 = find(p.pid, "MagOverlayWindow")
        press(p.pid, "GrowWidth")
        w1 = find(p.pid, "MagOverlayWindow")
        grew = bool(w0 and w1 and (w1[0][2][2] - w1[0][2][0]) > (w0[0][2][2] - w0[0][2][0]))
        rep.add("Ctrl+Alt+Right widens the output window", grew,
                f"width {(w0[0][2][2]-w0[0][2][0]) if w0 else 0} -> "
                f"{(w1[0][2][2]-w1[0][2][0]) if w1 else 0}")

        # --- running budget ---------------------------------------------------
        run_ws, run_private = mem(h)
        run_cpu = cpu_sample(h, 5.0)
        rep.add("running CPU < 10% of one core", run_cpu < 10.0,
                f"{run_cpu:.2f}% (working set {run_ws:.1f} MB, private {run_private:.1f} MB)")

        # --- pass-through -----------------------------------------------------
        press(p.pid, "TogglePassThrough")
        st_pass = status(p.pid)
        alive = bool(find(p.pid, "MagOverlayWindow"))
        rep.add("Ctrl+Alt+P switches to pass-through and keeps the window",
                alive and state_of(st_pass) == "PassThrough",
                f"state={state_of(st_pass)}")
        press(p.pid, "TogglePassThrough")
        st_back = status(p.pid)
        rep.add("the same hotkey restores interactive mode",
                state_of(st_back) == "Interactive", f"state={state_of(st_back)}")

        # --- settings persistence ---------------------------------------------
        time.sleep(1.5)
        if CONFIG.exists():
            try:
                data = json.loads(CONFIG.read_text(encoding="utf-8"))
                ok = isinstance(data, dict) and "hotkeys" in data
                rep.add("settings persist to JSON with the hotkey table", ok,
                        f"{sorted(data)[:6]}... at {CONFIG.name}")
            except Exception as exc:  # noqa: BLE001
                rep.add("settings persist to JSON with the hotkey table", False, str(exc))
        else:
            rep.add("settings persist to JSON with the hotkey table", False,
                    f"{CONFIG} was never written")

        # --- switch off --------------------------------------------------------
        press(p.pid, "ToggleMagnifier")
        off_state = state_of(status(p.pid))
        time.sleep(2.5)
        off_ws, _ = mem(h)
        rep.add("switching off hides the window and releases memory",
                off_state == "Off" and off_ws < 50.0, f"state={off_state}, {off_ws:.1f} MB")

        # --- shutdown ----------------------------------------------------------
        press(p.pid, "Quit")
        try:
            code = p.wait(timeout=10)
            rep.add("Ctrl+Alt+Q exits cleanly", code == 0, f"exit code {code}")
        except subprocess.TimeoutExpired:
            rep.add("Ctrl+Alt+Q exits cleanly", False, "still running after 10 s")

    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        if h:
            k.CloseHandle(h)

    # --- a chord another application owns is taken over, not lost ------------
    #
    # The likeliest reason to conclude the program does not work at all is a
    # hotkey that silently does nothing because something else got there first.
    # This takes the chord before the app can have it and then types it: the
    # magnifier has to come up anyway, driven by the low-level fallback.
    HOTKEY_ID = 0xBEEF
    VK_M = 0x4D

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

    def tap(vk, up=False):
        one = INPUT()
        one.type = 1
        one.ki.wVk = vk
        one.ki.wScan = u.MapVirtualKeyW(vk, 0)
        one.ki.dwFlags = 2 if up else 0
        u.SendInput(1, ctypes.byref(one), ctypes.sizeof(INPUT))
        time.sleep(0.06)

    def overlay_visible(pid):
        ov = find(pid, "MagOverlayWindow")
        return ov[0][2] if ov and ov[0][1] else None

    if not u.RegisterHotKey(None, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_M):
        rep.add("a chord owned by another app still works", False,
                "the harness could not take the chord first, so nothing was tested")
    else:
        p2 = subprocess.Popen([str(EXE)], cwd=str(ROOT))
        try:
            for _ in range(80):
                time.sleep(0.25)
                if find(p2.pid, "MagControlWindow"):
                    break
            time.sleep(2.5)
            st = status(p2.pid)
            took_over = "接管" in st or "taken over" in st
            before = overlay_visible(p2.pid)

            ctl = find(p2.pid, "MagControlWindow")
            if ctl:
                u.SetForegroundWindow(ctl[0][0])
            time.sleep(0.4)
            tap(0xA2)                 # LCtrl
            tap(0xA4)                 # LAlt
            tap(VK_M)
            tap(VK_M, up=True)
            tap(0xA4, up=True)
            tap(0xA2, up=True)
            time.sleep(3.0)
            after = overlay_visible(p2.pid)

            rep.add("a chord owned by another app still works",
                    took_over and before is None and after is not None,
                    f"notice={took_over}, hidden before={before is None}, "
                    f"shown after={after is not None}")
            press(p2.pid, "Quit")
            try:
                p2.wait(timeout=8)
            except subprocess.TimeoutExpired:
                p2.terminate()
        finally:
            if p2.poll() is None:
                p2.terminate()
            u.UnregisterHotKey(None, HOTKEY_ID)

    # --- a chord the program already owns can be rebound ---------------------
    #
    # Two separate faults made rebinding look broken, and neither was visible in
    # the field: pressing Ctrl committed a chord of its own (the keystroke for
    # Ctrl arrives with Ctrl already down, so the capture ended on "Ctrl+VK_11"
    # before the letter was ever reached), and a chord the program holds is
    # consumed by RegisterHotKey, so it never reached the field -- the action it
    # already belonged to fired instead. Both are checked here by typing a chord
    # the app owns into a field and requiring the new binding to come out, the
    # old registration to be released while the field is armed, and everything
    # to be back in force afterwards.
    def click_to_foreground(hwnd) -> bool:
        """Bring a window forward the way a user does: by clicking it.

        SetForegroundWindow from a process that is not itself in the foreground
        is refused, and a refused call is silent -- the keystrokes then land in
        whatever window does hold it, which reads as the program ignoring them.
        A real click is always granted.
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

    def field_text(hwnd) -> str:
        buf = ctypes.create_unicode_buffer(256)
        u.SendMessageW.restype = ctypes.c_ssize_t
        u.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_void_p]
        u.SendMessageW(hwnd, 0x000D, 256, buf)      # WM_GETTEXT
        return buf.value

    def click_control(hwnd) -> None:
        u.SendMessageW.restype = ctypes.c_ssize_t
        u.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_void_p]
        u.SendMessageW(hwnd, 0x0201, 1, None)       # WM_LBUTTONDOWN
        u.SendMessageW(hwnd, 0x0202, 0, None)       # WM_LBUTTONUP

    if CONFIG.exists():
        CONFIG.unlink()
    p3 = subprocess.Popen([str(EXE)], cwd=str(ROOT))
    try:
        for _ in range(80):
            time.sleep(0.25)
            if find(p3.pid, "MagControlWindow"):
                break
        time.sleep(2.0)

        settings = find(p3.pid, "MagControlWindow")[0][0]
        foreground = click_to_foreground(settings)
        owns_m = lambda: owns_hotkey(MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_M)  # noqa: E731
        owned_before = owns_m()
        was_hidden = overlay_visible(p3.pid) is None

        # ShrinkWidth is the "narrower" action; give it the chord Ctrl+Alt+M that
        # Show/hide currently owns.
        field_hwnd = find_child(p3.pid, 1100 + ACTION["ShrinkWidth"])
        clicked = field_hwnd is not None
        armed_text = ""
        released = False
        hinted = False
        if clicked:
            click_control(field_hwnd)
            time.sleep(0.6)
            armed_text = field_text(field_hwnd)
            released = not owns_m()
            tap(0x4A)                       # a bare 'J': refused, and said so
            time.sleep(0.8)
            hinted = field_text(field_hwnd) not in ("", armed_text)

        tap(0xA2)                           # LCtrl
        tap(0xA4)                           # LAlt
        tap(VK_M)
        tap(VK_M, up=True)
        tap(0xA4, up=True)
        tap(0xA2, up=True)
        time.sleep(1.8)

        stored = None
        try:
            stored = json.loads(CONFIG.read_text(encoding="utf-8"))["hotkeys"]["ShrinkWidth"]
        except Exception:  # noqa: BLE001
            pass
        rebound = stored == [MOD_CONTROL | MOD_ALT, VK_M]
        owns_after = owns_m()
        still_hidden = overlay_visible(p3.pid) is None

        rep.add("a chord the program already owns can be rebound",
                foreground and owned_before and clicked and released and rebound
                and owns_after and was_hidden and still_hidden,
                f"foreground={foreground} owned={owned_before} field={clicked} "
                f"released while armed={released} rebound={stored} "
                f"re-registered={owns_after}, the old action stayed put={still_hidden}")
        rep.add("a key that needs a modifier is refused, and says so",
                hinted, f"the field went from {armed_text!r} to a refusal notice")

        press(p3.pid, "Quit")
        try:
            p3.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p3.terminate()
    finally:
        if p3.poll() is None:
            p3.terminate()

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
