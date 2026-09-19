// platform/app_icon.h — the application icon.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace mag {

// Draws the application icon at the requested pixel size and returns a handle
// the caller owns (DestroyIcon when finished).
//
// It is drawn rather than loaded from a resource because this toolchain has no
// resource compiler: there is no way to embed an .ico in the executable. Drawing
// it also means every size the shell asks for — 16 in the tray, 32 on the title
// bar, whatever the task switcher wants — is rendered for that size instead of
// being resampled down from one bitmap.
//
// Returns nullptr when GDI+ is not running or the bitmap could not be created,
// in which case the caller should leave the default icon alone.
HICON make_app_icon(int size_px) noexcept;

}  // namespace mag
