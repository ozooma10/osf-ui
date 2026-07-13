#pragma once

#include <nlohmann/json.hpp>

namespace OSFUI
{
	// Schema-driven settings registry (renderer-plan.md Phase 5; grown into the
	// MCM platform core, docs/mcm-design.md §8.3). Each mod ships a read-only
	// JSON schema — a `settings/<id>.json` drop-in file or the same document
	// registered at runtime over the native bridge — the runtime renders all of
	// them via the built-in `settings` view, persists each mod's user values to
	// its own writable file, and notifies native consumers of changes so
	// settings actually DO something.
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
	//
	// Threading: main thread only. Any-thread consumers (C ABI getters,
	// Papyrus) read a mirror maintained by a change listener, never the store.
	class SettingsStore
	{
	public:
		// Where a schema came from — decides collision precedence
		// (mcm-design.md §14.1): a runtime (DLL) registration replaces a
		// drop-in file for the same id, never the other way around.
		enum class Source
		{
			kDropIn,  // settings/<id>.json scanned by LoadAll
			kNative,  // RegisterSettingsSchema over the C ABI
		};

		// Fired (on the calling thread) for every committed value — on Set,
		// Reset, once per current value via NotifyAll/NotifyMod, and on the
		// per-mod replay after an incremental RegisterSchema. Lets native code
		// react to settings.
		using ChangeListener = std::function<void(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)>;

		// Loads every `<schemaDir>/*.json` as a mod schema (sorted by filename
		// so duplicate-id resolution is deterministic); each mod's values
		// persist to `<valuesDir>/<id>.json`. Safe to call once, before any
		// RegisterSchema.
		void LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir);

		// Appends a change listener; listeners cannot be removed (subscribers
		// are process-lifetime components — the runtime's core reaction, the
		// web push, the ABI mirror — which multiplex their own dynamic
		// subscribers on top).
		void AddChangeListener(ChangeListener a_listener) { _listeners.push_back(std::move(a_listener)); }

		// Incrementally registers (or, per Source precedence, replaces) one
		// mod schema after LoadAll — the runtime-registration path
		// (mcm-design.md §8.2/§8.3). The document is the SAME shape as a
		// drop-in file and must carry a non-empty "id". Persisted values
		// overlay from the same per-mod values file as the drop-in tier, so a
		// mod can migrate tiers without losing user settings. Bumps the
		// registry generation and replays the mod's current values through the
		// listeners (so late subscribers/consumers sync without a read step).
		// Returns false on a shape error or when an existing registration
		// takes precedence.
		bool RegisterSchema(nlohmann::json a_schema, Source a_source);

		// Drops a mod from the registry (values file on disk is kept —
		// uninstalled is indistinguishable from temporarily-disabled under
		// MO2, mcm-design.md §10). Bumps the generation. False if unknown.
		bool RemoveMod(std::string_view a_modId);

		// Pushes every current value (of every mod / of one mod) through the
		// listeners (e.g. to apply persisted settings at startup).
		void NotifyAll() const;
		void NotifyMod(std::string_view a_modId) const;

		// Current value, or nullptr on unknown mod/key. Pointer is valid only
		// until the next store mutation; copy, don't keep.
		[[nodiscard]] const nlohmann::json* GetValue(std::string_view a_modId, std::string_view a_key) const;

		// Declared "type" of a setting ("bool", "int", ... "key"), or "" on
		// unknown mod/key. Lets feature code gate on schema facts (e.g. key
		// capture accepts any "key"-typed setting) without parsing schemas.
		[[nodiscard]] std::string GetSettingType(std::string_view a_modId, std::string_view a_key) const;

		// Monotonic counter bumped on every registry shape change (LoadAll,
		// RegisterSchema, RemoveMod). Consumers re-broadcast `settings.data`
		// when it moves (mcm-design.md §8.5).
		[[nodiscard]] std::uint64_t Generation() const { return _generation; }

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
			Source                source{ Source::kDropIn };
		};

		// Shared add/replace path for LoadAll and RegisterSchema: id
		// resolution (a_idHint = filename stem for drop-ins), Source
		// precedence, persisted-value overlay, generation bump. Notifies the
		// mod's values when a_notify (startup load defers to NotifyAll).
		bool AddSchema(nlohmann::json a_schema, Source a_source, std::string a_idHint, bool a_notify);

		[[nodiscard]] Mod*       FindMod(std::string_view a_modId);
		[[nodiscard]] const Mod* FindMod(std::string_view a_modId) const;
		[[nodiscard]] static const nlohmann::json* FindSetting(const Mod& a_mod, std::string_view a_key);
		[[nodiscard]] static std::optional<nlohmann::json> Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value);
		[[nodiscard]] static nlohmann::json DefaultFor(const nlohmann::json& a_setting);
		static bool Persist(const Mod& a_mod);
		void        Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const;

		std::vector<Mod>            _mods;
		std::vector<ChangeListener> _listeners;
		std::filesystem::path       _valuesDir;
		std::uint64_t               _generation{ 0 };
		bool                        _loaded{ false };
	};
}
