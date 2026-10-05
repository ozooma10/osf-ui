#pragma once

#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#endif

// Only the device-health query used by Runtime is needed on the desktop.
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (static_cast<std::int32_t>(hr) >= 0)
#endif
struct ID3D12Device
{
    std::int32_t GetDeviceRemovedReason();
    void Release();
};
struct ID3D12CommandQueue
{
    void Release();
};
