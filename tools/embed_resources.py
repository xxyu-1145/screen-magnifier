"""Embed the icon and the application manifest into a built executable.

Why this exists: the toolchain has no resource compiler. zig ships clang and
MinGW-w64 but nothing that turns an .ico into an RT_GROUP_ICON, and its linker
rejects a .res file outright ("unknown file type"). So the resource section is
built here and grafted onto the executable.

The work is bounded and mechanical: append one section holding a resource
directory tree, then point the optional header's resource data directory at it.
Nothing already in the image moves, so the executable keeps working even if a
later step goes wrong -- the embedder writes to a temporary file and only
replaces the original once everything has succeeded.

Run from the project root:  python tools/embed_resources.py build/magnifier.exe
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14
RT_MANIFEST = 24

LANGUAGE = 0x0409  # en-US
ICON_GROUP_ID = 1  # app_icon.cpp loads this id
MANIFEST_ID = 1    # CREATEPROCESS_MANIFEST_RESOURCE_ID

IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_DIRECTORY_ENTRY_RESOURCE = 2

MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Magnifier" version="1.0.1.0"
                    processorArchitecture="*"/>
  <description>Screen Magnifier</description>

  <!-- Visual styles come from comctl32 v6, which the loader only binds when the
       process declares a dependency on it. -->
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
                        version="6.0.0.0" processorArchitecture="*"
                        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>

  <!-- The magnifier never needs elevation: it reads the desktop through the
       public capture APIs and never touches another process. -->
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>

  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>

  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <!-- Windows 10 and 11. -->
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
</assembly>
"""


def align_up(value: int, to: int) -> int:
    return (value + to - 1) // to * to


def read_ico(path: Path):
    blob = path.read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", blob, 0)
    if reserved != 0 or kind != 1:
        raise SystemExit(f"{path} is not an icon file")
    images = []
    offset = 6
    for _ in range(count):
        w, h, colours, _r, planes, bits, size, image_offset = struct.unpack_from(
            "<BBBBHHII", blob, offset
        )
        images.append((w, h, colours, planes, bits, blob[image_offset:image_offset + size]))
        offset += 16
    return images


def build_group_icon(images, first_id: int) -> bytes:
    """GRPICONDIR: the table that ties the individual images into one icon."""
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for index, (w, h, colours, planes, bits, data) in enumerate(images):
        out += struct.pack("<BBBBHHIH", w, h, colours, 0, planes or 1, bits, len(data),
                           first_id + index)
    return bytes(out)


def build_rsrc(base_rva: int, groups) -> bytes:
    """The .rsrc contents.

    Layout: the root directory, then one directory per resource type, then one
    per name, then the data entries, then the blobs -- each level referring to
    the next by offset from the start of the section.
    """
    root_off = 0
    cursor = 16 + 8 * len(groups)
    type_off = {}
    for type_id, items in groups:
        type_off[type_id] = cursor
        cursor += 16 + 8 * len(items)
    name_off = {}
    for type_id, items in groups:
        for name_id, _data in items:
            name_off[(type_id, name_id)] = cursor
            cursor += 16 + 8          # one language entry each
    entry_off = {}
    for type_id, items in groups:
        for name_id, _data in items:
            entry_off[(type_id, name_id)] = cursor
            cursor += 16              # IMAGE_RESOURCE_DATA_ENTRY
    blob_off = {}
    for type_id, items in groups:
        for name_id, data in items:
            blob_off[(type_id, name_id)] = cursor
            cursor = align_up(cursor + len(data), 4)

    out = bytearray(cursor)

    def put_directory(offset: int, entries) -> None:
        struct.pack_into("<IIHHHH", out, offset, 0, 0, 0, 0, 0, len(entries))
        at = offset + 16
        for entry_id, child, is_directory in entries:
            struct.pack_into("<II", out, at, entry_id,
                             child | (0x80000000 if is_directory else 0))
            at += 8

    put_directory(root_off, [(t, type_off[t], True) for t, _ in groups])
    for type_id, items in groups:
        put_directory(type_off[type_id],
                      [(n, name_off[(type_id, n)], True) for n, _ in items])
    for type_id, items in groups:
        for name_id, _data in items:
            put_directory(name_off[(type_id, name_id)],
                          [(LANGUAGE, entry_off[(type_id, name_id)], False)])
    for type_id, items in groups:
        for name_id, data in items:
            struct.pack_into("<IIII", out, entry_off[(type_id, name_id)],
                             base_rva + blob_off[(type_id, name_id)], len(data), 0, 0)
    for type_id, items in groups:
        for name_id, data in items:
            start = blob_off[(type_id, name_id)]
            out[start:start + len(data)] = data
    return bytes(out)


