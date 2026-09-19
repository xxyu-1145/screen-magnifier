"""Prove the shipped archive works when someone else unpacks it.

What gets sent is `dist/ScreenMagnifier-1.0.1.zip`, not this repository, so the
thing that has to be verified is the archive: unpacked somewhere with nothing
else in it, started from there, and driven far enough to show a magnified
picture.

Two checks earn their keep:

  * **the import table**, because a build can pass every other test on this
    machine and still be dead on the recipient's. Anything the executable
    imports that is not part of Windows is a library the recipient will have to
    supply and almost certainly will not. (The *loaded* module list is not the
    same question -- every process on a machine with an audio or input-method
    add-on has that vendor's DLLs in it, and they say nothing about this
    program.)
  * **a directory name with Chinese characters and a space in it**, because that
    is where a Chinese recipient will actually unpack it, and this toolchain
    links an ANSI entry point.

Run from the project root:  python tests/verify_package.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / "dist" / "ScreenMagnifier-1.0.1.zip"
CONFIG_DIR = Path(os.environ.get("APPDATA", "")) / "Magnifier"
# Where a Chinese recipient is most likely to unpack it.
UNPACK_AS = "屏幕放大镜 1.0.1"

u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32")

u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_HOTKEY = 0x0312
BASE = 0x4000
TOGGLE, QUIT = 0, 15


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
                res.append((h, bool(u.IsWindowVisible(h)),
                            (r.left, r.top, r.right, r.bottom)))
        return True

    u.EnumWindows(cb, 0)
    return res


def thread_ids(pid):
    class TE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                    ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]
    snap, out, te = k.CreateToolhelp32Snapshot(0x4, 0), [], TE()
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


def press(pid, action, settle=2.5):
    for tid in thread_ids(pid):
        u.PostThreadMessageW(tid, WM_HOTKEY, BASE + action, 0)
    time.sleep(settle)


def imported_dlls(exe: Path):
    """Every DLL named in the executable's import table."""
    data = exe.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\x00\x00":
        raise SystemExit("not a PE file")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", data, dd + 8)[0]

    secs = []
    for i in range(nsec):
        o = pe + 24 + optsize + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, vsize, rawptr, rawsize))

    def offset(rva):
        for vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    def cstr(off):
        return data[off:data.index(b"\x00", off)].decode("ascii")

    out, off = [], offset(imp_rva)
    while True:
        entry = struct.unpack_from("<IIIII", data, off)
        if not any(entry):
            break
        out.append(cstr(offset(entry[3])))
        off += 20
    return out


