#include "Settings/SettingsJson.h"
#include <fstream>
#include "check.h"

int main()
{
    const auto parse = [](const char* path) {
        std::ifstream file(path);
        std::string error;
        auto schema = OSFSettings::SettingsJson::ParseSchema(file, error);
        CHECK(schema.has_value());
        CHECK(error.empty());
        return schema.value();
    };
    const auto runtime = parse("../../data/SFSE/Plugins/OSF/Settings/schemas/osfui.json");
    CHECK(runtime.id == "osfui");
    CHECK(runtime.hotkeys.empty());
    CHECK(runtime.groups.size() == 2);
    CHECK(runtime.groups[0].id == "Runtime" && runtime.groups[0].label == "Runtime");
    CHECK(runtime.groups[1].id == "Developer" && runtime.groups[1].label == "Developer");
    for (const auto* key : {"developerMode", "highRefreshCapture"}) {
        const auto* setting = runtime.FindSetting(key);
        CHECK(setting && setting->requiresRestart);
        CHECK(setting && std::holds_alternative<OSFSettings::BoolDefinition>(setting->definition));
        CHECK(setting && !std::get<bool>(setting->DefaultValue()));
    }
    const auto example = parse("../../examples/settings-view/data/SFSE/Plugins/OSF/Settings/schemas/osfui-example.json");
    CHECK(example.id == "osfui-example");
    CHECK(example.groups.size() == 1);
    CHECK(example.groups[0].id == "Panel" && example.groups[0].label == "Panel");
    CHECK(example.FindSetting("showDetails") != nullptr);
    CHECK(example.hotkeys.size() == 1);
    CHECK(example.hotkeys[0].id == "openPanel" && !example.hotkeys[0].menu);
    CHECK(example.hotkeys[0].group == "Panel");
    return g_failures;
}
