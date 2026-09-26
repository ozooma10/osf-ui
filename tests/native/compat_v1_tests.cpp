#include "Compat/V1/LegacyABI.h"
#include "Compat/V1/SettingsCodec.h"
#include "Compat/V1/LegacyViews.h"
#include "Views/ViewManager.h"
#include "Core/Paths.h"
#include "stubs/check.h"
#include <cstddef>
#include <iostream>

namespace OSFUI::Paths
{
    const std::filesystem::path& DataDir()
    {
        static const auto root = std::filesystem::temp_directory_path() /
            ("osfui-compat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / "OSF" / "UI";
        return root;
    }
}
namespace OSFUI::Log { bool DebugEnabled() { return false; } }

int main()
{
    using namespace OSFUI::Compat::V1;
    namespace fs = std::filesystem;
    static_assert(sizeof(Request) == 48 && offsetof(Request, _token) == 24);
    static_assert(!std::has_virtual_destructor_v<IOSFUIBridge>);
    const fs::path fixtures = "tests/native/fixtures/compat-v1";
    const auto read = [](const fs::path& file) { std::ifstream input(file); return Document::parse(input); };
    std::string error;
    for (const auto& [id, count] : std::vector<std::pair<std::string, unsigned>>{
        {"ddc.dynamicdialoguecamera", 6}, {"somaticcamera.sf", 18}, {"fieldos.aegis", 24}}) {
        const auto schema = read(fixtures / (id + ".json"));
        const auto translated = TranslateSettings(schema, Document::object(), error);
        CHECK(translated.has_value());
        if (!translated) { std::cerr << error << '\n'; continue; }
        CHECK(translated->values.size() == count);
        CHECK(translated->schema["groups"].is_object());
        CHECK(translated->Encode(translated->values).size() == 2); // sparse defaults plus version stamps
    }
    auto camera = TranslateSettings(read(fixtures / "somaticcamera.sf.json"),
        {{"iToggleKey", "F8"}, {"iPresetCycleKey", "MOUSE5"}, {"future", {{"opaque", 42}}}, {"$formatVersion", 8}}, error);
    CHECK(camera.has_value());
    CHECK(camera->values["iToggleKey"] == 0x77);
    CHECK(camera->values["iPresetCycleKey"] == 6);
    CHECK(camera->Encode(camera->values)["iPresetCycleKey"] == "MOUSE5");
    CHECK(camera->Encode(camera->values)["future"]["opaque"] == 42);
    CHECK(camera->Encode(camera->values)["$formatVersion"] == 8);
    CHECK(SaveValues(*camera, camera->values));
    const auto valuePath = Root() / "settings/values/somaticcamera.sf.json";
    CHECK(read(valuePath) == camera->Encode(camera->values));
    auto invalidKey = camera->values;
    invalidKey["iToggleKey"] = 0x07; // no legacy name; reject instead of persisting a different binding
    CHECK(!SaveValues(*camera, invalidKey));
    CHECK(read(valuePath) == camera->Encode(camera->values));
    fs::create_directory(valuePath.string() + ".tmp");
    CHECK(!SaveValues(*camera, camera->values));
    CHECK(read(valuePath) == camera->Encode(camera->values));
    for (unsigned code : {0x77u, 0x78u, 0x41u, 1u, 2u, 4u, 5u, 6u, 255u}) CHECK(KeyCode(KeyName(code)) == code);
    const auto ddc = TranslateSettings(read(fixtures / "ddc.dynamicdialoguecamera.json"), {{"enabled", false}}, error);
    CHECK(ddc->values["enabled"] == false);
    CHECK(ddc->schema["groups"]["Camera"][1]["options"]["Calm"] == "Calm - just a couple of angles");
    auto bad = read(fixtures / "ddc.dynamicdialoguecamera.json");
    bad["id"] = "../outside";
    CHECK(!TranslateSettings(bad, Document::object(), error));
    bad["id"] = "osfui";
    CHECK(!TranslateSettings(bad, Document::object(), error));

    for (const auto& [id, key] : std::vector<std::pair<std::string, std::string>>{
        {"x2357aiss.companionlog", "F8"}, {"starcade.arcade", "F9"}}) {
        Document runtime = {{"id", id}, {"groups", Document::array({{{"label", "Gameplay"}, {"settings", Document::array({
            {{"key", "openKey"}, {"type", "key"}, {"default", key}, {"inputContext", "launcher"}}})}}})}};
        auto definition = TranslateSettings(runtime, Document::object(), error);
        CHECK(definition && definition->values["openKey"] == KeyCode(key));
    }

    for (const auto& [id, fixture] : std::vector<std::pair<std::string, std::string>>{
        {"devilzdad.devilzshop/esm-explorer", "shop"}, {"x2357aiss.companionlog/companionlog", "aiss"}, {"starcade.arcade/launcher", "starcade"}}) {
        const auto folder = Root() / "views" / id;
        fs::create_directories(folder);
        fs::copy_file(fixtures / (fixture + ".manifest.json"), folder / "manifest.json");
    }
    OSFUI::ViewManager views;
    DiscoverViews(views);
    CHECK(views.All().size() == 3);
    CHECK(views.Find("devilzdad.devilzshop/esm-explorer")->pausesGame);
    CHECK(!views.Find("starcade.arcade/launcher")->pausesGame);
    CHECK(views.Find("starcade.arcade/launcher")->launcherMod == "starcade.arcade");
    CHECK(views.Find("x2357aiss.companionlog/companionlog")->launcherMod.empty());
    OSFUI::ViewManager mixed;
    OSFUI::ViewManifest modern;
    modern.id = "starcade.arcade/launcher";
    modern.title = "Modern wins";
    mixed.AddIfAbsent(modern);
    DiscoverViews(mixed);
    CHECK(mixed.All().size() == 3);
    CHECK(!mixed.Find(modern.id)->legacy && mixed.Find(modern.id)->title == modern.title);
    fs::remove_all(OSFUI::Paths::DataDir().parent_path().parent_path());
    std::cout << g_checks << " compatibility checks, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
