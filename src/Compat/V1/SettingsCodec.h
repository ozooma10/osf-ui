#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace OSFUI::Compat::V1
{
    using Document = nlohmann::ordered_json;
    std::uint32_t KeyCode(std::string_view name);
    std::string KeyName(std::uint32_t code);
    struct SettingsDefinition
    {
        std::string id;
        Document schema;
        Document defaults;
        Document values;
        Document preserved;
        std::vector<std::string> keys;
        std::int64_t version{};
        std::int64_t formatVersion{1};
        Document Encode(const Document& values) const;
    };
    std::optional<SettingsDefinition> TranslateSettings(const Document& schema, const Document& saved, std::string& error);
    std::filesystem::path Root();
    bool SaveValues(const SettingsDefinition& definition, const Document& values);
}
