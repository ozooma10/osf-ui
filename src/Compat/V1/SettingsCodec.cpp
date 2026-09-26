#include "Compat/V1/SettingsCodec.h"
#include "Core/Paths.h"
#include "Core/Ids.h"
#include <Windows.h>
#undef ERROR
#include <fstream>
#include <set>
#include <stdexcept>

namespace OSFUI::Compat::V1
{
    namespace
    {
        void Require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
        bool ValidValue(const Document& setting, const Document& value)
        {
            const auto type = setting.at("type").get<std::string>();
            if (type == "bool") return value.is_boolean();
            if (type == "enum") return value.is_string() && setting.at("options").contains(value.get<std::string>());
            if (type == "string") return value.is_string();
            if (type == "key") return value.is_number_unsigned() || value.is_number_integer();
            if (type == "int" && !value.is_number_integer()) return false;
            if (type == "float" && !value.is_number()) return false;
            return (!setting.contains("min") || value >= setting["min"]) && (!setting.contains("max") || value <= setting["max"]);
        }
    }

    std::optional<SettingsDefinition> TranslateSettings(const Document& source, const Document& saved, std::string& error)
    {
        try {
            Require(source.is_object() && source.contains("id") && source.at("id").is_string(), "schema needs an id");
            SettingsDefinition result;
            result.id = source.at("id").get<std::string>();
            Require(Ids::IsValidModId(result.id) && result.id != "osfui" && result.id != "internal", "invalid or reserved legacy mod id");
            Require(source.contains("groups") && source.at("groups").is_array(), "legacy groups must be an array");
            result.schema = {{"id", result.id}, {"title", source.value("title", result.id)},
                {"description", source.value("description", "")}, {"groups", Document::object()}};
            result.defaults = result.values = Document::object();
            result.preserved = saved.is_object() ? saved : Document::object();
            result.version = source.value("version", std::int64_t{});
            if (saved.is_object() && saved.contains("$formatVersion") && saved["$formatVersion"].is_number_integer())
                result.formatVersion = std::max<std::int64_t>(1, saved["$formatVersion"].get<std::int64_t>());
            result.preserved.erase("$formatVersion");
            result.preserved.erase("$schemaVersion");
            std::set<std::string> seen;
            for (const auto& group : source.at("groups")) {
                auto label = group.value("label", group.value("id", std::string("General")));
                const auto base = label;
                for (int suffix = 2; result.schema["groups"].contains(label); ++suffix) label = base + " (" + std::to_string(suffix) + ")";
                Require(group.contains("settings") && group.at("settings").is_array(), "legacy group needs settings array");
                auto controls = Document::array();
                for (const auto& authored : group.at("settings")) {
                    const auto key = authored.at("key").get<std::string>();
                    Require(!key.empty() && key.front() != '$' && seen.insert(key).second, "invalid or duplicate setting key");
                    const auto type = authored.at("type").get<std::string>();
                    Require(type == "bool" || type == "int" || type == "float" || type == "enum" || type == "key" || type == "string", "unsupported legacy setting type");
                    Document setting = authored;
                    if (type == "enum") {
                        auto options = Document::object();
                        const auto& values = authored.at("options");
                        Require(values.is_array(), "legacy enum options must be an array");
                        for (std::size_t i = 0; i < values.size(); ++i) {
                            const auto option = values[i].get<std::string>();
                            options[option] = authored.contains("optionLabels") && i < authored["optionLabels"].size() ? authored["optionLabels"][i] : values[i];
                        }
                        setting["options"] = std::move(options);
                        setting.erase("optionLabels");
                    }
                    if (type == "key") {
                        setting["default"] = KeyCode(authored.at("default").get<std::string>());
                        setting["allowMouse"] = true;
                        setting["allowUnbound"] = true;
                        result.keys.push_back(key);
                    }
                    auto value = setting.at("default");
                    auto savedValue = saved.is_object() ? saved.find(key) : saved.end();
                    // The old store accepted aliases on schema updates.
                    if (savedValue == saved.end() && authored.contains("aliases")) {
                        for (const auto& alias : authored.at("aliases")) {
                            const auto name = alias.get<std::string>();
                            if (saved.is_object() && saved.contains(name)) { savedValue = saved.find(name); break; }
                        }
                    }
                    if (savedValue != saved.end()) {
                        auto candidate = *savedValue;
                        if (type == "key") candidate = candidate.is_string() ? Document(KeyCode(candidate.get<std::string>())) : value;
                        if (ValidValue(setting, candidate)) value = std::move(candidate);
                    }
                    result.defaults[key] = setting.at("default");
                    result.values[key] = std::move(value);
                    result.preserved.erase(key);
                    if (authored.contains("aliases")) for (const auto& alias : authored.at("aliases")) result.preserved.erase(alias.get<std::string>());
                    controls.push_back(std::move(setting));
                }
                result.schema["groups"][label] = std::move(controls);
            }
            return result;
        } catch (const nlohmann::json::exception& e) { error = e.what(); }
          catch (const std::runtime_error& e) { error = e.what(); }
        return std::nullopt;
    }

    Document SettingsDefinition::Encode(const Document& current) const
    {
        auto output = preserved;
        for (const auto& [key, value] : current.items()) {
            if (defaults.contains(key) && value == defaults[key]) continue;
            output[key] = std::ranges::find(keys, key) != keys.end() ? Document(KeyName(value.get<std::uint32_t>())) : value;
        }
        if (version) output["$schemaVersion"] = version;
        output["$formatVersion"] = formatVersion;
        return output;
    }

    std::filesystem::path Root() { return Paths::DataDir().parent_path().parent_path() / "OSFUI"; }

    bool SaveValues(const SettingsDefinition& definition, const Document& values)
    {
        // The frozen callers understand names, so never silently turn an
        // unrepresentable native binding into UNBOUND on disk or in callbacks.
        for (const auto& key : definition.keys) {
            const auto code = values.at(key).get<std::uint32_t>();
            if (KeyCode(KeyName(code)) != code) return false;
        }
        const auto path = Root() / "settings" / "values" / (definition.id + ".json");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        const auto temp = std::filesystem::path(path.native() + L".tmp");
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file << definition.Encode(values).dump(2) << '\n';
            file.flush();
            if (!file) return false;
        }
        if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        REX::ERROR("Legacy settings '{}' could not be saved: {}", definition.id, ::GetLastError());
        return false;
    }
}
