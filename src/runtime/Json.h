#pragma once

#include <nlohmann/json.hpp>

// Defensive JSON helpers. All parsing in this project goes through ParseJson()
// (no exceptions escape) and typed Get*() accessors that fall back to defaults
// on missing keys or wrong types. JSON here is mod-provided content: treat it
// as untrusted input, never as a reason to crash the game.

namespace PrismaSF::Json
{
	using Value = nlohmann::json;

	// Parses text; returns std::nullopt (and logs `a_sourceName`) on failure.
	[[nodiscard]] std::optional<Value> Parse(std::string_view a_text, std::string_view a_sourceName);

	// Reads and parses a file; returns std::nullopt on missing file or bad JSON.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path);

	[[nodiscard]] std::string GetString(const Value& a_obj, std::string_view a_key, std::string_view a_default);
	[[nodiscard]] bool        GetBool(const Value& a_obj, std::string_view a_key, bool a_default);
	[[nodiscard]] std::int64_t GetInt(const Value& a_obj, std::string_view a_key, std::int64_t a_default);
}