def main() -> int:
    failures = 0
    if not ARCHIVE.exists():
        print(f"{ARCHIVE} is missing; run ./build.sh package first")
        return 1

    work_root = Path(tempfile.mkdtemp(prefix="magpkg_"))
    work = work_root / UNPACK_AS
    saved_config = None
    try:
        # --- the archive -------------------------------------------------
        with zipfile.ZipFile(ARCHIVE) as z:
            names = z.namelist()
            corrupt = z.testzip()
            readme = z.read("README.txt") if "README.txt" in names else b""
        print(f"archive: {ARCHIVE.name}")
        print(f"  contents: {names}")
        if corrupt is not None:
            print(f"  FAIL: {corrupt} is corrupt inside the archive")
            failures += 1
        if "ScreenMagnifier.exe" not in names:
            print("  FAIL: the executable is not in the archive")
            failures += 1
        try:
            text = readme.decode("utf-8-sig")
            print(f"  readme: {len(text)} characters, opens as UTF-8")
            if len(text) < 200:
                print("  FAIL: the readme is too short to be the real one")
                failures += 1
        except UnicodeDecodeError:
            print("  FAIL: the readme is not valid UTF-8, so it will look like mojibake")
            failures += 1

        with zipfile.ZipFile(ARCHIVE) as z:
            z.extractall(work)
        exe = work / "ScreenMagnifier.exe"
        print(f"  unpacked into {work.name!r}")

        # --- nothing outside Windows is needed to start it ---------------
        #
        # `api-ms-win-*` and `ext-ms-*` are API set contracts: there is no such
        # file on disk anywhere, on this machine or any other, and the loader
        # resolves them to the DLL that implements them. Everything else has to
        # be a real file that ships with Windows -- a libstdc++ or libgcc from
        # the toolchain would show up here, and it would run on this machine
        # (where the toolchain is installed) and nowhere else.
        system32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
        imported = imported_dlls(exe)
        contract = [d for d in imported if d.lower().startswith(("api-ms-win-", "ext-ms-"))]
        concrete = [d for d in imported if d not in contract]
        missing = [d for d in concrete if not (system32 / d).exists()]
        print(f"  imports {len(imported)} DLLs: {len(concrete)} real files, "
              f"{len(contract)} API set contracts")
        for name in missing:
            print(f"    the recipient will not have: {name}")
        if missing:
            print("  FAIL: the executable needs a library Windows does not ship")
            failures += 1
        else:
            print(f"  PASS: all {len(concrete)} real imports ship with Windows, and the "
                  f"API sets resolve on any Windows 10 or 11")

        # --- a first run, so the defaults are what is being judged --------
        if CONFIG_DIR.exists():
            saved_config = CONFIG_DIR.with_name("Magnifier.verify_package")
            if saved_config.exists():
                shutil.rmtree(saved_config, ignore_errors=True)
            shutil.move(str(CONFIG_DIR), str(saved_config))

        p = subprocess.Popen([str(exe)], cwd=str(work))
        for _ in range(80):
            time.sleep(0.25)
            if p.poll() is not None:
                print(f"  FAIL: it exited during startup with code {p.returncode}")
                return 1
            if windows(p.pid, "MagControlWindow"):
                break
        else:
            print("  FAIL: it never opened its settings window")
            p.kill()
            return 1
        time.sleep(2.0)
        print("  it starts from a path with Chinese characters and a space")

        # --- it actually magnifies ----------------------------------------
        press(p.pid, TOGGLE)
        ov = windows(p.pid, "MagOverlayWindow")
        if not ov or not ov[0][1]:
            print("  FAIL: the magnifier window did not appear")
            failures += 1
        else:
            x0, y0, x1, y1 = ov[0][2]
            print(f"  the magnifier window is up: {x1-x0}x{y1-y0} at ({x0},{y0})")
            # It excludes itself from capture, so a screenshot of it shows what
            # is behind it; all that can be asked here is that it is not the
            # opaque black of a window with nothing in it.
            try:
                from PIL import ImageGrab
                win = ImageGrab.grab(all_screens=True).convert("RGB").crop((x0, y0, x1, y1))
                px = win.load()
                step = max(1, min(win.width, win.height) // 60)
                n = dark = 0
                for y in range(0, win.height, step):
                    for x in range(0, win.width, step):
                        n += 1
                        if max(px[x, y]) <= 8:
                            dark += 1
                share = dark / max(n, 1)
                print(f"  {share*100:.1f}% of it is near-black")
                if share > 0.9:
                    print("  FAIL: the magnifier window came up black")
                    failures += 1
            except ImportError as ex:  # noqa: BLE001
                print(f"  SKIP: {ex}")

        press(p.pid, QUIT, settle=1.0)
        try:
            code = p.wait(timeout=10)
            print(f"  it quits with code {code}")
            if code != 0:
                print("  FAIL: it did not exit cleanly")
                failures += 1
        except subprocess.TimeoutExpired:
            print("  FAIL: still running after the quit hotkey")
            p.kill()
            failures += 1
    finally:
        if saved_config is not None:
            shutil.rmtree(CONFIG_DIR, ignore_errors=True)
            shutil.move(str(saved_config), str(CONFIG_DIR))
        shutil.rmtree(work_root, ignore_errors=True)
        if 'p' in dir() and p.poll() is None:
            p.kill()

    print("\n" + ("PASS" if failures == 0 else f"FAIL: {failures} problem(s)"))
    return failures


if __name__ == "__main__":
    sys.exit(main())
