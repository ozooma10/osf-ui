#include "Input/HardwareCursor.h"
#include "check.h"
#include <Windows.h>
#include <utility>

namespace
{
    HCURSOR cursor = nullptr;
    int displayCount = 0;
    int showCalls = 0;
    int centerCalls = 0;
    bool clipped = false;
    RECT clip{};
    auto arrow = reinterpret_cast<HCURSOR>(32512);
    auto hand = reinterpret_cast<HCURSOR>(32649);
}

BOOL GetCursorInfo(CURSORINFO* info)
{
    info->flags = cursor && displayCount >= 0 ? CURSOR_SHOWING : 0;
    info->hCursor = cursor;
    return TRUE;
}
HCURSOR GetCursor() { return cursor; }
HCURSOR SetCursor(HCURSOR value) { return std::exchange(cursor, value); }
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR id)
{
    const auto value = reinterpret_cast<std::uintptr_t>(id);
    return value == 32512 || value == 32649 ? reinterpret_cast<HCURSOR>(value) : nullptr;
}
int ShowCursor(BOOL show)
{
    ++showCalls;
    return displayCount += show ? 1 : -1;
}
BOOL GetClientRect(HWND, RECT* rect) { *rect = { 0, 0, 1920, 1080 }; return TRUE; }
BOOL ClientToScreen(HWND, POINT*) { return TRUE; }
BOOL SetCursorPos(int, int) { ++centerCalls; return TRUE; }
BOOL ClipCursor(const RECT* rect)
{
    clipped = rect != nullptr;
    if (rect) clip = *rect;
    return TRUE;
}
BOOL GetClipCursor(RECT* rect) { *rect = clip; return TRUE; }

int main()
{
    namespace hardware = OSFUI::HardwareCursor;
    auto hwnd = reinterpret_cast<HWND>(1);

    // Gameplay can hide the pointer with SetCursor(nullptr) at display count zero.
    hardware::Activate(hwnd);
    CHECK(hardware::IsActive() && cursor == arrow && clipped);
    CHECK(displayCount == 0 && showCalls == 0);
    hardware::Activate(hwnd);
    CHECK(centerCalls == 1); // Repeated activation must not move the mouse again.
    hardware::Deactivate();
    CHECK(!hardware::IsActive() && cursor == nullptr && !clipped);
    CHECK(displayCount == 0 && showCalls == 0);
    hardware::Deactivate();
    CHECK(displayCount == 0);

    // Balance only our counter increments; retain changes made by another owner.
    cursor = hand;
    displayCount = -2;
    hardware::Activate(hwnd);
    CHECK(cursor == arrow && displayCount == 0);
    ++displayCount;
    hardware::Deactivate();
    CHECK(cursor == hand && displayCount == -1);

    // A null shape during capture must not accumulate visibility-counter raises.
    cursor = nullptr;
    displayCount = 0;
    showCalls = 0;
    hardware::Activate(hwnd);
    cursor = nullptr;
    hardware::SetSystemCursorId(32649);
    hardware::Reassert(hwnd);
    CHECK(cursor == hand && showCalls == 0);
    hardware::Deactivate();
    CHECK(cursor == nullptr && displayCount == 0);

    // Relinquishing capture must leave a replacement installed by another owner alone.
    hardware::SetSystemCursorId(0);
    hardware::Activate(hwnd);
    cursor = hand;
    hardware::Deactivate();
    CHECK(cursor == hand);

    // Repeated open/close or focus-loss cycles cannot leave an arrow behind.
    cursor = nullptr;
    for (int i = 0; i < 20; ++i) {
        hardware::Activate(hwnd);
        hardware::Deactivate();
        CHECK(cursor == nullptr && displayCount == 0 && !clipped);
    }

    std::printf("hardware_cursor_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
