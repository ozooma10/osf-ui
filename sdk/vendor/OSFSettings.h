// OSF Settings native service.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <limits>
#include "REX/W32/KERNEL32.h"

namespace OSFSettings::API
{
    // Packed major.minor ABI versions, independent of the plugin release version.
    // The initial contract stays at 1.0 until launch. After launch, existing slots, signatures and status values are frozen; additions require a minor bump.
    inline constexpr std::uint32_t kVersion = 0x00010001u;
    inline constexpr std::uint32_t kUnboundKey = 0xFF; // Allowed only when the schema permits unbinding.
    inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
    inline constexpr std::uint32_t kLanguageVersion = 0x00010001u; // ISettings::GetLanguage
    inline constexpr wchar_t kModuleName[] = L"OSFSettings.dll";
    inline constexpr char kRequestExportName[] = "OSFSettings_RequestAPI";

    constexpr bool Supports(std::uint32_t have, std::uint32_t need) noexcept
    {
        return (have >> 16) == (need >> 16) && (have & 0xFFFFu) >= (need & 0xFFFFu);
    }

    enum class Status : std::uint32_t
    {
        Ok = 0,
        NotReady = 1,
        InvalidArgument = 2,
        UnknownMod = 3,
        UnknownSetting = 4,
        TypeMismatch = 5,
        InvalidValue = 6,
        BufferTooSmall = 7,
        SaveFailed = 8,
        UnknownSubscription = 9,
        InternalError = 10,
        UnknownHotkeyBlock = 11,
        UnknownHotkey = 12,
        UnknownAction = 13,
        AlreadyRegistered = 14,
        UnknownInvocation = 15
    };

    using Subscription = std::uint64_t; // Zero is never a valid subscription.
    using HotkeyBlock = std::uint64_t; // Each owner releases its own nonzero token.
    using Invocation = std::uint64_t;

    // Reread current settings. key == nullptr requests a full refresh, including the initial notification.
    // Callbacks run serially from an SFSE task; no main-thread guarantee.
    using ChangedFn = void (*)(const char* mod, const char* key, void* context) noexcept;
    using HotkeyFn = void (*)(const char* mod, const char* id, void* context) noexcept;

    // Submitted on an SFSE task, with no main-thread guarantee. Return promptly;
    // CompleteAction may be called inside this callback or later from another thread.
    using ActionFn = void (*)(Invocation invocation, const char* mod, const char* id, void* context) noexcept;

    // Include OSFSettingsRegistry.h to inspect registry records.
    struct RegistryView;
    using RegistryFn = void (*)(const RegistryView&, void* context) noexcept;

    struct ISettings
    {
        virtual bool IsReady() noexcept = 0;

        // Exact setting types. On failure, scalar outputs remain unchanged.
        virtual Status GetBool(const char* mod, const char* key, bool* out) noexcept = 0;
        virtual Status GetInt(const char* mod, const char* key, std::int64_t* out) noexcept = 0;
        virtual Status GetFloat(const char* mod, const char* key, double* out) noexcept = 0;

        // Caller-owned UTF-8 buffer. required includes the final NUL.
        // nullptr/0 queries the size and returns BufferTooSmall. Never truncates.
        virtual Status GetEnum(const char* mod, const char* key, char* out, std::uint32_t capacity, std::uint32_t* required) noexcept = 0;

        // Ok means saved and published, or already equal.
        virtual Status SetBool(const char* mod, const char* key, bool value) noexcept = 0;
        virtual Status SetInt(const char* mod, const char* key, std::int64_t value) noexcept = 0;
        virtual Status SetFloat(const char* mod, const char* key, double value) noexcept = 0;
        virtual Status SetEnum(const char* mod, const char* key, const char* value) noexcept = 0;
        virtual Status Reset(const char* mod, const char* key) noexcept = 0;
        virtual Status ResetMod(const char* mod) noexcept = 0;

        // Subscribe before reading to avoid missing changes. Registration is allowed before readiness; the initial notification waits until the provider is ready.
        virtual Status Subscribe(const char* mod, ChangedFn callback, void* context, Subscription* out) noexcept = 0;
        // Outside a callback, successful unsubscribe waits for that callback to finish.
        // Inside a callback, it prevents future calls; keep context alive until return.
        virtual Status Unsubscribe(Subscription subscription) noexcept = 0;

        // Native Starfield keyboard virtual-key codes (Win32 VK), with kUnboundKey for unbound.
        // Schema defaults may use names such as "F4"; reads and writes always use resolved codes.
        // Keys are a distinct setting type; failed reads preserve *out.
        virtual Status GetKey(const char* mod, const char* key, std::uint32_t* out) noexcept = 0;
        virtual Status SetKey(const char* mod, const char* key, std::uint32_t value) noexcept = 0;

