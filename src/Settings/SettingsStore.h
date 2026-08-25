#pragma once

#include <nlohmann/json.hpp>

#include "Bindings/ControlMapPolicy.h"
#include "Bindings/InputModes.h"

namespace OSFUI
{
	// Schema-driven settings registry. Mods should ship read-only JSON schemas as `settings/<id>.json` drop-in files. 
	// Native registration remains temporarily available for ABI compatibility while existing mods migrate.
	class SettingsStore
	{
	public:
		enum class Source
		{
			kDropIn,
			kNative,
		};

		// Fired for commits plus startup and schema-reload replays.
		using ChangeListener = std::function<void(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)>;

		// Fired when the published document changes after load.
		using RegistryListener = std::function<void()>;

		using KeyNameResolver = std::function<std::uint32_t(std::string_view a_name)>;
		using LegacyKeyMigrator = std::function<std::string(const std::string& a_name)>;
		using TextResolver = std::function<std::string(std::string_view a_modId, std::string_view a_address, std::string_view a_authoredEnglish)>;

		struct KeySetting
		{
			std::string modId;
			std::string key;
			std::string name;  // current value
		};

		struct PapyrusHotkeyTarget
		{
			std::string script;
			std::string function;

			bool operator==(const PapyrusHotkeyTarget&) const = default;
		};

		struct HotkeyTargetIssue
		{
			std::string mod;
			std::string key;
			std::string file;
			std::string message;
		};

		using PersistListener = std::function<void(std::string_view a_modId)>;

		~SettingsStore();
		SettingsStore() = default;
		SettingsStore(const SettingsStore&) = delete;
		SettingsStore& operator=(const SettingsStore&) = delete;

		// Loads schema drop-ins and their per-mod persisted values.
		void LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir);

		void AddChangeListener(ChangeListener a_listener) { _listeners.push_back(std::move(a_listener)); }
		void AddRegistryListener(RegistryListener a_listener) { _registryListeners.push_back(std::move(a_listener)); }
		void AddPersistListener(PersistListener a_listener) { _persistListeners.push_back(std::move(a_listener)); }

		// Legacy ABI 1 registration path.
		bool RegisterSchema(nlohmann::json a_schema, Source a_source);
		bool RemoveMod(std::string_view a_modId);
		[[nodiscard]] static bool ValidateSchemaShape(const nlohmann::json& a_schema, bool a_allowBuiltIn = false);
		[[nodiscard]] std::optional<Source> GetSource(std::string_view a_modId) const;

		void NotifyAll() const;
		void NotifyMod(std::string_view a_modId) const;

		[[nodiscard]] const nlohmann::json* GetValue(std::string_view a_modId, std::string_view a_key) const;
		[[nodiscard]] std::string GetSettingType(std::string_view a_modId, std::string_view a_key) const;

		[[nodiscard]] std::optional<std::string> CanonicalEnumValue(std::string_view a_modId, std::string_view a_key, std::string_view a_value) const;
		[[nodiscard]] std::vector<KeySetting> KeySettings() const;
		// Returns a validated schema-owned Papyrus target for one key.
		[[nodiscard]] std::optional<PapyrusHotkeyTarget> GetHotkeyTarget(std::string_view a_modId, std::string_view a_key) const;
		[[nodiscard]] std::vector<HotkeyTargetIssue> HotkeyTargetIssues() const;

		bool ReloadDropInFile(const std::filesystem::path& a_path);

		void SetKeyNameResolver(KeyNameResolver a_resolver)
		{
			_keyResolver = std::move(a_resolver);
			InvalidateData();
		}
		void SetLegacyKeyMigrator(LegacyKeyMigrator a_migrator)
		{
			_legacyKeyMigrator = std::move(a_migrator);
		}
		void SetTextResolver(TextResolver a_resolver)
		{
			_textResolver = std::move(a_resolver);
			InvalidateData();
		}
		void InvalidateLocalizedData() { InvalidateData(); }

		struct GameBinding
		{
			std::string   event;    // conflict entry `key` ("QuickSave")
			std::string   title;    // conflict entry `title` ("Starfield (Quicksave)")
			std::uint32_t code;     // physical scan code (DIK convention)
			// Canonical display key name, such as "F5".
			std::string   name;
			std::string   engineInputContextName;  // Starfield ControlMap context name ("Workshop")
			std::string   slot;
			ControlMapPolicy::Classification classification{ ControlMapPolicy::Classification::Core };
			GameplayModeMask definiteModes{ kAllGameplayModes };
			GameplayModeMask possibleModes{ 0 };

			bool operator==(const GameBinding&) const = default;
		};
		[[nodiscard]] bool SetGameBindings(std::vector<GameBinding> a_bindings)
		{
			if (_gameBindings == a_bindings) return false;
			_gameBindings = std::move(a_bindings);
			InvalidateData();
			return true;
		}
		[[nodiscard]] bool SetGameBindingWarningsEnabled(bool a_enabled)
		{
			if (_gameBindingWarningsEnabled != a_enabled) {
				_gameBindingWarningsEnabled = a_enabled;
				InvalidateData();
				return true;
			}
			return false;
		}

		struct HotkeyScope
		{
			bool scoped{ false };
			GameplayModeMask modes{ kAllGameplayModes };
		};
		[[nodiscard]] HotkeyScope ScopeForHotkey(std::string_view a_modId, std::string_view a_key) const;

		void SetKeyboardLabels(std::string a_layout, std::vector<std::pair<std::string, std::string>> a_labels)
		{
			_keyboardLayout = std::move(a_layout);
			_keyboardLabels = std::move(a_labels);
			InvalidateData();
		}

