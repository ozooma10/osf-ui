#pragma once

#include <nlohmann/json.hpp>

namespace OSFUI
{
	// Schema-driven settings registry (docs/mcm-design.md). Each mod ships a
	// read-only JSON schema — a `settings/<id>.json` drop-in file or the same
	// document registered at runtime over the native bridge. The runtime renders
	// all of them via the built-in `settings` view, persists each mod's user
	// values to its own writable file, and notifies native consumers of changes.
	//
	// Schema shape (bad fields fall back, never crash):
	//   { "id": str, "title": str,
	//     "targetVersion": "1.1.0"   (advisory authored-against OSF UI version)
	//     "groups": [ { "label": str,
	//                   "settings": [ { "key": str, "label": str,
	//                                   "type": "bool"|"int"|"float"|"enum"|"string"|"key"|"flags",
	//                                   "default": <typed>,
	//                                   "min"/"max"/"step": num   (int/float),
	//                                   "options": [str, ...]      (enum/flags) } ] } ] }
	// The base type set is frozen pre-1.0: post-1.0 extension is a base type +
	// `widget` + attributes, and a schema using a newer base type declares the
	// matching `targetVersion` so older hosts badge "needs update". A setting
	// whose type this host doesn't know serves its schema default read-only;
	// the user's saved value is preserved opaquely (never wiped, never served).
	//
	// Security: writes only ever touch a setting declared by some loaded schema,
	// with the value validated/clamped to that setting's declared type/range and
	// persisted to that mod's own values file. Untrusted JS cannot write
	// arbitrary keys, out-of-range values, or other paths
	// (docs/security-model.md).
	//
	// Threading: main thread only. Any-thread consumers (C ABI getters,
	// Papyrus) read a mirror maintained by a change listener, never the store.
	class SettingsStore
	{
	public:
		// Where a schema came from — decides collision precedence: a runtime
		// (DLL) registration replaces a drop-in file for the same id, never the
		// other way around.
		enum class Source
		{
			kDropIn,  // settings/<id>.json scanned by LoadAll
			kNative,  // RegisterSettingsSchema over the C ABI
		};

		// Fired (on the calling thread) for every committed value — on Set,
		// Reset, once per current value via NotifyAll/NotifyMod, and on the
		// per-mod replay after an incremental RegisterSchema.
		using ChangeListener = std::function<void(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)>;

		// Fired after the registry shape changes post-load (RegisterSchema commit
		// or RemoveMod — whenever Generation() moves outside LoadAll). The web
		// layer re-broadcasts `settings.data` off this so an open settings menu
		// re-renders on late registration.
		using RegistryListener = std::function<void()>;

		// Resolves a key name ("F10", "Grave", ...) to a physical key id (a
		// Windows VK code in practice; 0 = unresolvable). Injected by Runtime
		// (wiring input's ResolveKeyName) so the store can group key-typed
		// settings by physical key — the informational conflict view in Data() —
		// without depending on the input layer. Unset: Data() emits no conflict
		// data.
		using KeyNameResolver = std::function<std::uint32_t(std::string_view a_name)>;
		// Authored English -> active-locale text at a stable structural address.
		// Injected by Runtime so this host-testable store does not own locale or
		// filesystem policy. Resolution only touches emitted schema copies;
		// validation and persistence always use the authored schema.
		using TextResolver = std::function<std::string(std::string_view a_modId,
			std::string_view a_address,
			std::string_view a_authoredEnglish)>;

		// One key-typed setting's identity + current value (the key name string,
		// which may or may not resolve). Enumerated by both the HotkeyService
		// registry and the conflict grouping.
		struct KeySetting
		{
			std::string modId;
			std::string key;
			std::string name;  // current value
		};

		// Fired after a mod's values file write lands (the write-behind flush,
		// not the commit — Set/Reset notify through ChangeListener immediately).
		// The web layer pushes `settings.persisted` off this for save feedback.
		using PersistListener = std::function<void(std::string_view a_modId)>;