        // Blocks OSF-dispatched hotkeys; All blocks must be released before new presses can activate.
        virtual Status AcquireHotkeyBlock(HotkeyBlock* out) noexcept = 0;
        virtual Status ReleaseHotkeyBlock(HotkeyBlock block) noexcept = 0;

        // Exact IDs of a loaded hotkeys declaration without a menu target. Initialize at kPostPostLoad.
        virtual Status RegisterHotkey(const char* mod, const char* id, HotkeyFn callback, void* context) noexcept = 0;

        virtual Status GetString(const char* mod, const char* key, char* out, std::uint32_t capacity, std::uint32_t* required) noexcept = 0;
        virtual Status SetString(const char* mod, const char* key, const char* value, std::uint32_t length) noexcept = 0;

        // Reads the registry for the specified mod, invoking the callback with a consistent snapshot.
        // nullptr mod selects all mods; otherwise an exact mod ID.
        // Copy retained data in the callback;
        virtual Status ReadRegistry(const char* mod, RegistryFn callback, void* context) noexcept = 0;

        // Register once at kPostPostLoad. Exactly one native OR Papyrus handler per declaration.
        virtual Status RegisterAction(const char* mod, const char* id, ActionFn callback, void* context) noexcept = 0;
        // Only the first completion succeeds. Tokens expire on load.
        virtual Status CompleteAction(Invocation invocation, bool succeeded, const char* message) noexcept = 0;

        // ABI 1.1. The game's language code from sLanguage:General, lower-cased ("en", "de", "ptbr").
        // Same buffer contract as GetString. NotReady until the game's translation resources have loaded.
        virtual Status GetLanguage(char* out, std::uint32_t capacity, std::uint32_t* required) noexcept = 0;

    protected:
        ~ISettings() = default;
    };

    // Returns a borrowed process-lifetime interface, or nullptr for an unsupported  major/minor. outVersion is optional: actual ABI on success, zero on failure.
    using AcquireFn = void* (*)(std::uint32_t version, std::uint32_t* outVersion) noexcept;

    inline ISettings* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
    {
        if (outVersion) *outVersion = 0;
        const auto module = REX::W32::GetModuleHandleW(kModuleName);
        if (!module) return nullptr;
        const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
        return fn ? static_cast<ISettings*>(fn(version, outVersion)) : nullptr;
    }

    class Client
    {
    public:
        // Acquires and caches the service after SFSE kPostPostLoad.
        bool Init(std::uint32_t version = kBaseVersion) noexcept
        {
            std::uint32_t actual{};
            auto* api = RequestInterface(version, &actual);
            return Attach(api, actual);
        }

