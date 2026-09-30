#pragma once

#include "OSFSettings.h"

namespace OSFSettings::API::Providers
{
    inline constexpr std::uint32_t kVersion = 0x00010000u;
    inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
    inline constexpr char kRequestExportName[] = "OSFSettings_RequestProvidersAPI";

    using Registration = std::uint64_t;
    // Called synchronously before publishing an edit. valuesJson is a complete
    // key/value object (key setting values are VK integers). Return false to reject the edit.
    // Runs under the settings transaction lock: do not call Settings APIs here.
    using SaveFn = bool (*)(const char* mod, const char* valuesJson, void* context) noexcept;

    struct IProviders
    {
        // schemaJson uses the normal Settings schema. Only settings are supported;
        // register actions, launchers and control-map hotkeys through their own APIs.
        // valuesJson is a key/value object; omitted keys use defaults.
        // *registration == 0 creates; a nonzero owned token replaces the definition,
        // retaining existing values that still validate. Static schemas always win.
        // Copies all input. context must live until successful Unregister returns.
        virtual Status Register(const char* mod, const char* schemaJson, const char* valuesJson,
            SaveFn save, void* context, Registration* registration) noexcept = 0;
        virtual Status Unregister(Registration registration) noexcept = 0;

        // Observe a key setting without consuming input or creating a ControlMap action.
        // Registration may precede the definition. Only gameplay key-down edges fire,
        // subject to the shared hotkey blocks. Callback runs inline on input dispatch.
        // Unsubscribe waits for in-flight callbacks; self-unsubscribe is supported.
        virtual Status SubscribeKey(const char* mod, const char* key, HotkeyFn callback,
            void* context, Subscription* subscription) noexcept = 0;
        virtual Status UnsubscribeKey(Subscription subscription) noexcept = 0;

    protected:
        ~IProviders() = default;
    };

    inline IProviders* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
    {
        return static_cast<IProviders*>(RequestExport(kRequestExportName, version, outVersion));
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

        bool Attach(IProviders* api, std::uint32_t version = kVersion) noexcept
        {
            m_api = api && Supports(version, kBaseVersion) ? api : nullptr;
            m_version = m_api ? version : 0;
            return m_api != nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
        [[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
        [[nodiscard]] bool Has(std::uint32_t version) const noexcept { return m_api && Supports(m_version, version); }
        [[nodiscard]] IProviders* Raw() const noexcept { return m_api; }

        Status Register(const char* mod, const char* schemaJson, const char* valuesJson,
            SaveFn save, void* context, Registration* registration) const noexcept
        {
            return m_api ? m_api->Register(mod, schemaJson, valuesJson, save, context, registration) : Status::NotReady;
        }
        Status Unregister(Registration registration) const noexcept
        {
            return m_api ? m_api->Unregister(registration) : Status::NotReady;
        }
        Status SubscribeKey(const char* mod, const char* key, HotkeyFn callback, void* context, Subscription* subscription) const noexcept
        {
            return m_api ? m_api->SubscribeKey(mod, key, callback, context, subscription) : Status::NotReady;
        }
        Status UnsubscribeKey(Subscription subscription) const noexcept
        {
            return m_api ? m_api->UnsubscribeKey(subscription) : Status::NotReady;
        }

    private:
        IProviders* m_api{};
        std::uint32_t m_version{};
    };
}
