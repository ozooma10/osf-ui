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

		// Fired after the registry SHAPE changes post-load (a RegisterSchema
		// commit or RemoveMod — whenever Generation() moves outside LoadAll).
		// The web layer re-broadcasts `settings.data` off this so an open
		// settings menu re-renders on late registration (mcm-design.md §8.5).
		using RegistryListener = std::function<void()>;

		// Resolves a key NAME ("F10", "Grave", ...) to a physical key id (a
		// Windows VK code in practice; 0 = unresolvable). Injected by the
		// composition root (Runtime wires input's ResolveKeyName) so the store
		// can group key-typed settings by PHYSICAL key — the informational
		// conflict view in Data() (mcm-design.md §9) — without depending on
		// the input layer itself. Unset: Data() emits no conflict data.
		using KeyNameResolver = std::function<std::uint32_t(std::string_view a_name)>;

		// One key-typed setting's identity + current value (the key NAME
		// string — which may or may not resolve). The HotkeyService registry
		// and the conflict grouping both enumerate these.
		struct KeySetting
		{
			std::string modId;
			std::string key;
			std::string name;  // current value
		};

		// Fired after a mod's values file WRITE lands (the write-behind flush,
		// not the commit — Set/Reset notify through ChangeListener immediately).
		// The web layer pushes `settings.persisted` off this so the settings
		// view can show save feedback.
		using PersistListener = std::function<void(std::string_view a_modId)>;

		// Flushes any pending write-behind values (never lose a committed
		// change on clean teardown).
		~SettingsStore();
		SettingsStore() = default;
		SettingsStore(const SettingsStore&) = delete;
		SettingsStore& operator=(const SettingsStore&) = delete;

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
		void AddRegistryListener(RegistryListener a_listener) { _registryListeners.push_back(std::move(a_listener)); }
		void AddPersistListener(PersistListener a_listener) { _persistListeners.push_back(std::move(a_listener)); }

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

		// Pure, ANY thread: the shape/id gate AddSchema applies — rejects (with
		// a warning) a non-object document and a missing/invalid/reserved "id".
		// Deeper field problems are NOT errors; they fall back defensively at
		// registration. The C ABI (BridgeApi::RegisterSettingsSchema) reports
		// these synchronously with this before queueing the main-thread merge.
		[[nodiscard]] static bool ValidateSchemaShape(const nlohmann::json& a_schema);

		// The Source a mod registered from, or nullopt on unknown id. Lets the
		// ABI unregister path refuse to drop schemas it does not own.
		[[nodiscard]] std::optional<Source> GetSource(std::string_view a_modId) const;

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

		// Every `type:"key"` setting across all mods, with its current value.
		// Empty-key and non-string-valued entries are skipped (defensive).
		[[nodiscard]] std::vector<KeySetting> KeySettings() const;

		void SetKeyNameResolver(KeyNameResolver a_resolver) { _keyResolver = std::move(a_resolver); }

		// Monotonic counter bumped on every registry shape change (LoadAll,
		// RegisterSchema, RemoveMod). Consumers re-broadcast `settings.data`
		// when it moves (mcm-design.md §8.5).
		[[nodiscard]] std::uint64_t Generation() const { return _generation; }

		// The document the settings view consumes:
		// { "mods": [ { id, title, schema, values }, ... ] }. Data() returns
		// the json object (for native senders — no dump/re-parse round trip);
		// DataJson() is its serialized form (tests, logging).
		// With a KeyNameResolver set, every key-typed setting whose current
		// value collides with another key-typed setting (same resolved
		// physical key, any mod) carries `conflicts: [{mod, key, title}]` in
		// its emitted schema object — INFORMATIONAL only (mcm-design.md §9):
		// the renderer badges both sides; a colliding bind is never rejected.
		[[nodiscard]] nlohmann::json Data() const;
		[[nodiscard]] std::string    DataJson() const;

		// Validate + clamp + store + notify. a_valueJson is the raw JSON text
		// of the value. Returns false on unknown mod/key or bad type (false =
		// nothing committed). Persistence is write-behind: the commit and the
		// notification are immediate, the disk write lands via PumpPersistence.
		bool Set(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);

		// Restore defaults: one key, or the whole mod when a_key is empty.
		// Notifies; persistence is write-behind like Set. Under sparse
		// persistence a reset key simply leaves the values file. Returns false
		// on unknown mod/key.
		bool Reset(std::string_view a_modId, std::string_view a_key);

		// Debounced write-behind (mcm-design.md §8.1): a committed Set/Reset
		// opens (or joins) a per-mod ~500ms window; the pump writes the mod
		// once the window elapses, so a slider drag costs one disk write per
		// window instead of one atomic tmp+rename per step. a_nowSeconds is a
		// caller-owned monotonic clock (Runtime::Tick passes its uptime) —
		// call every main tick.
		static constexpr double kPersistDelaySeconds = 0.5;
		void PumpPersistence(double a_nowSeconds);

		// Write every dirty mod NOW — menu close (the user just finished
		// editing) and teardown (~SettingsStore).
		void FlushPersistence();

	private:
		struct Mod
		{
			std::string           id;
			nlohmann::json        schema;  // read-only
			nlohmann::json        values;  // { key: current value }
			std::filesystem::path valuesPath;
			Source                source{ Source::kDropIn };
			bool                  dirty{ false };  // has unflushed write-behind changes
			double                dueAt{ 0.0 };    // when the open window flushes (store clock)
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
		// The values that go to disk: only ≠ schema default (sparse, §8.1).
		[[nodiscard]] static nlohmann::json SparseValues(const Mod& a_mod);
		// Open (or join) the mod's write-behind window; PumpPersistence lands it.
		void        MarkDirty(Mod& a_mod);
		// The one flush path: clear dirty, write, log + fire persist listeners
		// on success. Every site that lands a values file goes through here.
		void        PersistNow(Mod& a_mod) const;
		static bool Persist(const Mod& a_mod);
		void        Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const;
		void        NotifyRegistryChanged() const;

		std::vector<Mod>              _mods;
		KeyNameResolver               _keyResolver;
		std::vector<ChangeListener>   _listeners;
		std::vector<RegistryListener> _registryListeners;
		std::vector<PersistListener>  _persistListeners;
		std::filesystem::path       _valuesDir;
		std::uint64_t               _generation{ 0 };
		bool                        _loaded{ false };
		double                      _now{ 0.0 };  // last PumpPersistence clock; MarkDirty stamps windows with it
	};
}
