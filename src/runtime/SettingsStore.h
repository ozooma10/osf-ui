#pragma once

#include <nlohmann/json.hpp>

namespace PrismaSF
{
	// Schema-driven settings registry (renderer-plan.md Phase 5). Each mod
	// ships a read-only JSON schema (one `settings/<id>.json` file); the
	// runtime renders all of them via the built-in `settings` view, persists
	// each mod's user values to its own writable file, and notifies native
	// consumers of changes so settings actually DO something.
	//
	// Schema shape (defensive — bad fields fall back, never crash):
	//   { "id": str, "title": str,
	//     "groups": [ { "label": str,
	//                   "settings": [ { "key": str, "label": str,
	//                                   "type": "bool"|"int"|"float"|"enum"|"string",
	//                                   "default": <typed>,
	//                                   "min"/"max"/"step": num   (int/float),
	//                                   "options": [str, ...]      (enum) } ] } ] }
	//
	// Security: writes only ever touch a setting that EXISTS in some loaded
	// schema, with the value validated/clamped to that setting's declared
	// type/range, persisted to that mod's own values file. Untrusted JS can't
	// write arbitrary keys, out-of-range values, or other paths
	// (docs/security-model.md).
	class SettingsStore
	{
	public:
		// Fired (on the calling thread) for every committed value — on Set,
		// Reset, and once per current value via NotifyAll. Lets native code
		// react to settings.
		using ChangeListener = std::function<void(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)>;

		// Loads every `<schemaDir>/*.json` as a mod schema; each mod's values
		// persist to `<valuesDir>/<id>.json`. Safe to call once.
		void LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir);

		void SetChangeListener(ChangeListener a_listener) { _listener = std::move(a_listener); }

		// Pushes every current value through the listener (e.g. to apply
		// persisted settings at startup).
		void NotifyAll() const;

		// JSON the settings view consumes: { "mods": [ { id, title, schema, values }, ... ] }.
		[[nodiscard]] std::string DataJson() const;

		// Validate + clamp + store + persist + notify. a_valueJson is the raw
		// JSON text of the value. Returns false on unknown mod/key or bad type.
		bool Set(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);

		// Restore defaults: one key, or the whole mod when a_key is empty.
		// Persists + notifies. Returns false on unknown mod/key.
		bool Reset(std::string_view a_modId, std::string_view a_key);

	private:
		struct Mod
		{
			std::string           id;
			nlohmann::json        schema;  // read-only
			nlohmann::json        values;  // { key: current value }
			std::filesystem::path valuesPath;
		};

		[[nodiscard]] Mod*       FindMod(std::string_view a_modId);
		[[nodiscard]] const Mod* FindMod(std::string_view a_modId) const;
		[[nodiscard]] static const nlohmann::json* FindSetting(const Mod& a_mod, std::string_view a_key);
		[[nodiscard]] static std::optional<nlohmann::json> Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value);
		[[nodiscard]] static nlohmann::json DefaultFor(const nlohmann::json& a_setting);
		static bool Persist(const Mod& a_mod);
		void        Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const;

		std::vector<Mod> _mods;
		ChangeListener   _listener;
	};
}
