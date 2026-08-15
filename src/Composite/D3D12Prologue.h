#pragma once

// GDI-free Win32 + D3D12 include prologue, shared by the composite/ translation
// units that talk to the engine's D3D12 device.
//
// NOGDI is the point: wingdi.h defines an `ERROR` macro that otherwise clobbers
// REX::ERROR at every use site in the including TU. WIN32_LEAN_AND_MEAN trims the
// rest of the Win32 surface; NOMINMAX keeps the min/max macros from breaking
// std::min/std::max.
//
// The defines must land BEFORE <Windows.h> is first seen, so include this header
// before any other header that could pull Win32 in.
//
// Deliberately NOT folded into Platform/WindowsPlatform.h: that is a Win32-free
// facade included by many non-graphics consumers, and it must stay that way.

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
