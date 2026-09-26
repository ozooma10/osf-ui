// Runtime settings definitions with caller-owned persistence.
#pragma once
#include "OSFSettings.h"

namespace OSFSettings::API::Providers
{
    inline constexpr std::uint32_t kVersion = 0x00010000u;
    using Registration = std::uint64_t;
    // Called synchronously before publishing an edit. valuesJson is a complete
    // key/value object (keys are VK integers). Return false to reject the edit.
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

    inline IProviders* RequestInterface() noexcept
    {
        const auto module = REX::W32::GetModuleHandleW(kModuleName);
        if (!module) return nullptr;
        const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, "OSFSettings_RequestProvidersAPI"));
        std::uint32_t actual{};
        auto* api = fn ? static_cast<IProviders*>(fn(kVersion, &actual)) : nullptr;
        return Supports(actual, kVersion) ? api : nullptr;
    }
}
