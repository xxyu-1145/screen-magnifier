"""Confirm that a built executable carries the icon and the manifest.

Loads the image as a data file -- which runs no code -- and asks the loader for
its resources, so this checks what the shell will see rather than what the
embedder thinks it wrote.

Run from the project root:  python tools/check_resources.py build/magnifier.exe
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
from pathlib import Path

k = ctypes.WinDLL("kernel32", use_last_error=True)
u = ctypes.WinDLL("user32", use_last_error=True)

LOAD_LIBRARY_AS_DATAFILE = 0x00000002
RT_ICON, RT_GROUP_ICON, RT_MANIFEST = 3, 14, 24
IMAGE_ICON = 1

k.LoadLibraryExW.restype = ctypes.c_void_p
k.LoadLibraryExW.argtypes = [wt.LPCWSTR, ctypes.c_void_p, wt.DWORD]
k.FreeLibrary.argtypes = [ctypes.c_void_p]
u.LoadImageW.restype = ctypes.c_void_p
u.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.UINT, ctypes.c_int, ctypes.c_int,
                         wt.UINT]
u.DestroyIcon.argtypes = [ctypes.c_void_p]

ENUM_PROC = ctypes.WINFUNCTYPE(wt.BOOL, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_void_p)


def resource_ids(module: int, type_id: int) -> list[object]:
    found: list[object] = []

    @ENUM_PROC
    def callback(_module, _type, name, _param):
        value = ctypes.cast(name, ctypes.c_void_p).value or 0
        found.append(value if value < 0x10000 else "named")
        return True

    # EnumResourceNames lives in kernel32, not user32.
    k.EnumResourceNamesW.restype = wt.BOOL
    k.EnumResourceNamesW(ctypes.c_void_p(module), ctypes.c_void_p(type_id), callback, None)
    return found


def main(argv: list[str]) -> int:
    path = Path(argv[1] if len(argv) > 1 else "build/magnifier.exe")
    if not path.exists():
        print(f"{path} not found")
        return 1

    module = k.LoadLibraryExW(str(path), None, LOAD_LIBRARY_AS_DATAFILE)
    if not module:
        print(f"could not load {path} (error {ctypes.get_last_error()})")
        return 1

    failures = 0
    try:
        icons = resource_ids(module, RT_ICON)
        groups = resource_ids(module, RT_GROUP_ICON)
        manifests = resource_ids(module, RT_MANIFEST)

        print(f"{path.name}: RT_ICON {icons}")
        print(f"{'':{len(path.name)}}  RT_GROUP_ICON {groups}")
        print(f"{'':{len(path.name)}}  RT_MANIFEST {manifests}")

        if not icons:
            print("  FAIL: no icon images embedded")
            failures += 1
        if 1 not in groups:
            print("  FAIL: no group icon with id 1 (what the app loads)")
            failures += 1
        if manifests != [1]:
            print("  FAIL: no manifest with id 1")
            failures += 1

        # The real question: does the loader hand back an icon?
        for size in (16, 32, 256):
            icon = u.LoadImageW(ctypes.c_void_p(module), ctypes.c_void_p(1), IMAGE_ICON, size,
                                size, 0)
            print(f"  LoadImage({size}x{size}) -> {'ok' if icon else 'FAILED'}")
            if icon:
                u.DestroyIcon(ctypes.c_void_p(icon))
            else:
                failures += 1
        # The question the user actually asks: does Explorer show it? That is
        # SHGetFileInfo, not the resource loader.
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)

        class SHFILEINFOW(ctypes.Structure):
            _fields_ = [("hIcon", ctypes.c_void_p), ("iIcon", ctypes.c_int),
                        ("dwAttributes", wt.DWORD), ("szDisplayName", wt.WCHAR * 260),
                        ("szTypeName", wt.WCHAR * 80)]

        SHGFI_ICON = 0x000000100
        SHGFI_LARGEICON = 0x000000000
        info = SHFILEINFOW()
        shell32.SHGetFileInfoW.restype = ctypes.c_void_p
        got = shell32.SHGetFileInfoW(str(path), 0, ctypes.byref(info), ctypes.sizeof(info),
                                     SHGFI_ICON | SHGFI_LARGEICON)
        print(f"  Explorer icon -> {'ok' if (got and info.hIcon) else 'FAILED'} "
              f"(index {info.iIcon})")
        if got and info.hIcon:
            u.DestroyIcon(info.hIcon)
        else:
            failures += 1
    finally:
        k.FreeLibrary(ctypes.c_void_p(module))

    print("\n" + ("PASS" if failures == 0 else f"FAIL ({failures})"))
    return failures


if __name__ == "__main__":
    sys.exit(main(sys.argv))