        // Borrows the interface; nullptr or an incompatible version detaches.
        bool Attach(ISettings* api, std::uint32_t version = kVersion) noexcept
        {
            m_api = api && Supports(version, kBaseVersion) ? api : nullptr;
            m_version = m_api ? version : 0;
            return m_api != nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
        [[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
        [[nodiscard]] bool Has(std::uint32_t version) const noexcept { return m_api && Supports(m_version, version); }
        [[nodiscard]] ISettings* Raw() const noexcept { return m_api; }
        [[nodiscard]] bool IsReady() const noexcept { return m_api && m_api->IsReady(); }

        Status GetBool(const char* mod, const char* key, bool* out) const noexcept
        {
            return m_api ? m_api->GetBool(mod, key, out) : Status::NotReady;
        }
        Status GetInt(const char* mod, const char* key, std::int64_t* out) const noexcept
        {
            return m_api ? m_api->GetInt(mod, key, out) : Status::NotReady;
        }
        Status GetFloat(const char* mod, const char* key, double* out) const noexcept
        {
            return m_api ? m_api->GetFloat(mod, key, out) : Status::NotReady;
        }
        Status GetEnum(const char* mod, const char* key, char* out, std::uint32_t capacity, std::uint32_t* required) const noexcept
        {
            return m_api ? m_api->GetEnum(mod, key, out, capacity, required) : Status::NotReady;
        }
        Status GetEnum(const char* mod, const char* key, std::string& out) const noexcept
        {
            std::string buffer;
            std::uint32_t required{};
            auto status = GetEnum(mod, key, nullptr, 0, &required);
            while (status == Status::BufferTooSmall) {
                buffer.resize(required);
                status = GetEnum(mod, key, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
            }
            if (status == Status::Ok) {
                buffer.resize(required - 1); // Exclude the terminating NUL.
                out.swap(buffer);
            }
            return status;
        }

        Status GetKey(const char* mod, const char* key, std::uint32_t* out) const noexcept
        {
            return m_api ? m_api->GetKey(mod, key, out) : Status::NotReady;
        }

        Status GetString(const char* mod, const char* key, char* out, std::uint32_t capacity, std::uint32_t* required) const noexcept
        {
            return m_api ? m_api->GetString(mod, key, out, capacity, required) : Status::NotReady;
        }
        Status GetString(const char* mod, const char* key, std::string& out) const noexcept
        {
            std::string buffer;
            std::uint32_t required{};
            auto status = GetString(mod, key, nullptr, 0, &required);
            while (status == Status::BufferTooSmall) {
                buffer.resize(required);
                status = GetString(mod, key, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
            }
            if (status == Status::Ok) {
                buffer.resize(required - 1);
                out.swap(buffer);
            }
            return status;
        }
        Status SetString(const char* mod, const char* key, const char* value, std::uint32_t length) const noexcept
        {
            return m_api ? m_api->SetString(mod, key, value, length) : Status::NotReady;
        }
        Status SetString(const char* mod, const char* key, std::string_view value) const noexcept
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) return Status::InvalidValue;
            return SetString(mod, key, value.empty() ? "" : value.data(), static_cast<std::uint32_t>(value.size()));
        }
        Status SetString(const char* mod, const char* key, const char* value) const noexcept
        {
            return value ? SetString(mod, key, std::string_view(value)) : Status::InvalidArgument;
        }
        Status SetKey(const char* mod, const char* key, std::uint32_t value) const noexcept
        {
            return m_api ? m_api->SetKey(mod, key, value) : Status::NotReady;
        }

        Status SetBool(const char* mod, const char* key, bool value) const noexcept
        {
            return m_api ? m_api->SetBool(mod, key, value) : Status::NotReady;
        }
        Status SetInt(const char* mod, const char* key, std::int64_t value) const noexcept
        {
            return m_api ? m_api->SetInt(mod, key, value) : Status::NotReady;
        }
        Status SetFloat(const char* mod, const char* key, double value) const noexcept
        {
            return m_api ? m_api->SetFloat(mod, key, value) : Status::NotReady;
        }
        Status SetEnum(const char* mod, const char* key, const char* value) const noexcept
        {
            return m_api ? m_api->SetEnum(mod, key, value) : Status::NotReady;
        }
        Status Reset(const char* mod, const char* key) const noexcept
        {
            return m_api ? m_api->Reset(mod, key) : Status::NotReady;
        }
        Status ResetMod(const char* mod) const noexcept
        {
            return m_api ? m_api->ResetMod(mod) : Status::NotReady;
        }

        Status Subscribe(const char* mod, ChangedFn callback, void* context, Subscription* out) const noexcept
        {
            return m_api ? m_api->Subscribe(mod, callback, context, out) : Status::NotReady;
        }
        Status Unsubscribe(Subscription subscription) const noexcept
        {
            return m_api ? m_api->Unsubscribe(subscription) : Status::NotReady;
        }

        Status AcquireHotkeyBlock(HotkeyBlock* out) const noexcept
        {
            return m_api ? m_api->AcquireHotkeyBlock(out) : Status::NotReady;
        }
        Status ReleaseHotkeyBlock(HotkeyBlock block) const noexcept
        {
            return m_api ? m_api->ReleaseHotkeyBlock(block) : Status::NotReady;
        }

        Status RegisterHotkey(const char* mod, const char* id, HotkeyFn callback, void* context) const noexcept
        {
            return m_api ? m_api->RegisterHotkey(mod, id, callback, context) : Status::NotReady;
        }

        Status ReadRegistry(const char* mod, RegistryFn callback, void* context) const noexcept
        {
            return m_api ? m_api->ReadRegistry(mod, callback, context) : Status::NotReady;
        }

        Status RegisterAction(const char* mod, const char* id, ActionFn callback, void* context) const noexcept
        {
            return m_api ? m_api->RegisterAction(mod, id, callback, context) : Status::NotReady;
        }
        Status CompleteAction(Invocation invocation, bool succeeded, const char* message = nullptr) const noexcept
        {
            return m_api ? m_api->CompleteAction(invocation, succeeded, message) : Status::NotReady;
        }

        // NotReady, without touching the provider, when it predates ABI 1.1.
        Status GetLanguage(char* out, std::uint32_t capacity, std::uint32_t* required) const noexcept
        {
            return Has(kLanguageVersion) ? m_api->GetLanguage(out, capacity, required) : Status::NotReady;
        }
        Status GetLanguage(std::string& out) const noexcept
        {
            std::string buffer;
            std::uint32_t required{};
            auto status = GetLanguage(nullptr, 0, &required);
            while (status == Status::BufferTooSmall) {
                buffer.resize(required);
                status = GetLanguage(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
            }
            if (status == Status::Ok) {
                buffer.resize(required - 1);
                out.swap(buffer);
            }
            return status;
        }

    private:
        ISettings* m_api{};
        std::uint32_t m_version{};
    };
}