		// Flushes any pending write-behind values, so a clean teardown cannot
		// lose a committed change.
		~SettingsStore();
		SettingsStore() = default;
		SettingsStore(const SettingsStore&) = delete;
		SettingsStore& operator=(const SettingsStore&) = delete;

		// Loads every `<schemaDir>/*.json` as a mod schema (sorted by filename
		// so duplicate-id resolution is deterministic); each mod's values
		// persist to `<valuesDir>/<id>.json`. Safe to call once, before any
		// RegisterSchema.
		void LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir);

		// Listeners cannot be removed: subscribers are process-lifetime
		// components (runtime reaction, web push, ABI mirror) that multiplex
		// their own dynamic subscribers on top.
		void AddChangeListener(ChangeListener a_listener) { _listeners.push_back(std::move(a_listener)); }
		void AddRegistryListener(RegistryListener a_listener) { _registryListeners.push_back(std::move(a_listener)); }
		void AddPersistListener(PersistListener a_listener) { _persistListeners.push_back(std::move(a_listener)); }

		// Incrementally registers (or, per Source precedence, replaces) one mod
		// schema after LoadAll. Same document shape as a drop-in file; must
		// carry a non-empty "id". Persisted values overlay from the same per-mod
		// values file as the drop-in tier, so a mod can migrate tiers without
		// losing user settings. Bumps the registry generation and replays the
		// mod's current values through the listeners, so late subscribers sync
		// without a read step. Returns false on a shape error or when an
		// existing registration takes precedence.
		bool RegisterSchema(nlohmann::json a_schema, Source a_source);

		// Drops a mod from the registry; the values file on disk is kept, since
		// uninstalled is indistinguishable from temporarily-disabled under MO2.
		// Bumps the generation. False if unknown.
		bool RemoveMod(std::string_view a_modId);

		// Pure, callable from any thread: the shape/id gate AddSchema applies —
		// rejects (with a warning) a non-object document and a
		// missing/invalid/reserved "id". Deeper field problems are not errors;
		// they fall back at registration. BridgeApi::RegisterSettingsSchema uses
		// this to report synchronously before queueing the main-thread merge.
		[[nodiscard]] static bool ValidateSchemaShape(const nlohmann::json& a_schema);

		// The Source a mod registered from, or nullopt on unknown id. Lets the
		// ABI unregister path refuse to drop schemas it does not own.
		[[nodiscard]] std::optional<Source> GetSource(std::string_view a_modId) const;

		// Pushes every current value (all mods / one mod) through the listeners,
		// e.g. to apply persisted settings at startup.
		void NotifyAll() const;
		void NotifyMod(std::string_view a_modId) const;

		// Current value, or nullptr on unknown mod/key. Pointer is valid only
		// until the next store mutation; copy, don't keep.
		[[nodiscard]] const nlohmann::json* GetValue(std::string_view a_modId, std::string_view a_key) const;

		// Declared "type" of a setting ("bool", "int", ... "key"), or "" on
		// unknown mod/key. Lets feature code gate on schema facts (key capture
		// accepts any "key"-typed setting) without parsing schemas.
		[[nodiscard]] std::string GetSettingType(std::string_view a_modId, std::string_view a_key) const;

		// The authored spelling of a_value among a_modId.a_key's declared enum
		// options, matched ASCII-case-insensitively. The Papyrus write path
		// canonicalizes with this before Set: script strings arrive through
		// BSFixedString interning with arbitrary casing (seen in-game 2026-07-17,
		// "fast" arrived recased and enum validation refused it; rationale in
		// api/SettingsMirror.h). nullopt when the setting isn't enum-typed or
		// nothing matches — the caller passes the original through and lets Set
		// refuse normally.
		[[nodiscard]] std::optional<std::string> CanonicalEnumValue(std::string_view a_modId, std::string_view a_key, std::string_view a_value) const;

		// Every `type:"key"` setting across all mods, with its current value.
		// Empty-key and non-string-valued entries are skipped.
		[[nodiscard]] std::vector<KeySetting> KeySettings() const;

