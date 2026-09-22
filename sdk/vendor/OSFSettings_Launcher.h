// Optional launcher service. Copy this header; link nothing.
#pragma once
#include <cstdint>
#include "REX/W32/KERNEL32.h"

namespace OSFSettings::API::Launcher
{
    inline constexpr std::uint32_t kVersion = 0x00020000;
    enum class Status : std::uint32_t { Ok, InvalidArgument, AlreadyRegistered, NotFound, InternalError };
    // Runs as Settings leaves the menu stack. Return promptly and queue work on your
    // established UI/runtime lane. The provider owns closing; there is no return session.
    // Strings have callback lifetime.
    using OpenFn = void (*)(const char* modId, const char* id, void* context) noexcept;
    struct Destination
    {
        const char* modId{};
        const char* id{};
        const char* modTitle{}; // Optional; defaults to modId.
        const char* title{};
        const char* description{};
        const char* menu{}; // Exactly one of menu or open. Native menu names, not SWF paths.
        OpenFn open{};
        void* context{};
    };
    struct ILauncher
    {
        // Metadata is copied. Native callback code/context must live until process exit.
        virtual Status Register(const Destination&) noexcept = 0;
        virtual Status SetAvailable(const char* modId, const char* id, bool available, const char* reason) noexcept = 0;
    protected:
        ~ILauncher() = default;
    };
    using AcquireFn = void* (*)(std::uint32_t, std::uint32_t*) noexcept;
    inline ILauncher* RequestInterface() noexcept
    {
        const auto module = REX::W32::GetModuleHandleW(L"OSFSettings.dll");
        if (!module) return nullptr;
        const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, "OSFSettings_RequestLauncherAPI"));
        std::uint32_t version{};
        auto* api = fn ? static_cast<ILauncher*>(fn(kVersion, &version)) : nullptr;
        return (version >> 16) == (kVersion >> 16) && version >= kVersion ? api : nullptr;
    }
    class Client
    {
    public:
        bool Init() noexcept { m_api = RequestInterface(); return m_api != nullptr; }
        explicit operator bool() const noexcept { return m_api != nullptr; }
        Status Register(const Destination& destination) const noexcept { return m_api ? m_api->Register(destination) : Status::InternalError; }
        Status SetAvailable(const char* mod, const char* id, bool available, const char* reason = "") const noexcept
        { return m_api ? m_api->SetAvailable(mod, id, available, reason) : Status::InternalError; }
    private:
        ILauncher* m_api{};
    };
}
