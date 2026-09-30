#include "Compat/V1/LegacyBridge.h"
#include "API/BridgeApi.h"
#include "Core/Paths.h"
#include "SettingsServices.h"
#include "vendor/OSFSettings_Providers.h"
#include "vendor/OSFSettingsRegistry.h"
#include "check.h"

extern "C" void* OSFUI_RequestBridge(std::uint32_t) noexcept;

namespace OSFUI::Paths
{
    const std::filesystem::path& DataDir()
    {
        static const auto root = std::filesystem::temp_directory_path() /
            ("osfui-legacy-bridge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / "OSF" / "UI";
        return root;
    }
}
namespace OSFUI::Log { bool DebugEnabled() { return false; } }

namespace
{
    using namespace OSFSettings::API;
    struct CameraSettings : SettingsTest::Settings
    {
        std::uint32_t toggle = 0x73; // F4
        Status ReadRegistry(const char* mod, RegistryFn callback, void* context) noexcept override
        {
            CHECK(std::string_view(mod) == "somaticcamera.sf");
            SettingView setting{};
            setting.key = {"iToggleKey", 10};
            setting.type = SettingType::Key;
            setting.value.key = toggle;
            GroupView group{};
            group.settings = &setting; group.settingCount = 1;
            ModView camera{};
            camera.groups = &group; camera.groupCount = 1;
            callback({&camera, 1}, context);
            return Status::Ok;
        }
    };
    struct Providers : OSFSettings::API::Providers::IProviders
    {
        Status Register(const char*, const char*, const char*, OSFSettings::API::Providers::SaveFn,
            void*, OSFSettings::API::Providers::Registration*) noexcept override { return Status::Ok; }
        Status Unregister(OSFSettings::API::Providers::Registration) noexcept override { return Status::Ok; }
        Status SubscribeKey(const char*, const char*, HotkeyFn, void*, Subscription*) noexcept override { return Status::Ok; }
        Status UnsubscribeKey(Subscription) noexcept override { return Status::Ok; }
    } providers;

    void* RequestProviders(std::uint32_t need, std::uint32_t* actual) noexcept
    {
        *actual = Supports(kVersion, need) ? kVersion : 0;
        return *actual ? &providers : nullptr;
    }
}

int main()
{
    using namespace OSFUI::Compat::V1;
    CameraSettings settings;
    SettingsTest::Install(&settings, nullptr);
    REX::W32::test::procLookup = [](void*, const char* name) noexcept -> void* {
        if (std::string_view(name) == "OSFSettings_RequestAPI") return reinterpret_cast<void*>(&SettingsTest::RequestSettings);
        if (std::string_view(name) == "OSFSettings_RequestProvidersAPI") return reinterpret_cast<void*>(&RequestProviders);
        return nullptr;
    };

    // The shipped SomaticCameraSF 1.0.0.4 requests 1.8 before subscribing via
    // the unchanged slot 9. Schema-only tests cannot detect a rejected request.
    auto* bridge = static_cast<IOSFUIBridge*>(OSFUI_RequestBridge(0x10008));
    CHECK(bridge != nullptr);
    if (!bridge) return g_failures;
    CHECK(bridge->GetInterfaceVersion() == 0x10008);
    for (std::uint32_t minor = 0; minor <= 8; ++minor) CHECK(OSFUI_RequestBridge(0x10000 | minor) == bridge);
    CHECK(OSFUI_RequestBridge(0x10009) == nullptr);
    CHECK(OSFUI_RequestBridge(0x20000) == nullptr);
    CHECK(OSFUI_RequestBridge(0x00008) == nullptr);

    std::vector<std::string> received;
    const auto token = bridge->SubscribeSettings("somaticcamera.sf",
        [](const char* mod, const char* key, const char* json, void* context) noexcept {
            CHECK(std::string_view(mod) == "somaticcamera.sf");
            CHECK(std::string_view(key) == "iToggleKey");
            static_cast<std::vector<std::string>*>(context)->emplace_back(json);
        }, &received);
    CHECK(token != 0);
    Initialize();
    Pump();
    CHECK(received == std::vector<std::string>{"\"F4\""});
    CHECK(settings.changed != nullptr);
    if (!settings.changed) return g_failures;
    settings.toggle = 0x75; // F6, the reported rebind
    settings.changed("somaticcamera.sf", "iToggleKey", settings.changedUser);
    Pump();
    CHECK(received.size() == 2 && received.back() == "\"F6\"");
    Pump();
    CHECK(received.size() == 2);
    bridge->UnsubscribeSettings(token);
    CHECK(settings.changed == nullptr);

    // Accepting 1.8 also promises its appended retained-state slot.
    CHECK(bridge->SetViewState("somaticcamera.sf", "camera", "{\"active\":true}"));
    auto state = OSFUI::API::BridgeApi::Get().TakePendingState();
    CHECK(state.size() == 1);
    CHECK(state[0].mod == "somaticcamera.sf" && state[0].key == "camera" && state[0].value["active"] == true);
    CHECK(!bridge->SetViewState("somaticcamera.sf", "camera", "invalid json"));
    return g_failures;
}
