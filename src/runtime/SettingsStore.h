#pragma once

#include <nlohmann/json.hpp>

namespace SWUI
{
	// Schema-driven settings (renderer-plan.md Phase 5). A mod ships a
	// read-only JSON schema describing its settings (groups of typed entries);
	// the runtime renders it via the built-in `settings` view and persists the
	// user's values to a writable file.
	//
	// Schema shape (defensive — bad fields fall back, never crash):
	//   { "title": str,
	//     "groups": [ { "label": str,
	//                   "settings": [ { "key": str, "label": str,
	//                                   "type": "bool"|"int"|"float"|"enum"|"string",
	//                                   "default": <typed>,
	//                                   "min"/"max"/"step": num   (int/float),
	//                                   "options": [str, ...]      (enum) } ] } ] }
	//
	// Security: the only writes are to the values file, keyed by settings that
	// EXIST in the schema, with values validated/clamped to the schema's
	// type/range. Untrusted JS can't write arbitrary keys or out-of-range
	// values (see docs/security-model.md).
	class SettingsStore
	{
	public:
		// Loads the schema (read-only) and merges any persisted values over the
		// schema defaults. A missing values file is fine (defaults are used).
		// Returns false (and logs) only if the schema itself is missing/invalid.
		bool Load(const std::filesystem::path& a_schemaPath, const std::filesystem::path& a_valuesPath);

		[[nodiscard]] bool IsLoaded() const { return _loaded; }

		// JSON the settings view consumes: { "schema": <schema>, "values": { key: value } }.
		[[nodiscard]] std::string DataJson() const;

		// Validates a_value against the schema entry for a_key, clamps it, stores
		// it, and persists. Returns false if the key is unknown or the value is
		// the wrong type. a_valueJson is the raw JSON text of the value.
		bool Set(std::string_view a_key, std::string_view a_valueJson);

	private:
		[[nodiscard]] const nlohmann::json* FindSetting(std::string_view a_key) const;
		// Returns the validated/clamped value, or std::nullopt if invalid.
		[[nodiscard]] std::optional<nlohmann::json> Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value) const;
		bool Persist() const;

		nlohmann::json        _schema;  // the mod's schema (read-only)
		nlohmann::json        _values;  // { key: current value }
		std::filesystem::path _valuesPath;
		bool                  _loaded{ false };
	};
}
