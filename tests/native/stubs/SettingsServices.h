#pragma once

#include "OSFSettings.h"
#include "OSFSettings_Diagnostics.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace SettingsTest
{
    using namespace OSFSettings::API;

    struct Settings : ISettings
    {
        bool ready = true;
        std::map<std::string, bool> values{{"developerMode", false}, {"highRefreshCapture", false}, {"showDetails", false}};
        std::map<std::string, Status> readStatus;
        int reads = 0, acquisitions = 0, releases = 0, registrations = 0;
        Status acquireStatus = Status::Ok, releaseStatus = Status::Ok;
        Status subscribeStatus = Status::Ok, hotkeyStatus = Status::Ok;
        HotkeyBlock nextBlock = 1;
        std::set<HotkeyBlock> blocks;
        ChangedFn changed{};
        void* changedUser{};
        HotkeyFn hotkey{};
        void* hotkeyUser{};
        std::string registeredMod, registeredId;

        bool IsReady() noexcept override { return ready; }
        Status GetBool(const char*, const char* key, bool* out) noexcept override
        {
            ++reads;
            if (readStatus.contains(key) && readStatus.at(key) != Status::Ok) return readStatus.at(key);
            if (!values.contains(key)) return Status::UnknownSetting;
            *out = values.at(key);
            return Status::Ok;
        }
        Status GetInt(const char*, const char*, std::int64_t*) noexcept override { return Status::TypeMismatch; }
        Status GetFloat(const char*, const char*, double*) noexcept override { return Status::TypeMismatch; }
        Status GetEnum(const char*, const char*, char*, std::uint32_t, std::uint32_t*) noexcept override { return Status::TypeMismatch; }
        Status SetBool(const char*, const char*, bool) noexcept override { return Status::Ok; }
        Status SetInt(const char*, const char*, std::int64_t) noexcept override { return Status::TypeMismatch; }
        Status SetFloat(const char*, const char*, double) noexcept override { return Status::TypeMismatch; }
        Status SetEnum(const char*, const char*, const char*) noexcept override { return Status::TypeMismatch; }
        Status Reset(const char*, const char*) noexcept override { return Status::Ok; }
        Status ResetMod(const char*) noexcept override { return Status::Ok; }
        Status Subscribe(const char*, ChangedFn fn, void* user, Subscription* out) noexcept override
        {
            if (subscribeStatus != Status::Ok) return subscribeStatus;
            changed = fn; changedUser = user; *out = 1;
            return Status::Ok;
        }
        Status Unsubscribe(Subscription) noexcept override { changed = nullptr; return Status::Ok; }
        Status GetKey(const char*, const char*, std::uint32_t*) noexcept override { return Status::TypeMismatch; }
        Status SetKey(const char*, const char*, std::uint32_t) noexcept override { return Status::TypeMismatch; }
        Status AcquireHotkeyBlock(HotkeyBlock* out) noexcept override
        {
            ++acquisitions;
            if (acquireStatus != Status::Ok) return acquireStatus;
            *out = nextBlock++;
            blocks.insert(*out);
            return Status::Ok;
        }
        Status ReleaseHotkeyBlock(HotkeyBlock token) noexcept override
        {
            ++releases;
            if (releaseStatus != Status::Ok) return releaseStatus;
            return blocks.erase(token) ? Status::Ok : Status::UnknownHotkeyBlock;
        }
        Status RegisterHotkey(const char* mod, const char* id, HotkeyFn fn, void* user) noexcept override
        {
            if (hotkeyStatus != Status::Ok) return hotkeyStatus;
            ++registrations; registeredMod = mod; registeredId = id; hotkey = fn; hotkeyUser = user;
            return Status::Ok;
        }
        Status GetString(const char*, const char*, char*, std::uint32_t, std::uint32_t*) noexcept override { return Status::TypeMismatch; }
        Status SetString(const char*, const char*, const char*, std::uint32_t) noexcept override { return Status::TypeMismatch; }
        Status ReadRegistry(const char*, RegistryFn, void*) noexcept override { return Status::InternalError; }
    };

    struct Diagnostics : OSFSettings::API::Diagnostics::IDiagnostics
    {
        using Issue = OSFSettings::API::Diagnostics::Issue;
        struct Saved
        {
            OSFSettings::API::Diagnostics::Severity severity;
            std::string title, impact, nextSteps;
        };
        std::map<std::pair<std::string, std::string>, Saved> issues;
        Status reportStatus = Status::Ok, clearStatus = Status::Ok;
        int reports = 0, clears = 0;
        Status Report(const Issue& issue) noexcept override
        {
            ++reports;
            if (reportStatus != Status::Ok) return reportStatus;
            issues.insert_or_assign({issue.modId, issue.id}, Saved{issue.severity, issue.title,
                issue.impact ? issue.impact : "", issue.nextSteps ? issue.nextSteps : ""});
            return Status::Ok;
        }
        Status Clear(const char* mod, const char* id) noexcept override
        {
            ++clears;
            if (clearStatus != Status::Ok) return clearStatus;
            issues.erase({mod, id});
            return Status::Ok;
        }
        Status ClearMod(const char*) noexcept override { return Status::InternalError; } // UI must clear only owned IDs.
        bool Has(const std::string& id) const { return issues.contains({"osfui", id}); }
        const Saved& Get(const std::string& id) const { return issues.at({"osfui", id}); }
    };

    inline Settings* settings{};
    inline Diagnostics* diagnostics{};
    inline std::uint32_t settingsVersion = kVersion;
    inline std::uint32_t diagnosticsVersion = kVersion;
    inline void* (*viewsAcquire)(std::uint32_t, std::uint32_t*) noexcept = nullptr;

    inline void* RequestSettings(std::uint32_t need, std::uint32_t* actual) noexcept
    {
        *actual = settings && Supports(settingsVersion, need) ? settingsVersion : 0;
        return *actual ? settings : nullptr;
    }
    inline void* RequestDiagnostics(std::uint32_t need, std::uint32_t* actual) noexcept
    {
        *actual = diagnostics && Supports(diagnosticsVersion, need) ? diagnosticsVersion : 0;
        return *actual ? diagnostics : nullptr;
    }
    inline void Install(Settings* provider, Diagnostics* reports)
    {
        settings = provider; diagnostics = reports;
        settingsVersion = kVersion; diagnosticsVersion = kVersion;
        REX::W32::test::moduleLookup = [](const wchar_t* name) noexcept -> void* {
            if (std::wstring_view(name) == L"OSFUI.dll") return viewsAcquire ? &viewsAcquire : nullptr;
            return settings || diagnostics ? &settings : nullptr;
        };
        REX::W32::test::procLookup = [](void*, const char* name) noexcept -> void* {
            if (std::string_view(name) == "OSFSettings_RequestAPI") return reinterpret_cast<void*>(&RequestSettings);
            if (std::string_view(name) == "OSFSettings_RequestDiagnosticsAPI") return reinterpret_cast<void*>(&RequestDiagnostics);
            if (std::string_view(name) == "OSFUI_RequestAPI") return reinterpret_cast<void*>(viewsAcquire);
            return nullptr;
        };
    }
}
