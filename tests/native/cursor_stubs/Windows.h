#pragma once

#include <cstdint>

// Win32 cursor API double for the real HardwareCursor.cpp; never changes the desktop cursor.
using BOOL = int;
using DWORD = std::uint32_t;
using HWND = void*;
using HCURSOR = void*;
using HINSTANCE = void*;
using LPCWSTR = const wchar_t*;
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr DWORD CURSOR_SHOWING = 1;
struct POINT { std::int32_t x, y; };
struct RECT { std::int32_t left, top, right, bottom; };
struct CURSORINFO { DWORD cbSize, flags; HCURSOR hCursor; POINT ptScreenPos; };
#define MAKEINTRESOURCEW(id) reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(id))

BOOL GetCursorInfo(CURSORINFO*);
HCURSOR GetCursor();
HCURSOR SetCursor(HCURSOR);
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR);
int ShowCursor(BOOL);
BOOL GetClientRect(HWND, RECT*);
BOOL ClientToScreen(HWND, POINT*);
BOOL SetCursorPos(int, int);
BOOL ClipCursor(const RECT*);
BOOL GetClipCursor(RECT*);
