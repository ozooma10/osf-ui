#include "Compat/V1/LegacyViews.h"
#include "Compat/V1/SettingsCodec.h"
#include "Views/ViewManager.h"
#include "Core/Json.h"
#include "Core/Ids.h"

namespace OSFUI::Compat::V1
{
    void DiscoverViews(ViewManager& views)
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator mods(Root() / "views", ec), end; !ec && mods != end; mods.increment(ec)) {
            if (!mods->is_directory(ec) || !Ids::IsValidModId(mods->path().filename().string())) continue;
            std::error_code viewError;
            for (std::filesystem::directory_iterator it(mods->path(), viewError); !viewError && it != end; it.increment(viewError)) {
                if (!it->is_directory(viewError)) continue;
                const auto path = it->path() / "manifest.json";
                if (!std::filesystem::exists(path, viewError)) continue;
                auto json = Json::ParseFile(path);
                if (!json || !json->is_object()) continue;
                if (json->contains("manifestVersion")) {
                    REX::WARN("Legacy view '{}' declares a modern manifest; move it to OSF/UI/views", path.string());
                    continue;
                }
                (*json)["manifestVersion"] = 1;
                if (Json::Get(*json, "hub", false)) {
                    (*json)["launcher"] = {{"modId", mods->path().filename().string()},
                        {"modTitle", Json::Get(*json, "title", mods->path().filename().string())}};
                }
                if (auto manifest = ViewManifest::Parse(path, *json)) {
                    manifest->legacy = true;
                    REX::INFO("Legacy view '{}' discovered", manifest->id);
                    views.AddIfAbsent(std::move(*manifest));
                }
            }
        }
    }
}