def embed(exe_path: Path, ico_path: Path) -> None:
    data = bytearray(exe_path.read_bytes())

    def u16(offset: int) -> int:
        return struct.unpack_from("<H", data, offset)[0]

    def u32(offset: int) -> int:
        return struct.unpack_from("<I", data, offset)[0]

    if data[0:2] != b"MZ":
        raise SystemExit(f"{exe_path} is not a PE image")
    pe = u32(0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{exe_path} has no PE signature")

    coff = pe + 4
    section_count = u16(coff + 2)
    optional_size = u16(coff + 16)
    optional = coff + 20
    magic = u16(optional)
    if magic != 0x20B:
        raise SystemExit("only 64-bit images are handled")

    section_alignment = u32(optional + 32)
    file_alignment = u32(optional + 36)
    size_of_image = u32(optional + 56)
    size_of_headers = u32(optional + 60)
    directories = optional + 112

    existing_rva = u32(directories + IMAGE_DIRECTORY_ENTRY_RESOURCE * 8)
    existing_size = u32(directories + IMAGE_DIRECTORY_ENTRY_RESOURCE * 8 + 4)
    if existing_rva != 0 or existing_size != 0:
        raise SystemExit(f"{exe_path} already has resources; nothing to do")

    # Append after the last section's raw data.
    end_of_data = 0
    sections = []
    for index in range(section_count):
        header = optional + optional_size + index * 40
        raw_size = u32(header + 16)
        raw_offset = u32(header + 20)
        sections.append(data[header:header + 8].rstrip(b"\0").decode("ascii", "replace"))
        end_of_data = max(end_of_data, raw_offset + raw_size)

    header_offset = optional + optional_size + section_count * 40
    if header_offset + 40 > size_of_headers:
        raise SystemExit("no room in the header for another section")

    images = read_ico(ico_path)
    first_icon_id = ICON_GROUP_ID + 1
    groups = [
        (RT_ICON, [(first_icon_id + i, image[5]) for i, image in enumerate(images)]),
        (RT_GROUP_ICON, [(ICON_GROUP_ID, build_group_icon(images, first_icon_id))]),
        (RT_MANIFEST, [(MANIFEST_ID, MANIFEST.encode("utf-8"))]),
    ]

    new_rva = align_up(size_of_image, section_alignment)
    rsrc = build_rsrc(new_rva, groups)
    raw_offset = align_up(end_of_data, file_alignment)
    raw_size = align_up(len(rsrc), file_alignment)

    struct.pack_into("<8sIIIIIIHHI", data, header_offset,
                     b".rsrc", len(rsrc), new_rva, raw_size, raw_offset,
                     0, 0, 0, 0, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ)
    struct.pack_into("<H", data, coff + 2, section_count + 1)
    struct.pack_into("<I", data, optional + 56,
                     align_up(new_rva + len(rsrc), section_alignment))
    struct.pack_into("<II", data, directories + IMAGE_DIRECTORY_ENTRY_RESOURCE * 8,
                     new_rva, len(rsrc))

    if len(data) < raw_offset:
        data += b"\0" * (raw_offset - len(data))
    data += rsrc
    if len(data) < raw_offset + raw_size:
        data += b"\0" * (raw_offset + raw_size - len(data))

    temporary = exe_path.with_suffix(".res-embed.tmp")
    temporary.write_bytes(bytes(data))
    temporary.replace(exe_path)
    print(f"embedded {len(images)} icons and the manifest into {exe_path.name} "
          f"(section {sections[0]!r}+, .rsrc {len(rsrc)} bytes at RVA {new_rva:#x})")


def main(argv: list[str]) -> int:
    if len(argv) < 2 or len(argv) > 3:
        print(__doc__)
        return 2
    exe_path = Path(argv[1])
    ico_path = Path(argv[2]) if len(argv) == 3 else Path("app.ico")
    if not exe_path.exists():
        raise SystemExit(f"{exe_path} not found")
    if not ico_path.exists():
        raise SystemExit(f"{ico_path} not found; run tests/make_icon.py first")
    embed(exe_path, ico_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
