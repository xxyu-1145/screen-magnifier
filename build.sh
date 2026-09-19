#!/usr/bin/env bash
# build.sh - single source of truth for compiling the magnifier.
#
# Toolchain note: this machine has MSVC 2010 (too old for C++20, and no
# Windows SDK installed), so we build with the clang and MinGW-w64 headers
# that ship inside the `ziglang` Python package. zig resolves -l flags
# against its bundled .def files, so no import libraries are needed on disk.
#
# Usage:
#   ./build.sh            release build
#   ./build.sh debug      debug build (assertions, no optimisation)
#   ./build.sh test       build and run the core unit tests
#   ./build.sh clean      remove build output
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"

# Where to find zig, in order of preference: whatever the caller set, then
# whatever is on PATH, then the copy inside the `ziglang` pip package, which is
# how this machine is set up and the way the README suggests.
if [ -z "${ZIG:-}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  else
    ZIG="$(python -c 'import ziglang, pathlib; print(pathlib.Path(ziglang.__file__).parent / "zig.exe")' 2>/dev/null || true)"
  fi
fi

if [ -z "${ZIG:-}" ] || [ ! -x "$ZIG" ]; then
  echo "error: zig not found${ZIG:+ at $ZIG}" >&2
  echo "       install it with: python -m pip install ziglang" >&2
  echo "       or point at an existing copy with: ZIG=/path/to/zig $0 $*" >&2
  exit 1
fi

MODE="${1:-release}"

# Libc++ emits a wall of -Wnullability-completeness noise for its own headers.
# Silence warnings originating in system headers; keep ours.
COMMON=(
  c++ -target x86_64-windows-gnu -std=c++20
  -Wno-nullability-completeness
  -Wno-unknown-pragmas
  -Wno-unused-command-line-argument
  -I"$ROOT/src"
)

# gdiplus supplies the anti-aliased path, gradient and shadow primitives the
# settings window is painted with; there is no d3dcompiler import library, so
# D3DCompile is resolved from its DLL at runtime instead.
LIBS=(
  -ld3d11 -ldxgi -ldcomp -lgdiplus
  -luser32 -lgdi32 -ldwmapi -lshell32 -lole32 -lshcore
  -lcomctl32 -lmsimg32 -luxtheme -ladvapi32 -lwinmm
)

SOURCES=(
  "$ROOT/src/core/event_bus.cpp"
  "$ROOT/src/core/hit_test.cpp"
  "$ROOT/src/core/selection_controller.cpp"
  "$ROOT/src/core/magnification_controller.cpp"
  "$ROOT/src/core/interaction_state_machine.cpp"
  "$ROOT/src/core/config.cpp"
  "$ROOT/src/core/i18n.cpp"
  "$ROOT/src/platform/topology.cpp"
  "$ROOT/src/platform/app_icon.cpp"
  "$ROOT/src/platform/input.cpp"
  "$ROOT/src/platform/tray.cpp"
  "$ROOT/src/capture/dxgi_capture.cpp"
  "$ROOT/src/capture/gdi_capture.cpp"
  "$ROOT/src/capture/capture_factory.cpp"
  "$ROOT/src/render/render_service.cpp"
  "$ROOT/src/app/ui_draw.cpp"
  "$ROOT/src/app/overlay_window.cpp"
  "$ROOT/src/app/selection_overlay.cpp"
  "$ROOT/src/app/control_window.cpp"
  "$ROOT/src/app/app_host.cpp"
  "$ROOT/src/app/main.cpp"
)

mkdir -p "$BUILD"

# The toolchain has no resource compiler and its linker rejects a .res file, so
# the icon and the application manifest are grafted onto the image after it is
# linked. A failure here leaves a perfectly usable executable that simply has no
# icon, which is why it warns rather than stops the build.
embed_resources() {
  local exe="$1"
  if [ ! -f "$ROOT/app.ico" ]; then
    echo "note: app.ico is missing, so no icon will be embedded" >&2
    echo "      regenerate it with: python tests/make_icon.py" >&2
    return 0
  fi
  python "$ROOT/tools/embed_resources.py" "$exe" "$ROOT/app.ico" || {
    echo "warning: resource embedding failed; the executable is still usable" >&2
  }
}

case "$MODE" in
  clean)
    rm -rf "$BUILD"
    echo "cleaned $BUILD"
    ;;
  debug)
    echo "== debug build =="
    "$ZIG" "${COMMON[@]}" -O0 -g -DDEBUG=1 "${SOURCES[@]}" \
      -o "$BUILD/magnifier.exe" "${LIBS[@]}" -Wl,--subsystem,windows
    embed_resources "$BUILD/magnifier.exe"
    echo "built $BUILD/magnifier.exe"
    ;;
  release)
    echo "== release build =="
    "$ZIG" "${COMMON[@]}" -O2 -DNDEBUG=1 "${SOURCES[@]}" \
      -o "$BUILD/magnifier.exe" "${LIBS[@]}" -Wl,--subsystem,windows
    embed_resources "$BUILD/magnifier.exe"
    echo "built $BUILD/magnifier.exe"
    ;;
  console)
    # Same as release but keeps a console attached, for diagnostics.
    echo "== console build =="
    "$ZIG" "${COMMON[@]}" -O2 -DNDEBUG=1 "${SOURCES[@]}" \
      -o "$BUILD/magnifier_console.exe" "${LIBS[@]}"
    embed_resources "$BUILD/magnifier_console.exe"
    echo "built $BUILD/magnifier_console.exe"
    ;;
  test)
    echo "== core unit tests =="
    "$ZIG" "${COMMON[@]}" -O2 -DNDEBUG=1 \
      "$ROOT/tests/test_core.cpp" \
      "$ROOT/src/core/selection_controller.cpp" \
      "$ROOT/src/core/magnification_controller.cpp" \
      "$ROOT/src/core/interaction_state_machine.cpp" \
      "$ROOT/src/core/hit_test.cpp" \
      "$ROOT/src/core/event_bus.cpp" \
      "$ROOT/src/core/config.cpp" \
      "$ROOT/src/core/i18n.cpp" \
      -o "$BUILD/test_core.exe" \
      -lkernel32 -luser32 -ladvapi32
    "$BUILD/test_core.exe"
    ;;
  package)
    # The shippable artifacts: one executable with its resources embedded, and
    # an archive holding it plus a readme, so a recipient can unzip and run it.
    # Everything the executable imports ships with Windows 10/11.
    echo "== package =="
    "$ZIG" "${COMMON[@]}" -O2 -DNDEBUG=1 "${SOURCES[@]}" \
      -o "$BUILD/magnifier.exe" "${LIBS[@]}" -Wl,--subsystem,windows
    embed_resources "$BUILD/magnifier.exe"
    mkdir -p "$ROOT/dist"
    cp "$BUILD/magnifier.exe" "$ROOT/dist/ScreenMagnifier.exe"
    python "$ROOT/tools/check_resources.py" "$ROOT/dist/ScreenMagnifier.exe"
    python "$ROOT/tools/make_zip.py" "$ROOT/dist/ScreenMagnifier.exe"
    echo "packaged $ROOT/dist"
    ;;
  *)
    echo "usage: $0 [release|debug|console|test|package|clean]" >&2
    exit 2
    ;;
esac
