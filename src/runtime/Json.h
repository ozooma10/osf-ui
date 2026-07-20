#pragma once

#include <nlohmann/json.hpp>

// JSON helpers. All parsing goes through these (no exceptions escape) and
// through typed Get*() accessors that fall back to defaults on missing keys or
// wrong types. JSON here is mod-provided content: untrusted input, never a
// reason to crash the game.

namespace OSFUI::Json
{
	using Value = nlohmann::json;

	// Parses text; returns std::nullopt (and logs `a_sourceName`) on failure.
	[[nodiscard]] std::optional<Value> Parse(std::string_view a_text, std::string_view a_sourceName);

	// Reads and parses a file; returns std::nullopt on missing file or bad JSON.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path);

	// ParseFile with a user-facing failure reason (settings load-error banner):
	// a_outError receives "cannot open file" or nlohmann's parse message with
	// line/column, and is empty exactly when parsing succeeded. Does not log —
	// the caller owns severity and wording.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path, std::string& a_outError);

	[[nodiscard]] std::string GetString(const Value& a_obj, std::string_view a_key, std::string_view a_default);
	[[nodiscard]] bool        GetBool(const Value& a_obj, std::string_view a_key, bool a_default);
	[[nodiscard]] std::int64_t GetInt(const Value& a_obj, std::string_view a_key, std::int64_t a_default);

	// Typo/format-skew diagnostics: logs every key of a_obj not in a_known.
	// The caller picks the level via a_warn — true = WARN for host-owned files
	// (config.json, vanillakeys*.json, where an unknown key can only be a typo);
	// false = INFO for author-shipped files, where a newer mod on an older host
	// makes unknown keys the normal compatible case (gate that call on devMode).
	// Never rejects; lenient parsing is the contract.
	void ReportUnknownKeys(const Value& a_obj, std::initializer_list<std::string_view> a_known, std::string_view a_sourceName, bool a_warn);
}