		[[nodiscard]] std::uint64_t Generation() const { return _generation; }

		struct LoadError
		{
			std::string kind;
			std::string file;
			std::string mod;
			std::string message;
		};
		[[nodiscard]] const std::vector<LoadError>& LoadErrors() const { return _loadErrors; }

		[[nodiscard]] nlohmann::json Data() const;
		[[nodiscard]] const nlohmann::json& DataView() const;
		[[nodiscard]] std::string    DataJson() const;

		[[nodiscard]] nlohmann::json ConflictsFor(std::uint32_t a_code, std::string_view a_excludeMod, std::string_view a_excludeKey) const;

		bool Set(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);
		struct SetResult
		{
			bool        ok{ false };
			std::string code;
		};
		[[nodiscard]] SetResult SetWithResult(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson);
		[[nodiscard]] SetResult SetValueWithResult(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		[[nodiscard]] nlohmann::json ConflictsForSetting(std::string_view a_modId, std::string_view a_key) const;

		bool Reset(std::string_view a_modId, std::string_view a_key);

		static constexpr double kPersistDelaySeconds = 0.5;
		void PumpPersistence(double a_nowSeconds);

		void FlushPersistence();

	private:
		struct Mod
		{
			std::string           id;
			nlohmann::json        schema;  // read-only
			nlohmann::json        values;  // { key: current value }
			nlohmann::json        preserved;  // { key: opaque saved value }
			// OSF UI release targeted by this schema.
			std::string              targetVersion;
			std::int64_t             formatVersion{ 2 };
			std::filesystem::path valuesPath;
			std::filesystem::path schemaPath;  // drop-in source file; empty for native registrations
			std::vector<std::string> shadowed;
			std::vector<HotkeyTargetIssue> hotkeyTargetIssues;
			Source                source{ Source::kDropIn };
			bool                  dirty{ false };  // has unflushed write-behind changes
			double                dueAt{ 0.0 };    // when the open window flushes (store clock)
		};

		bool AddSchema(nlohmann::json a_schema, Source a_source, std::string a_idHint, bool a_notify, bool a_dropInReplace = false, std::filesystem::path a_sourcePath = {});

		[[nodiscard]] Mod*       FindMod(std::string_view a_modId);
		[[nodiscard]] const Mod* FindMod(std::string_view a_modId) const;
		[[nodiscard]] static const nlohmann::json* FindSetting(const Mod& a_mod, std::string_view a_key);
		[[nodiscard]] static std::optional<nlohmann::json> Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value);
		[[nodiscard]] static nlohmann::json DefaultFor(const nlohmann::json& a_setting);
		struct HotkeyContext
		{
			std::string id{ "gameplay" };
			std::string label{ "Gameplay" };
			bool        blocksGameplay{ false };
			bool        modeScoped{ false };
			GameplayModeMask modes{ kAllGameplayModes };
		};
		[[nodiscard]] HotkeyContext ResolveHotkeyContext(const Mod& a_mod, const nlohmann::json& a_setting) const;
		static void WarnHotkeyContexts(const nlohmann::json& a_schema, std::string_view a_modId);
		struct BoundKey
		{
			std::string   modId;
			std::string   key;
			std::string   title;
			std::uint32_t code;
			bool          blocksGameplay{ false };
			bool          modeScoped{ false };
			GameplayModeMask modes{ kAllGameplayModes };
			bool          gameBinding{ false };
			std::string   engineInputContextName;  // populated only for @game pseudo-entries
			std::string   slot;
			ControlMapPolicy::Classification classification{ ControlMapPolicy::Classification::Unknown };
			GameplayModeMask possibleModes{ 0 };
		};
		[[nodiscard]] std::vector<BoundKey> ResolveBoundKeys() const;

		[[nodiscard]] static nlohmann::json CollectConflicts(const std::vector<BoundKey>& a_bound, std::uint32_t a_code, std::string_view a_excludeMod, std::string_view a_excludeKey, const HotkeyContext& a_selfContext);
		[[nodiscard]] static nlohmann::json SparseValues(const Mod& a_mod);
		void        MarkDirty(Mod& a_mod);
		[[nodiscard]] bool        PersistNow(Mod& a_mod) const;
		static bool Persist(const Mod& a_mod);
		void        Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const;
		void        NotifyRegistryChanged() const;
		void        InvalidateData() { _dataCache.reset(); }
		void        RecordLoadError(std::string a_kind, std::string a_file, std::string a_mod, std::string a_message);
		bool        EraseLoadErrorsForFile(std::string_view a_file);
		bool        EraseLoadErrorsForMod(std::string_view a_modId);
		std::vector<Mod>              _mods;
		KeyNameResolver               _keyResolver;
		LegacyKeyMigrator             _legacyKeyMigrator;
		std::string                   _keyboardLayout;
		std::vector<std::pair<std::string, std::string>> _keyboardLabels;
		TextResolver                  _textResolver;
		std::vector<GameBinding>      _gameBindings;
		bool                          _gameBindingWarningsEnabled{ true };
		std::vector<ChangeListener>   _listeners;
		std::vector<RegistryListener> _registryListeners;
		std::vector<PersistListener>  _persistListeners;
		mutable std::optional<nlohmann::json> _dataCache;
		std::vector<LoadError>      _loadErrors;
		std::filesystem::path       _valuesDir;
		std::uint64_t               _generation{ 0 };
		double                      _now{ 0.0 };  // last PumpPersistence clock; MarkDirty stamps windows with it
		bool                        _loaded{ false };
	};
}