		// Dev-mode schema hot-reload: re-parse one drop-in settings/<id>.json and
		// replace the same-id registered schema in place. Values survive — a
		// dirty write-behind window flushes first, then the overlay re-reads the
		// values file exactly like startup, so key aliases apply to a live
		// rename too. A runtime (native) registration still outranks the file
		// (refused, same precedence as startup); an unseen id registers as a
		// fresh drop-in. Notifies like RegisterSchema (value replay + registry
		// re-broadcast). Returns false on unparseable/invalid schema or refusal.
		bool ReloadDropInFile(const std::filesystem::path& a_path);

		void SetKeyNameResolver(KeyNameResolver a_resolver)
		{
			_keyResolver = std::move(a_resolver);
			InvalidateData();
		}
		void SetTextResolver(TextResolver a_resolver)
		{
			_textResolver = std::move(a_resolver);
			InvalidateData();
		}
		// Locale/catalog changes do not mutate schemas or values, but they do
		// invalidate the resolved document cached for web/native readers.
		void InvalidateLocalizedData() { InvalidateData(); }

		// The game's own key bindings, already resolved to VKs by the composition
		// root (VanillaKeys). They join the conflict grouping as pseudo-entries
		// under the reserved mod id "@game"; they can never be a setting's self,
		// so they only ever appear as the other side of a collision (Data()
		// badges and ConflictsFor()). The HotkeyService registry is untouched —
		// it reads KeySettings().
		struct VanillaKey
		{
			std::string   event;  // conflict entry `key` ("QuickSave")
			std::string   title;  // conflict entry `title` ("Starfield (Quicksave)")
			std::uint32_t vk;
			// Display key name ("F5", canonical KeyName spelling) — emitted in
			// Data()'s top-level `vanillaKeys` so the keybinds view can render
			// the game's full keyboard map, not just colliding entries.
			std::string   name;
		};
		void SetVanillaKeys(std::vector<VanillaKey> a_keys)
		{
			_vanillaKeys = std::move(a_keys);
			InvalidateData();
		}

		// Monotonic counter bumped on every registry shape change (LoadAll,
		// RegisterSchema, RemoveMod). Consumers re-broadcast `settings.data`
		// when it moves.
		[[nodiscard]] std::uint64_t Generation() const { return _generation; }

		// One settings artifact that failed to load — named rather than silently
		// dropped. Surfaced additively in Data() as top-level `loadErrors` so
		// the Mods surface can render a banner. `kind` is a stable enum string:
		//   "schema-name"   filename stem fails the mod-id grammar; file skipped
		//   "schema-parse"  schema file is unreadable/not an object; file skipped
		//   "values-parse"  the mod's values file was corrupt — quarantined to
		//                   <id>.json.bad, defaults served (the mod still loads)
		struct LoadError
		{
			std::string kind;
			std::string file;     // filename only (schema or values file)
			std::string mod;      // owning mod id; empty when no schema loaded
			std::string message;  // human-readable reason (parse position etc.)
		};
		[[nodiscard]] const std::vector<LoadError>& LoadErrors() const { return _loadErrors; }

		// The document the settings view consumes:
		// { "mods": [ { id, title, schema, values }, ... ] }. Data() returns the
		// json object (for native senders — no dump/re-parse round trip);
		// DataJson() is its serialized form (tests, logging).
		// With a KeyNameResolver set, every key-typed setting whose current value
		// collides with another key-typed setting (same resolved physical key,
		// any mod) carries `conflicts: [{mod, key, title}]` in its emitted schema
		// object — informational only. A named input context with blocksGameplay
		// omits @game entries as expected reuse; mod-to-mod collisions are never
		// omitted.
		[[nodiscard]] nlohmann::json Data() const;
		// Cached read-only form for internal consumers that serialize or inspect
		// immediately. The reference is invalidated by the next store mutation.
		[[nodiscard]] const nlohmann::json& DataView() const;
		[[nodiscard]] std::string    DataJson() const;

