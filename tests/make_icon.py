"""Render the application icon to a multi-resolution .ico.

The app draws the same mark at runtime (src/platform/app_icon.cpp) because this
toolchain cannot embed a resource, but having the file is useful for a shortcut,
an installer, or anywhere else that wants an icon on disk. If `app.ico` sits
next to the executable, the app loads it in preference to drawing its own, so
replacing it needs no rebuild.

Run from the project root:  python tests/make_icon.py
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "app.ico"
SIZES = [16, 24, 32, 48, 64, 128, 256]

# The same geometry the runtime painter uses, as fractions of the tile.
LENS_CX, LENS_CY, LENS_R = 0.415, 0.415, 0.235
RING_W, HANDLE_W = 0.092, 0.085
HANDLE_LEN = 0.205


def lerp(a: tuple[int, int, int], b: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def render(size: int, supersample: int = 4) -> Image.Image:
    """Draw at `supersample` times the size and downsample, which is the only
    way to get clean curves out of PIL."""
    s = size * supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    radius = s * 0.235
    top, bottom = (0x59, 0x9B, 0xF7), (0x21, 0x53, 0xBE)

    # The tile, painted one row at a time so the gradient is exact.
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    tile_px = tile.load()
    for y in range(s):
        row = lerp(top, bottom, y / max(1, s - 1))
        for x in range(s):
            tile_px[x, y] = (*row, 255)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    img.paste(tile, (0, 0), mask)

    # A sheen across the top half, fading out before the middle.
    sheen = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sheen_px = sheen.load()
    sheen_h = int(s * 0.55)
    for y in range(sheen_h):
        a = int(70 * (1.0 - y / max(1, sheen_h - 1)))
        for x in range(s):
            sheen_px[x, y] = (255, 255, 255, a)
    sheen_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(sheen_mask).rounded_rectangle(
        [0, 0, s - 1, sheen_h], radius=radius, fill=255
    )
    img.alpha_composite(Image.composite(sheen, Image.new("RGBA", (s, s), (0, 0, 0, 0)), sheen_mask))

    # A hairline rim so the tile has an edge against a light background.
    draw.rounded_rectangle(
        [0, 0, s - 1, s - 1], radius=radius, outline=(0x0B, 0x1B, 0x3A, 60),
        width=max(1, int(s * 0.012)),
    )

    cx, cy, r = LENS_CX * s, LENS_CY * s, LENS_R * s
    ring_w = max(2, int(RING_W * s))

    # The glass, faintly tinted so the lens reads as glass and not as a hole.
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255, 64))

    # The handle first, so the ring's stroke caps it cleanly.
    a = 0.7071
    hx0, hy0 = cx + a * r * 0.72, cy + a * r * 0.72
    hx1, hy1 = hx0 + a * HANDLE_LEN * s, hy0 + a * HANDLE_LEN * s
    hw = max(2, int(HANDLE_W * s))
    draw.line([hx0, hy0, hx1, hy1], fill=(255, 255, 255, 255), width=hw)
    draw.ellipse([hx1 - hw / 2, hy1 - hw / 2, hx1 + hw / 2, hy1 + hw / 2],
                 fill=(255, 255, 255, 255))

    # The ring.
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255, 255), width=ring_w)

    # A highlight on the upper-left, which is what makes it look like glass.
    hl_r = r * 0.30
    hlx, hly = cx - a * r * 0.62, cy - a * r * 0.62
    draw.ellipse([hlx - hl_r / 2, hly - hl_r / 2, hlx + hl_r / 2, hly + hl_r / 2],
                 fill=(255, 255, 255, 150))

    return img.resize((size, size), Image.LANCZOS)


def main() -> int:
    frames = [render(size) for size in SIZES]
    # Pillow writes every requested size into one .ico when given the largest.
    frames[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"wrote {OUT} ({', '.join(str(s) for s in SIZES)})")
    png = ROOT / "build" / "ui"
    png.mkdir(parents=True, exist_ok=True)
    frames[-1].save(png / "icon_256.png")
    print(f"wrote {png / 'icon_256.png'} for eyeballing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
