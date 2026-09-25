#pragma once

#include "OSFSettings.h"

namespace OSFSettings::API::Diagnostics
{
    // Diagnostics ABI version, independent of the settings service and plugin release.
    inline constexpr std::uint32_t kVersion = 0x00010000u;
    inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
    inline constexpr char kRequestExportName[] = "OSFSettings_RequestDiagnosticsAPI";

    enum class Severity : std::uint32_t
    {
        Warning = 0,
        Error = 1
    };

    // Borrowed NUL-terminated UTF-8 strings, copied by Report before it returns.
    struct Issue
    {
        const char* modId{};
        const char* id{};
        Severity severity{ Severity::Warning };
        const char* title{};
        const char* impact{}; // Optional; nullptr or empty is allowed.
        const char* nextSteps{}; // Optional; nullptr or empty is allowed.
    };

    struct IDiagnostics
    {
        virtual Status Report(const Issue& issue) noexcept = 0;
        virtual Status Clear(const char* modId, const char* id) noexcept = 0;
        virtual Status ClearMod(const char* modId) noexcept = 0;

    protected:
        ~IDiagnostics() = default;
    };

    // Borrowed process-lifetime interface. Unsupported versions return nullptr and zero outVersion.
    inline IDiagnostics* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
    {
        if (outVersion) *outVersion = 0;
        const auto module = REX::W32::GetModuleHandleW(kModuleName);
        if (!module) return nullptr;
        const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
        return fn ? static_cast<IDiagnostics*>(fn(version, outVersion)) : nullptr;
    }

    class Client
    {
    public:
        // Initialize at SFSE kPostLoad or later, before sharing the client across threads.
        bool Init(std::uint32_t version = kBaseVersion) noexcept
        {
            std::uint32_t actual{};
            auto* api = RequestInterface(version, &actual);
            return Attach(api, actual);
        }

        bool Attach(IDiagnostics* api, std::uint32_t version = kVersion) noexcept
        {
            m_api = api && Supports(version, kBaseVersion) ? api : nullptr;
            m_version = m_api ? version : 0;
            return m_api != nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
        [[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
        [[nodiscard]] bool Has(std::uint32_t version) const noexcept { return m_api && Supports(m_version, version); }
        [[nodiscard]] IDiagnostics* Raw() const noexcept { return m_api; }

        Status Report(const Issue& issue) const noexcept
        {
            return m_api ? m_api->Report(issue) : Status::NotReady;
        }
        Status Clear(const char* modId, const char* id) const noexcept
        {
            return m_api ? m_api->Clear(modId, id) : Status::NotReady;
        }
        Status ClearMod(const char* modId) const noexcept
        {
            return m_api ? m_api->ClearMod(modId) : Status::NotReady;
        }

    private:
        IDiagnostics* m_api{};
        std::uint32_t m_version{};
    };
}
