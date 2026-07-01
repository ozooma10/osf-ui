// ============================================================================
// OSFUI_API.h - OSF UI native bridge API.
// ============================================================================

#pragma once

#include "REX/W32/KERNEL32.h"  // GetModuleHandleW / GetProcAddress / HMODULE
#include <cstdint>

namespace OSFUI::API
{
    // Packed (MAJOR << 16) | MINOR. MAJOR breaks ABI; MINOR bumps on an appended vmethod.
    inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 0u;
    inline constexpr std::uint32_t kBridgeAPIMajor   = kBridgeAPIVersion >> 16;

    inline constexpr const wchar_t* kModuleName        = L"OSFUI.dll";
    inline constexpr const char*    kRequestExportName = "OSFUI_RequestBridge";

    using CommandFn = void (*)(const char* a_command,
                               const char* a_payloadJson,
                               const char* a_sourceViewId,
                               void*       a_user) noexcept;

    using ReadyFn = void (*)(void* a_user) noexcept;

    struct IOSFUIBridge
    {
        virtual std::uint32_t GetInterfaceVersion() = 0;
        virtual void          GetPluginVersion(std::uint32_t& a_major,
                                               std::uint32_t& a_minor,
                                               std::uint32_t& a_patch) = 0;
        virtual const char*   GetBridgeProtocolVersion() = 0;
        virtual bool          IsBridgeReady() = 0;

        virtual void RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) = 0;
        virtual void UnregisterCommand(const char* a_command) = 0;

        virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;

        virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;

    protected:
        ~IOSFUIBridge() = default;
    };

    using RequestBridge_t = IOSFUIBridge* (*)(std::uint32_t a_abiVersion) noexcept;

    // FETCH ONCE and cache. Call after SFSE kPostLoad. Do NOT call per-frame.
    inline IOSFUIBridge* RequestBridge(std::uint32_t a_abiVersion = kBridgeAPIVersion) noexcept
    {
        const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kModuleName);
        if (!mod) {
            return nullptr;  // OSF UI not installed/loaded.
        }
        const auto fn = reinterpret_cast<RequestBridge_t>(
            REX::W32::GetProcAddress(mod, kRequestExportName));
        return fn ? fn(a_abiVersion) : nullptr;  // older OSF UI / MAJOR mismatch -> nullptr.
    }
}
