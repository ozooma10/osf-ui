#include "../../examples/settings-view/src/SettingsViewConsumer.h"
#include "API/BridgeApi.h"
#include "Bridge/MessageBridge.h"
#include "Core/Log.h"
#include "SettingsServices.h"
#include "check.h"

namespace OSFUI::Log
{
    void WarnOnce(std::once_flag& flag, std::string_view message) { std::call_once(flag, [&] { REX::test::Log("WARN", std::string(message)); }); }
    bool DebugEnabled() { return true; }
    void SetDebugLogging(bool) {}
}

int main()
{
    SettingsTest::Settings settings;
    SettingsTest::Diagnostics diagnostics;
    SettingsTest::Install(&settings, &diagnostics);
    SettingsTest::viewsAcquire = [](std::uint32_t version, std::uint32_t* actual) noexcept -> void* {
        *actual = OSFUI::API::Views::kVersion;
        return OSFUI::API::Views::Supports(*actual, version) ? static_cast<OSFUI::API::Views::IViews*>(&OSFUI::API::BridgeApi::Get()) : nullptr;
    };
    auto& api = OSFUI::API::BridgeApi::Get();
    api.SetViewCatalog({SettingsViewExample::kViewId});
    SettingsViewExample::Consumer consumer;
    CHECK(consumer.Initialize());
    CHECK(consumer.Initialize()); // No duplicate lifetime hotkey registrations.
    CHECK(settings.registrations == 1);
    CHECK(settings.registeredMod == "osfui-example" && settings.registeredId == "openPanel");
    CHECK(!api.IsReady()); // The consumer does not require an existing browser to initialize.
    auto initial = api.TakePendingBatch();
    CHECK(initial.viewRegistrations == std::vector<std::string>{SettingsViewExample::kViewId});
    CHECK(initial.state.size() == 1 && initial.state[0].value == false);
    CHECK(initial.state[0].mod == "osfui-example" && initial.state[0].key == "showDetails");
    settings.values["showDetails"] = true;
    settings.changed("osfui-example", nullptr, settings.changedUser);
    auto changed = api.TakeViewStateOps();
    CHECK(changed.size() == 1 && changed[0].value == true);
    settings.changed("osfui-example", "unrelated", settings.changedUser);
    CHECK(api.TakeViewStateOps().empty());
    settings.hotkey("osfui-example", "openPanel", settings.hotkeyUser);
    auto open = api.TakePendingBatch();
    CHECK(open.state.size() == 1 && open.state[0].value == true);
    CHECK(open.presentation.size() == 1 && open.presentation[0].view == SettingsViewExample::kViewId && open.presentation[0].open);
    settings.readStatus["showDetails"] = OSFSettings::API::Status::UnknownSetting;
    settings.hotkey("osfui-example", "openPanel", settings.hotkeyUser);
    CHECK(api.TakePendingBatch().presentation.empty());
    CHECK(api.TakeViewStateOps().empty());
    return g_failures;
}
