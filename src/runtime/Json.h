#pragma once

#include <nlohmann/json.hpp>

// Defensive JSON helpers. All parsing in this project goes through ParseJson()
// (no exceptions escape) and typed Get*() accessors that fall back to defaults
// on missing keys or wrong types. JSON here is mod-provided content: treat it
// as untrusted input, never as a reason to crash the game.

namespace OSFUI::Json
{
	using Value = nlohmann::json;

	// Parses text; returns std::nullopt (and logs `a_sourceName`) on failure.
	[[nodiscard]] std::optional<Value> Parse(std::string_view a_text, std::string_view a_sourceName);

	// Reads and parses a file; returns std::nullopt on missing file or bad JSON.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path);

	// ParseFile with the failure REASON for user-facing surfacing (settings
	// load-error banner): a_outError receives "cannot open file" or nlohmann's
	// parse message including line/column. Does not log — the caller owns
	// severity and wording. a_outError is empty exactly when parsing succeeded.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path, std::string& a_outError);

	[[nodiscard]] std::string GetString(const Value& a_obj, std::string_view a_key, std::string_view a_default);
	[[nodiscard]] bool        GetBool(const Value& a_obj, std::string_view a_key, bool a_default);
	[[nodiscard]] std::int64_t GetInt(const Value& a_obj, std::string_view a_key, std::int64_t a_default);

	// Typo/format-skew diagnostics (api-freeze-plan item 8): log every key of
	// a_obj that is not in a_known. The CALLER picks the level via a_warn —
	// true = WARN (host-owned files: config.json, vanillakeys*.json — there is
	// no legitimate version-skew source of unknown keys, so an unknown key is
	// a typo); false = INFO (author-shipped files — a newer mod on an older
	// host makes unknown keys the NORMAL compatible case; call gated on
	// devMode). Never rejects — lenient parsing stays the contract.
	void ReportUnknownKeys(const Value& a_obj, std::initializer_list<std::string_view> a_known, std::string_view a_sourceName, bool a_warn);
}
