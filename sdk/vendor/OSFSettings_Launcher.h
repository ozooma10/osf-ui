#pragma once

#include "OSFSettings.h"

namespace OSFSettings::API::Launcher
{
    inline constexpr std::uint32_t kVersion = 0x00010000u;
    inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
    inline constexpr char kRequestExportName[] = "OSFSettings_RequestLauncherAPI";

    // Open starts a request while Settings remains visible and owns input. The same signature is used for the after-close callback supplied to Complete.
    using OpenFn = void (*)(const char* modId, const char* id, std::uint64_t requestId, void* context) noexcept;

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
        // UnknownLauncher rejects a destination that was never registered.
        virtual Status SetAvailable(const char* modId, const char* id, bool available, const char* reason) noexcept = 0;
        // Completes once: a callback closes Settings; nullptr reports failure and leaves it open.
        // UnknownLaunchRequest rejects abandoned, expired, or already-completed requests; afterClose runs on removal.
        virtual Status Complete(std::uint64_t requestId, OpenFn afterClose, void* context, const char* reason) noexcept = 0;

    protected:
        ~ILauncher() = default;
    };

    inline ILauncher* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
    {
        return static_cast<ILauncher*>(RequestExport(kRequestExportName, version, outVersion));
    }

    class Client
    {
    public:
        // Initialize at SFSE kPostLoad or later, before sharing the client
        bool Init(std::uint32_t version = kBaseVersion) noexcept
        {
            std::uint32_t actual{};
            auto* api = RequestInterface(version, &actual);
            return Attach(api, actual);
        }

        bool Attach(ILauncher* api, std::uint32_t version = kVersion) noexcept
        {
            m_api = api && Supports(version, kBaseVersion) ? api : nullptr;
            m_version = m_api ? version : 0;
            return m_api != nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
        [[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
        [[nodiscard]] bool Has(std::uint32_t version) const noexcept { return m_api && Supports(m_version, version); }
        [[nodiscard]] ILauncher* Raw() const noexcept { return m_api; }

        Status Register(const Destination& destination) const noexcept
        {
            return m_api ? m_api->Register(destination) : Status::NotReady;
        }
        Status SetAvailable(const char* modId, const char* id, bool available, const char* reason = "") const noexcept
        {
            return m_api ? m_api->SetAvailable(modId, id, available, reason) : Status::NotReady;
        }
        Status Complete(std::uint64_t requestId, OpenFn afterClose, void* context = nullptr, const char* reason = "") const noexcept
        {
            return m_api ? m_api->Complete(requestId, afterClose, context, reason) : Status::NotReady;
        }

    private:
        ILauncher* m_api{};
        std::uint32_t m_version{};
    };
}