		// The other key-typed settings currently bound to physical key a_vk —
		// `[{mod, key, title}]`, excluding a_excludeMod.a_excludeKey (the setting
		// being rebound, whose stored value is still the old binding). Live-warn
		// during capture: answers a mid-rebind key press with the collisions the
		// bind would create, before the view commits it. Empty on a unique key,
		// a_vk == 0, or no resolver; informational only, like the Data()
		// annotation. The rebound setting's input context applies the same @game
		// filtering as Data().
		[[nodiscard]] nlohmann::json ConflictsFor(std::uint32_t a_vk, std::string_view a_excludeMod, std::string_view a_excludeKey) const;

		// Validate + clamp + store + notify. a_valueJson is the raw JSON text
		// of the value. Returns false on unknown mod/key or bad type (false =
		// nothing committed). Persistence is write-behind: the commit and the
		// notification are immediate, the disk write lands via PumpPersistence.
		bool Set(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);

		// Set with a machine-readable refusal code for the web ack. Codes are
		// stable enum strings:
		//   "unknown-setting"  mod or key not declared by any loaded schema
		//   "read-only"        requires-gated stub, or a setting whose type
		//                      this host doesn't know (served default)
		//   "invalid-value"    unparseable JSON or validation refused
		// ok == true ⇔ code empty ⇔ a value was committed (read the
		// authoritative post-clamp value back via GetValue).
		struct SetResult
		{
			bool        ok{ false };
			std::string code;
		};
		[[nodiscard]] SetResult SetWithResult(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);
		// Parsed-value overload for callers that already decoded a containing
		// message (notably settings.set); avoids dump + parse of the value.
		[[nodiscard]] SetResult SetValueWithResult(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// The changed setting's current conflict list — ConflictsFor() on its
		// committed value (resolved through the key resolver), same shape and
		// @game filtering as Data()'s annotation. Empty array on a non-key/
		// unbound/unresolvable setting or when no resolver is set. Emitted with
		// `settings.changed` for key-typed settings so views update badges
		// without a full registry re-fetch.
		[[nodiscard]] nlohmann::json ConflictsForSetting(std::string_view a_modId, std::string_view a_key) const;

		// Restore defaults: one key, or the whole mod when a_key is empty.
		// Notifies; persistence is write-behind like Set. Under sparse
		// persistence a reset key leaves the values file. Returns false on
		// unknown mod/key.
		bool Reset(std::string_view a_modId, std::string_view a_key);

		// Debounced write-behind: a committed Set/Reset opens (or joins) a
		// per-mod ~500ms window; the pump writes the mod once the window
		// elapses, so a slider drag costs one disk write per window instead of
		// one atomic tmp+rename per step. a_nowSeconds is a caller-owned
		// monotonic clock (Runtime::Tick passes its uptime) — call every main
		// tick.
		static constexpr double kPersistDelaySeconds = 0.5;
		void PumpPersistence(double a_nowSeconds);

		// Write every dirty mod immediately — menu close and teardown
		// (~SettingsStore).
		void FlushPersistence();

	private:
		struct Mod
		{
			std::string           id;
			nlohmann::json        schema;  // read-only
			nlohmann::json        values;  // { key: current value }
			// Forward-compat: saved entries this host cannot understand —
			// unknown-typed settings' values and keys no schema declares —
			// round-trip verbatim through every rewrite. Never served;
			// consumers only see store-validated `values`, and a newer host
			// re-adopts these.
			nlohmann::json        preserved;  // { key: opaque saved value }
			// OSF UI version this schema was authored against (same advisory
			// field as ViewManifest::targetVersion). Never gates — the schema
			// loads best-effort — but the Mods surface badges "needs update"
			// when it is newer than the running OSF UI. Empty when undeclared
			// or malformed.
			std::string              targetVersion;
			// Values-file encoding stamp: max(build's version, the loaded
			// file's), so a newer host's stamp survives our rewrites.
			std::int64_t             formatVersion{ 1 };
			std::filesystem::path valuesPath;
			std::filesystem::path schemaPath;  // drop-in source file; empty for runtime registrations
			// Drop-in files that also claimed this id and were skipped
			// (first wins). Surfaced additively in Data() so the Mods surface
			// can badge the conflict.
			std::vector<std::string> shadowed;
			Source                source{ Source::kDropIn };
			bool                  dirty{ false };  // has unflushed write-behind changes
			double                dueAt{ 0.0 };    // when the open window flushes (store clock)
		};

		// Shared add/replace path for LoadAll, RegisterSchema, and
		// ReloadDropInFile: id resolution (a_idHint = filename stem for
		// drop-ins), Source precedence, persisted-value overlay, generation
		// bump. Notifies the mod's values when a_notify (startup load defers to
		// NotifyAll). a_dropInReplace relaxes one precedence rule for the dev
		// hot-reload: a drop-in may replace a same-source registration (its own
		// earlier file), never a runtime one.
		bool AddSchema(nlohmann::json a_schema, Source a_source, std::string a_idHint, bool a_notify, bool a_dropInReplace = false, std::filesystem::path a_sourcePath = {});

		[[nodiscard]] Mod*       FindMod(std::string_view a_modId);
		[[nodiscard]] const Mod* FindMod(std::string_view a_modId) const;
		[[nodiscard]] static const nlohmann::json* FindSetting(const Mod& a_mod, std::string_view a_key);
		[[nodiscard]] static std::optional<nlohmann::json> Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value);
		[[nodiscard]] static nlohmann::json DefaultFor(const nlohmann::json& a_setting);
		// Authored key context (metadata-only v1). The "gameplay" context is
		// implicit; named contexts are local to one mod and may declare that
		// Starfield's curated gameplay bindings are unavailable.
		struct InputContext
		{
			std::string id{ "gameplay" };
			std::string label{ "Gameplay" };
			bool        blocksGameplay{ false };
		};
		[[nodiscard]] InputContext ResolveInputContext(const Mod& a_mod, const nlohmann::json& a_setting) const;
		static void WarnInputContexts(const nlohmann::json& a_schema, std::string_view a_modId);
		// One key-typed setting whose current value resolved to a physical key.
		// Shared by the Data() conflict annotation and ConflictsFor().
		struct BoundKey
		{
			std::string   modId;
			std::string   key;
			std::string   title;
			std::uint32_t vk;
			bool          blocksGameplay{ false };
		};
		[[nodiscard]] std::vector<BoundKey> ResolveBoundKeys() const;
		// The values that go to disk: sparse — only those ≠ schema default.
		[[nodiscard]] static nlohmann::json SparseValues(const Mod& a_mod);
		// Open (or join) the mod's write-behind window; PumpPersistence lands it.
		void        MarkDirty(Mod& a_mod);
		// The one flush path: clear dirty, write, log + fire persist listeners on
		// success. Every site that lands a values file goes through here.
		void        PersistNow(Mod& a_mod) const;
		static bool Persist(const Mod& a_mod);
		void        Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const;
		void        NotifyRegistryChanged() const;
		void        InvalidateData() { _dataCache.reset(); }
		// Replace-or-add keyed on (kind, file, mod) — a hot-reload retry of a
		// still-broken file must update its entry, not stack duplicates.
		void        RecordLoadError(std::string a_kind, std::string a_file, std::string a_mod, std::string a_message);
		// True (and data invalidated) when entries were dropped: schema-kind
		// entries for a file that now loads / values-kind entries for a mod
		// whose values now overlay clean (or that left the registry).
		bool        EraseLoadErrorsForFile(std::string_view a_file);
		bool        EraseLoadErrorsForMod(std::string_view a_modId);

		std::vector<Mod>              _mods;
		KeyNameResolver               _keyResolver;
		TextResolver                  _textResolver;
		std::vector<VanillaKey>       _vanillaKeys;
		std::vector<ChangeListener>   _listeners;
		std::vector<RegistryListener> _registryListeners;
		std::vector<PersistListener>  _persistListeners;
		mutable std::optional<nlohmann::json> _dataCache;
		std::vector<LoadError>      _loadErrors;
		std::filesystem::path       _valuesDir;
		std::uint64_t               _generation{ 0 };
		bool                        _loaded{ false };
		double                      _now{ 0.0 };  // last PumpPersistence clock; MarkDirty stamps windows with it
	};
}
