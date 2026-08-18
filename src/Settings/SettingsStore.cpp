#include "Settings/SettingsStore.h"

#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "Core/Log.h"
#include "Core/Color.h"
#include "Core/StringUtil.h"
#include "Core/Version.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "API/PapyrusNames.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::size_t kMaxStringLen = 256;
		constexpr std::size_t kMaxModIdLen = Ids::kMaxModIdLen;
		constexpr std::size_t kMaxHotkeyContextIdLength = 64;

		// Reserved meta key: the schema `version` the file was last written under
		// `$`-prefixed so it can never collide with a setting
		// key, and is ignored by builds that predate versioning.
		constexpr const char* kSchemaVersionKey = "$schemaVersion";

		// Values-file encoding version, distinct from the mod's schema `version`.
		// Stamped on every rewrite; a newer OSF UI runtime's higher stamp round-trips
		// (Mod::formatVersion keeps the max).
		constexpr const char*  kFormatVersionKey = "$formatVersion";
		// v2: key-typed values re-anchored from VK-based names to physical
		// scan-code names (the LegacyKeyMigrator pass in LoadMod).
		constexpr std::int64_t kValuesFormatVersion = 2;

		bool IsValidHotkeyContextId(std::string_view a_id)
		{
			if (a_id.empty() || a_id.size() > kMaxHotkeyContextIdLength) {
				return false;
			}
			const auto isAlnum = [](const char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				       (c >= '0' && c <= '9');
			};
			if (!isAlnum(a_id.front())) {
				return false;
			}
			return std::all_of(a_id.begin() + 1, a_id.end(), [&](const char c) {
				return isAlnum(c) || c == '.' || c == '_' || c == '-';
			});
		}

		std::optional<GameplayModeMask> ParseGameplayModes(const nlohmann::json& a_context)
		{
			// Absent (legacy/unscoped), not an array, and empty all read the same:
			// no declared scope.
			const auto* declared = Json::GetArray(a_context, "gameplayModes");
			if (!declared || declared->empty()) return std::nullopt;
			GameplayModeMask modes = 0;
			for (const auto& value : *declared) {
				if (!value.is_string()) return std::nullopt;
				const auto mode = GameplayModeFromName(value.get_ref<const std::string&>());
				if (!mode) return std::nullopt;
				modes |= ModeBit(*mode);
			}
			return modes == 0 ? std::nullopt : std::optional{ modes };
		}

		struct ParsedHotkeyTarget
		{
			bool present{ false };
			std::optional<SettingsStore::PapyrusHotkeyTarget> target;
			std::string error;
		};

		ParsedHotkeyTarget ParseHotkeyTarget(const nlohmann::json& a_setting)
		{
			const auto it = a_setting.find("onPress");
			if (it == a_setting.end()) {
				return {};
			}
			ParsedHotkeyTarget out{ .present = true };
			if (Json::Get(a_setting, "type", "") != "key") {
				out.error = "onPress is allowed only on type:\"key\" settings";
				return out;
			}
			if (!it->is_object()) {
				out.error = "onPress must be an object with string script and function fields";
				return out;
			}
			for (const auto& [name, value] : it->items()) {
				(void)value;
				if (name != "script" && name != "function") {
					out.error = "onPress accepts only script and function fields";
					return out;
				}
			}
			const auto script = Json::Get(*it, "script", "");
			const auto function = Json::Get(*it, "function", "");
			if (!PapyrusNames::IsScriptName(script)) {
				out.error = "onPress.script must be a Papyrus script name of at most 128 characters";
				return out;
			}
			if (!PapyrusNames::IsIdentifier(function)) {
				out.error = "onPress.function must be a Papyrus function name of at most 128 characters";
				return out;
			}
			out.target = SettingsStore::PapyrusHotkeyTarget{ script, function };
			return out;
		}

		// The frozen base type set. A setting whose declared type is outside it is
		// a forward-compat case, not an error: serve the schema default read-only
		// and preserve the saved value opaquely, never wipe it.
		bool IsKnownType(std::string_view a_type)
		{
			for (const std::string_view t : { "bool", "int", "float", "enum", "string", "key", "flags" }) {
				if (a_type == t) {
					return true;
				}
			}
			return false;
		}

		// Iterate every setting object across all groups. `a_fn(setting)` may
		// return true to stop early. Templated on the json's constness so Data()
		// can annotate its schema copy through the same walk.
		template <class Json, class Fn>
		void ForEachSetting(Json& a_schema, Fn&& a_fn)
		{
			const auto groups = a_schema.find("groups");
			if (groups == a_schema.end() || !groups->is_array()) {
				return;
			}
			for (auto& group : *groups) {
				const auto settings = group.find("settings");
				if (settings == group.end() || !settings->is_array()) {
					continue;
				}
				for (auto& setting : *settings) {
					if (setting.is_object() && a_fn(setting)) {
						return;
					}
				}
			}
		}

		// Every OBJECT element of a_object's a_key array, with its ARRAY index.
		// A missing key, a non-array, and non-object elements are all skipped.
		//
		// The index passed through is deliberately the position in the array,
		// not a count of the objects seen: StableId() uses it as the
		// localization address for an entry that declares no id, so a filtered
		// counter would silently re-address every entry after a malformed one.
		// Templated on the json's constness like ForEachSetting.
		template <class Json, class Fn>
		void ForEachObjectIn(Json& a_object, std::string_view a_key, Fn&& a_fn)
		{
			const auto it = a_object.find(a_key);
			if (it == a_object.end() || !it->is_array()) {
				return;
			}
			for (std::size_t i = 0; i < it->size(); ++i) {
				if (auto& element = (*it)[i]; element.is_object()) {
					a_fn(element, i);
				}
			}
		}

		std::string StableId(const nlohmann::json& a_object, std::string_view a_field, std::size_t a_index)
		{
			auto id = Json::Get(a_object, a_field, "");
			return id.empty() ? std::to_string(a_index) : id;
		}

		void ResolveField(nlohmann::json& a_object, std::string_view a_field,
			std::string a_address, std::string_view a_modId, const SettingsStore::TextResolver& a_resolver)
		{
			if (!a_resolver) {
				return;
			}
			const auto it = a_object.find(a_field);
			if (it != a_object.end() && it->is_string()) {
				*it = a_resolver(a_modId, a_address, it->get_ref<const std::string&>());
			}
		}

		void LocalizeSchema(nlohmann::json& a_schema, std::string_view a_modId,
			const SettingsStore::TextResolver& a_resolver)
		{
			if (!a_resolver) {
				return;
			}
			ResolveField(a_schema, "title", "settings.title", a_modId, a_resolver);
			ResolveField(a_schema, "description", "settings.description", a_modId, a_resolver);

			ForEachObjectIn(a_schema, "inputContexts", [&](auto& a_context, std::size_t a_index) {
				const auto id = StableId(a_context, "id", a_index);
				ResolveField(a_context, "label", "inputContexts." + id + ".label", a_modId, a_resolver);
			});

			ForEachObjectIn(a_schema, "pages", [&](auto& a_page, std::size_t a_index) {
				const auto id = StableId(a_page, "id", a_index);
				ResolveField(a_page, "label", "pages." + id + ".label", a_modId, a_resolver);
			});

			ForEachObjectIn(a_schema, "presets", [&](auto& a_preset, std::size_t a_index) {
				const auto root = "presets." + StableId(a_preset, "id", a_index);
				ResolveField(a_preset, "label", root + ".label", a_modId, a_resolver);
				ResolveField(a_preset, "description", root + ".description", a_modId, a_resolver);
			});

			ForEachObjectIn(a_schema, "groups", [&](auto& a_group, std::size_t a_groupIndex) {
				const auto groupId = StableId(a_group, "id", a_groupIndex);
				ResolveField(a_group, "label", "groups." + groupId + ".label", a_modId, a_resolver);
				ForEachObjectIn(a_group, "settings", [&](auto& item, std::size_t itemIndex) {
					const auto type = Json::Get(item, "type", "");
					const auto key = Json::Get(item, "key", "");
					if (type == "action") {
						const auto root = "actions." + (key.empty() ? std::to_string(itemIndex) : key);
						for (const auto* field : { "label", "hint", "confirm" }) ResolveField(item, field, root + "." + field, a_modId, a_resolver);
					} else if (type == "note") {
						const auto id = StableId(item, "id", itemIndex);
						ResolveField(item, "text", "notes." + id + ".text", a_modId, a_resolver);
					} else if (type == "image") {
						const auto id = StableId(item, "id", itemIndex);
						ResolveField(item, "caption", "images." + id + ".caption", a_modId, a_resolver);
					} else if (!key.empty()) {
						const auto root = "settings." + key;
						ResolveField(item, "label", root + ".label", a_modId, a_resolver);
						ResolveField(item, "hint", root + ".hint", a_modId, a_resolver);
						if (auto format = item.find("format"); format != item.end() && format->is_object()) {
							ResolveField(*format, "prefix", root + ".format.prefix", a_modId, a_resolver);
							ResolveField(*format, "suffix", root + ".format.suffix", a_modId, a_resolver);
						}
						const auto options = item.find("options");
						auto labels = item.find("optionLabels");
						if (options != item.end() && options->is_array() && labels != item.end() && labels->is_array()) {
							const auto count = (std::min)(options->size(), labels->size());
							for (std::size_t i = 0; i < count; ++i) {
								if ((*options)[i].is_string() && (*labels)[i].is_string()) {
									const auto address = root + ".options." + (*options)[i].get<std::string>();
									(*labels)[i] = a_resolver(a_modId, address, (*labels)[i].get_ref<const std::string&>());
								}
							}
						}
					}
				});
			});
		}
	}

	SettingsStore::~SettingsStore()
	{
		FlushPersistence();
	}

	void SettingsStore::LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir)
	{
		InvalidateData();
		_mods.clear();
		if (!_loadErrors.empty()) {
			++_loadErrorGeneration;
		}
		_loadErrors.clear();
		_valuesDir = a_valuesDir;
		++_generation;

		std::error_code ec;
		if (!std::filesystem::is_directory(a_schemaDir, ec)) {
			REX::WARN("SettingsStore: no schema directory at {} — settings UI will be empty", a_schemaDir.string());
			return;
		}

		// Sort by filename: directory_iterator order is unspecified, and
		// duplicate-id resolution (first wins) must be deterministic.
		std::vector<std::filesystem::path> files;
		std::filesystem::directory_iterator it(
			a_schemaDir, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::directory_iterator end;
		for (; it != end; it.increment(ec)) {
			const auto entry = *it;
			std::error_code entryEc;
			if (entry.is_regular_file(entryEc) && entry.path().extension() == ".json") {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());

		for (const auto& path : files) {
			// Drop-in id == filename stem, so the stem must be a safe opaque mod id.
			// Rejected here so the log names the file.
			if (const auto stem = path.stem().string(); !Ids::IsAcceptedModId(stem)) {
				REX::ERROR("SettingsStore: [content] skipping {} — filename stem is not a safe mod id",
					path.string());
				RecordLoadError("schema-name", path.filename().string(), "",
					"filename stem is not a safe mod id");
				continue;
			}
			std::string parseError;
			auto schema = Json::ParseFile(path, &parseError);
			if (!schema || !schema->is_object()) {
				const auto why = schema ? std::string("not a JSON object") : parseError;
				REX::ERROR("SettingsStore: [content] skipping {} — {}", path.string(), why);
				RecordLoadError("schema-parse", path.filename().string(), "", why);
				continue;
			}
			// Startup load: notifications defer to the OnStart NotifyAll.
			AddDropInSchema(std::move(*schema), path.stem().string(),
				/*a_notify=*/false, /*a_replaceExisting=*/false, path);
		}

		REX::INFO("SettingsStore: {} mod schema(s) registered from {}", _mods.size(), a_schemaDir.string());
	}

	bool SettingsStore::ReloadDropInFile(const std::filesystem::path& a_path)
	{
		std::string parseError;
		auto schema = Json::ParseFile(a_path, &parseError);
		if (!schema || !schema->is_object()) {
			const auto why = schema ? std::string("not a JSON object") : parseError;
			REX::WARN("SettingsStore: [content] hot-reload skipped — {}: {}", a_path.string(), why);
			RecordLoadError("schema-parse", a_path.filename().string(), "", why);
			NotifyRegistryChanged();
			return false;
		}
		EraseLoadErrorsForFile(a_path.filename().string());
		return AddDropInSchema(std::move(*schema), a_path.stem().string(),
			/*a_notify=*/true, /*a_replaceExisting=*/true, a_path);
	}

	bool SettingsStore::AddDropInSchema(nlohmann::json a_schema, std::string a_idHint,
		bool a_notify, bool a_replaceExisting, std::filesystem::path a_sourcePath)
	{
		if (!a_schema.is_object()) {
			REX::WARN("SettingsStore: [content] rejected schema — not a JSON object");
			return false;
		}
		auto id = Json::Get(a_schema, "id", a_idHint);
		// The id must equal the filename stem (documented drop-in contract);
		// warn and override, so a file can't hijack another mod's id and MO2's per-file VFS priority stays the arbiter of who owns settings/<id>.json.
		if (id != a_idHint) {
			REX::WARN("SettingsStore: [content] schema id '{}' must equal the filename stem — using '{}'",
				id.substr(0, kMaxModIdLen), a_idHint);
			id = a_idHint;
		}
		a_schema["id"] = id;  // the document the web layer sees carries the effective id
		if (!Ids::IsAcceptedModId(id)) {
			REX::ERROR("SettingsStore: [content] rejected schema id '{}' — invalid filename stem (max {} bytes)",
				id.substr(0, kMaxModIdLen), kMaxModIdLen);
			return false;
		}
		// Unknown top-level keys are the normal compatible case (a newer schema on
		// an older OSF UI runtime), so developer mode only.
		if (Log::DebugEnabled()) {
			Json::ReportUnknownKeys(a_schema,
				{ "id", "title", "description", "version", "targetVersion", "accent",
					"icon", "presets", "inputContexts", "pages", "groups" },
				"SettingsStore: schema '" + id + "'", /*a_warn=*/false);
		}

		Mod* existing = FindMod(id);
		if (existing) {
			if (!a_replaceExisting) {
				// First-wins: log both files and record the loser so Data() can report the conflict.
				const auto kept = existing->schemaPath.empty() ? std::string("the first-loaded schema") : existing->schemaPath.string();
				REX::ERROR("SettingsStore: [content] duplicate schema id '{}' - keeping {}, ignoring {}", id, kept, a_sourcePath.string());
				const auto loser = a_sourcePath.filename().string();
				if (std::find(existing->shadowed.begin(), existing->shadowed.end(), loser) == existing->shadowed.end()) {
					existing->shadowed.push_back(loser);
					InvalidateData();
				}
				return false;
			}

			if (existing->dirty && !PersistNow(*existing)) {
				REX::ERROR("SettingsStore: cannot replace schema '{}'; pending values could not be saved", existing->id);
				return false;
			}
			REX::DEBUG("SettingsStore: hot-reloading drop-in schema '{}'", id);
		}

		WarnHotkeyContexts(a_schema, id);

		Mod mod;
		mod.id = std::move(id);
		mod.schema = std::move(a_schema);
		mod.valuesPath = _valuesDir / (mod.id + ".json");
		mod.schemaPath = std::move(a_sourcePath);
		mod.values = nlohmann::json::object();
		mod.preserved = nlohmann::json::object();
		const auto schemaSource = mod.schemaPath.filename().string();
		ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
			const auto parsed = ParseHotkeyTarget(a_setting);
			if (parsed.present && !parsed.target) {
				auto key = Json::Get(a_setting, "key", "");
				if (key.empty()) {
					key = "<unnamed>";
				}
				REX::ERROR("SettingsStore: [content] '{}.{}' {} — declarative hotkey dispatch disabled",
					mod.id, key, parsed.error);
				mod.hotkeyTargetIssues.push_back({ mod.id, std::move(key), schemaSource, parsed.error });
			}
			return false;
		});
		if (existing) {
			mod.shadowed = std::move(existing->shadowed);  // conflicts outlive a replacement/hot-reload
		}

		// Advisory OSF UI release target, same field and semantics as a view
		// manifest's. Never gates loading: a schema authored for a newer OSF UI
		// loads best-effort (unknown types serve read-only defaults, unknown keys
		// are preserved). Carried in `osfui/settings` state for the "needs update" badge.
		if (auto target = Json::Get(mod.schema, "targetVersion", ""); !target.empty()) {
			if (const auto targetParts = ParseDottedVersion(target)) {
				mod.targetVersion = std::move(target);
				if (kOsfuiReleaseVersionParts < *targetParts) {
					REX::WARN("SettingsStore: [content] '{}' targets OSF UI {} but this is {} — update OSF UI",
						mod.id, mod.targetVersion, kOsfuiReleaseVersion);
				}
			} else {
				REX::WARN("SettingsStore: [content] '{}' targetVersion '{}' is not '<major>[.<minor>[.<patch>]]' — ignored",
					mod.id, target);
			}
		}

		// Persisted values over schema defaults; a replacement rebuilds from the
		// same file, so added/removed/retyped keys resolve like a fresh load.
		// A corrupt file is never silently discarded: under
		// sparse persistence that is indistinguishable from "user reset
		// everything") — quarantine to <id>.json.bad, serve defaults, and report
		// the reason in Data()'s loadErrors.
		EraseLoadErrorsForMod(mod.id);
		nlohmann::json saved = nlohmann::json::object();
		bool           valuesFileLoaded = false;
		std::error_code fsEc;
		if (std::filesystem::exists(mod.valuesPath, fsEc)) {
			std::string parseError;
			auto parsed = Json::ParseFile(mod.valuesPath, &parseError);
			if (parsed && parsed->is_object()) {
				saved = std::move(*parsed);
				valuesFileLoaded = true;
			} else {
				const auto why = parsed ? std::string("not a JSON object") : parseError;
				auto quarantine = mod.valuesPath;
				quarantine += ".bad";
				std::filesystem::remove(quarantine, fsEc);  // keep the newest bad file
				std::filesystem::rename(mod.valuesPath, quarantine, fsEc);
				REX::ERROR("SettingsStore: '{}' values file is corrupt ({}) — {}; defaults served",
					mod.id, why,
					fsEc ? "quarantine rename failed, file left in place" :
					       "kept as " + quarantine.filename().string());
				RecordLoadError("values-parse", mod.valuesPath.filename().string(), mod.id, why);
			}
		}

		// Version bookkeeping: the schema's `version`
		// (default 0) is stamped as `$schemaVersion`. `$`-prefixed keys are
		// invisible to the schema walk below (no setting may be named `$…`), so
		// older builds ignore it. The log line is triage only; alias adoption
		// below is what carries values across a rename.
		const auto schemaVersion = static_cast<std::int64_t>(Json::Get(mod.schema, "version", 0));
		// Absent or non-integer defaults to the current version, so only a real
		// recorded mismatch logs.
		if (const auto fileVersion = Json::Get(saved, kSchemaVersionKey, schemaVersion); fileVersion != schemaVersion) {
			REX::INFO("SettingsStore: '{}' values migrating v{} -> v{}", mod.id, fileVersion, schemaVersion);
		}
		// Encoding-format stamp, written on every rewrite. A file from a newer
		// OSF UI keeps its higher stamp, so round-tripping through this runtime never
		// downgrades it.
		//
		// Format v1 -> v2 is the physical re-anchor of key names: pre-2.x names
		// were VK-anchored, so on a non-US layout the same spelling now means a
		// different physical key. A missing stamp reads as v1 — stamping and
		// versioning are coeval, so any unstamped file predates v2. With the
		// migrator wired, every key-typed saved value is re-anchored under the
		// CURRENT layout and the file is rewritten eagerly below; without it
		// (native tests, misconfigured composition) the file loads untouched and
		// keeps its v1 stamp so a later session still migrates it.
		// A missing (or quarantined) values file has nothing to migrate:
		// fileFormat only means something for a file that actually loaded.
		const auto fileFormat = valuesFileLoaded ? Json::Get(saved, kFormatVersionKey, 1) :
		                                           kValuesFormatVersion;
		bool migratedFormat = false;
		if (fileFormat > kValuesFormatVersion) {
			REX::INFO("SettingsStore: '{}' values file declares format v{} (this build writes v{}) — written by a newer OSF UI; unknown content rides in the preserved bag",
				mod.id, fileFormat, kValuesFormatVersion);
			mod.formatVersion = fileFormat;
		} else if (fileFormat < kValuesFormatVersion) {
			if (_legacyKeyMigrator) {
				ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
					if (Json::Get(a_setting, "type", "") != "key") {
						return false;
					}
					const auto migrate = [&](const std::string& a_savedKey) {
						if (a_savedKey.empty()) {
							return;
						}
						const auto it = saved.find(a_savedKey);
						if (it == saved.end() || !it->is_string()) {
							return;
						}
						const auto& oldName = it->get_ref<const std::string&>();
						if (oldName.empty()) {
							return;  // deliberate unbound (allowUnbound)
						}
						if (auto renamed = _legacyKeyMigrator(oldName); renamed != oldName) {
							REX::INFO("SettingsStore: '{}.{}' = '{}' re-anchored to '{}' (values format v{} -> v{})",
								mod.id, a_savedKey, oldName, renamed, fileFormat, kValuesFormatVersion);
							*it = std::move(renamed);
						}
					};
					// The value may sit under the key or a declared alias (the
					// adoption below reads both); re-anchor wherever it lives.
					migrate(Json::Get(a_setting, "key", ""));
					for (const auto& alias : Json::GetStringArray(a_setting, "aliases")) {
						migrate(alias);
					}
					return false;
				});
				migratedFormat = true;  // stamp v2 + eager rewrite, even value-unchanged
			} else {
				mod.formatVersion = fileFormat;  // defer: keep the old stamp
			}
		}

		// Keys this schema accounts for: declared keys and, for known-typed
		// settings, their aliases. Whatever the file holds outside this set is a
		// forward-compat unknown and goes to the opaque preserved bag.
		std::unordered_set<std::string> accounted;

		std::size_t count = 0;
		ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::Get(a_setting, "key", "");
			if (key.empty()) {
				return false;
			}
			++count;
			accounted.insert(key);

			// Unknown-typed setting: serve the schema default read-only; the
			// saved value is preserved verbatim below, never served or wiped.
			// Its aliases stay un-accounted on purpose — this runtime can't adopt
			// them, and dropping them would strand a rename for the runtime that can.
			if (!IsKnownType(Json::Get(a_setting, "type", ""))) {
				if (const auto it = saved.find(key); it != saved.end()) {
					mod.preserved[key] = *it;
				}
				mod.values[key] = DefaultFor(a_setting);
				return false;
			}

			for (const auto& alias : Json::GetStringArray(a_setting, "aliases")) {
				accounted.insert(alias);
			}
			if (const auto it = saved.find(key); it != saved.end()) {
				if (auto valid = Validate(a_setting, *it)) {
					mod.values[key] = std::move(*valid);
					return false;
				}
			}
			// Renamed key: the current key is absent or no
			// longer validates, so adopt the first declared `alias` in the file
			// that does. The old key is not schema-declared, so SparseValues drops
			// it and the next write lands under the new name.
			for (const auto& alias : Json::GetStringArray(a_setting, "aliases")) {
				const auto it = saved.find(alias);
				if (it == saved.end()) {
					continue;
				}
				if (auto valid = Validate(a_setting, *it)) {
					REX::INFO("SettingsStore: '{}.{}' adopted from alias '{}'", mod.id, key, alias);
					mod.values[key] = std::move(*valid);
					return false;
				}
			}
			mod.values[key] = DefaultFor(a_setting);
			return false;
		});

		// Saved keys nothing in the schema accounts for — a setting that left the
		// schema, or one from a newer schema — round-trip opaquely instead of
		// being pruned. $schemaVersion and $formatVersion stay owned by the logic
		// above; any other $-key (future format meta) is preserved like an
		// unknown setting.
		for (const auto& [key, value] : saved.items()) {
			if (key == kSchemaVersionKey || key == kFormatVersionKey || accounted.contains(key)) {
				continue;
			}
			mod.preserved[key] = value;
		}
		if (!mod.preserved.empty() && Log::DebugEnabled()) {
			REX::DEBUG("SettingsStore: '{}' preserving {} entr{} this OSF UI runtime can't understand (kept verbatim, not served)",
				mod.id, mod.preserved.size(), mod.preserved.size() == 1 ? "y" : "ies");
		}

		// Prune-to-default on load: a file differing
		// from its sparse form — full legacy file, saved value now equal to an
		// updated default, adopted alias, clamping, stale `$schemaVersion` —
		// schedules a rewrite so those knobs track upstream defaults again.
		// Preserved unknowns are part of the sparse form, and a missing
		// `$formatVersion` alone is clean: stamping never triggers a rewrite.
		// EXCEPT across a format-version boundary: a completed v1 -> v2
		// migration must land on disk eagerly, or an unflushed migration would
		// re-run under a DIFFERENT layout next session and move keys twice.
		if (migratedFormat) {
			MarkDirty(mod);
		} else if (const auto expected = SparseValues(mod); saved != expected) {
			auto stampedOnly = saved;
			stampedOnly[kFormatVersionKey] = mod.formatVersion;
			if (stampedOnly != expected) {
				MarkDirty(mod);
			}
		}

		REX::INFO("SettingsStore: loaded mod '{}' ('{}', {} settings)",
			mod.id, Json::Get(mod.schema, "title", mod.id), count);

		if (existing) {
			*existing = std::move(mod);
		} else {
			_mods.push_back(std::move(mod));
		}
		InvalidateData();
		++_generation;

		if (a_notify) {
			// Replay so consumers that subscribed before this mod registered
			// sync without a separate read, then announce
			// the shape change so the web layer re-broadcasts the registry.
			NotifyMod(existing ? existing->id : _mods.back().id);
			NotifyRegistryChanged();
		}
		return true;
	}

	bool SettingsStore::RemoveMod(std::string_view a_modId)
	{
		const auto it = std::find_if(_mods.begin(), _mods.end(),
			[&](const Mod& a_mod) { return Ids::EqualsCaseInsensitiveAscii(a_mod.id, a_modId); });
		if (it == _mods.end()) {
			return false;
		}
		if (it->dirty && !PersistNow(*it)) {
			REX::ERROR("SettingsStore: cannot remove mod '{}'; pending values could not be saved", it->id);
			return false;
		}
		REX::INFO("SettingsStore: removed mod '{}' (values file kept)", it->id);
		EraseLoadErrorsForMod(it->id);
		_mods.erase(it);
		InvalidateData();
		++_generation;
		NotifyRegistryChanged();
		return true;
	}

	void SettingsStore::RecordLoadError(std::string a_kind, std::string a_file, std::string a_mod, std::string a_message)
	{
		for (auto& e : _loadErrors) {
			if (e.kind == a_kind && e.file == a_file && e.mod == a_mod) {
				if (e.message == a_message) {
					return;
				}
				e.message = std::move(a_message);
				InvalidateData();
				++_loadErrorGeneration;
				return;
			}
		}
		_loadErrors.push_back({ std::move(a_kind), std::move(a_file), std::move(a_mod), std::move(a_message) });
		InvalidateData();
		++_loadErrorGeneration;
	}

	bool SettingsStore::EraseLoadErrorsForFile(std::string_view a_file)
	{
		const auto count = std::erase_if(_loadErrors,
			[&](const LoadError& a_e) { return a_e.mod.empty() && a_e.file == a_file; });
		if (count > 0) {
			InvalidateData();
			++_loadErrorGeneration;
		}
		return count > 0;
	}

	bool SettingsStore::EraseLoadErrorsForMod(std::string_view a_modId)
	{
		const auto count = std::erase_if(_loadErrors,
			[&](const LoadError& a_e) {
				return !a_e.mod.empty() && Ids::EqualsCaseInsensitiveAscii(a_e.mod, a_modId);
			});
		if (count > 0) {
			InvalidateData();
			++_loadErrorGeneration;
		}
		return count > 0;
	}

	void SettingsStore::NotifyAll() const
	{
		for (const auto& mod : _mods) {
			for (const auto& [key, value] : mod.values.items()) {
				Notify(mod.id, key, value);
			}
		}
	}

	void SettingsStore::NotifyMod(std::string_view a_modId) const
	{
		const auto* mod = FindMod(a_modId);
		if (!mod) {
			return;
		}
		for (const auto& [key, value] : mod->values.items()) {
			Notify(mod->id, key, value);
		}
	}

	const nlohmann::json* SettingsStore::GetValue(std::string_view a_modId, std::string_view a_key) const
	{
		const auto* mod = FindMod(a_modId);
		if (!mod) {
			return nullptr;
		}
		const auto it = mod->values.find(a_key);
		return it != mod->values.end() ? &*it : nullptr;
	}

	std::string SettingsStore::GetSettingType(std::string_view a_modId, std::string_view a_key) const
	{
		const auto* mod = FindMod(a_modId);
		if (!mod) {
			return {};
		}
		const auto* setting = FindSetting(*mod, a_key);
		return setting ? Json::Get(*setting, "type", "") : std::string{};
	}

	std::optional<std::string> SettingsStore::CanonicalEnumValue(std::string_view a_modId, std::string_view a_key, std::string_view a_value) const
	{
		const auto* mod = FindMod(a_modId);
		const auto* setting = mod ? FindSetting(*mod, a_key) : nullptr;
		if (!setting || Json::Get(*setting, "type", "") != "enum") {
			return std::nullopt;
		}
		const auto* options = Json::GetArray(*setting, "options");
		if (!options) {
			return std::nullopt;
		}
		for (const auto& opt : *options) {
			if (opt.is_string() && Ids::EqualsCaseInsensitiveAscii(opt.get_ref<const std::string&>(), a_value)) {
				return opt.get<std::string>();
			}
		}
		return std::nullopt;
	}

	std::vector<SettingsStore::KeySetting> SettingsStore::KeySettings() const
	{
		std::vector<KeySetting> out;
		for (const auto& mod : _mods) {
			ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
				if (Json::Get(a_setting, "type", "") == "key") {
					const auto key = Json::Get(a_setting, "key", "");
					if (!key.empty()) {
						if (const auto it = mod.values.find(key); it != mod.values.end() && it->is_string()) {
							out.push_back({ mod.id, key, it->get<std::string>() });
						}
					}
				}
				return false;
			});
		}
		return out;
	}

	std::optional<SettingsStore::PapyrusHotkeyTarget> SettingsStore::GetHotkeyTarget(
		std::string_view a_modId, std::string_view a_key) const
	{
		const auto* mod = FindMod(a_modId);
		const auto* setting = mod ? FindSetting(*mod, a_key) : nullptr;
		if (!setting) {
			return std::nullopt;
		}
		return ParseHotkeyTarget(*setting).target;
	}

	std::vector<SettingsStore::HotkeyTargetIssue> SettingsStore::HotkeyTargetIssues() const
	{
		std::vector<HotkeyTargetIssue> out;
		for (const auto& mod : _mods) {
			out.insert(out.end(), mod.hotkeyTargetIssues.begin(), mod.hotkeyTargetIssues.end());
		}
		return out;
	}

	SettingsStore::HotkeyContext SettingsStore::ResolveHotkeyContext(const Mod& a_mod, const nlohmann::json& a_setting) const
	{
		HotkeyContext fallback;
		const auto ref = Json::Get(a_setting, "inputContext", "");
		if (ref.empty() || ref == "gameplay" || !IsValidHotkeyContextId(ref)) {
			return fallback;
		}

		const auto* contexts = Json::GetArray(a_mod.schema, "inputContexts");
		if (!contexts) {
			return fallback;
		}
		std::unordered_set<std::string> seen;
		for (const auto& context : *contexts) {
			if (!context.is_object()) {
				continue;
			}
			const auto id = Json::Get(context, "id", "");
			if (id == "gameplay" || !IsValidHotkeyContextId(id) || !seen.insert(id).second) {
				continue;
			}
			if (id == ref) {
				auto label = Json::Get(context, "label", id);
				if (label.empty()) {
					label = id;
				}
				if (_textResolver) {
					label = _textResolver(a_mod.id, "inputContexts." + id + ".label", label);
				}
				HotkeyContext resolved;
				resolved.id = id;
				resolved.label = std::move(label);
				resolved.blocksGameplay = Json::Get(context, "blocksGameplay", false);
				if (const auto modes = ParseGameplayModes(context)) {
					resolved.modeScoped = true;
					resolved.modes = *modes;
				}
				return resolved;
			}
		}
		return fallback;
	}

	void SettingsStore::WarnHotkeyContexts(const nlohmann::json& a_schema, std::string_view a_modId)
	{
		const auto contexts = a_schema.find("inputContexts");
		if (contexts == a_schema.end()) {
			return;
		}
		if (!contexts->is_array()) {
			REX::WARN("SettingsStore: [content] '{}.inputContexts' must be an array -- hotkey contexts fall back to gameplay", a_modId);
			return;
		}
		std::unordered_set<std::string> seen;
		for (const auto& context : *contexts) {
			const auto id = context.is_object() ? Json::Get(context, "id", "") : std::string{};
			if (id == "gameplay") {
				REX::WARN("SettingsStore: [content] '{}.inputContexts' cannot redefine reserved context 'gameplay' -- ignoring it", a_modId);
				continue;
			}
			if (!IsValidHotkeyContextId(id)) {
				REX::WARN("SettingsStore: [content] '{}' has an invalid hotkey-context id -- ignoring it", a_modId);
				continue;
			}
			if (!seen.insert(id).second) {
				REX::WARN("SettingsStore: [content] '{}' defines hotkey context '{}' more than once -- keeping the first", a_modId, id);
				continue;
			}
			if (context.contains("gameplayModes") && !ParseGameplayModes(context)) {
				REX::WARN("SettingsStore: [content] '{}.inputContexts.{}.gameplayModes' must be a non-empty array containing only onFoot, ship, vehicle, or zeroG -- context keeps legacy unscoped dispatch",
					a_modId, id);
			}
		}
	}

	SettingsStore::HotkeyScope SettingsStore::ScopeForHotkey(std::string_view a_modId, std::string_view a_key) const
	{
		const auto* mod = FindMod(a_modId);
		const auto* setting = mod ? FindSetting(*mod, a_key) : nullptr;
		if (!mod || !setting || Json::Get(*setting, "type", "") != "key") return {};
		const auto context = ResolveHotkeyContext(*mod, *setting);
		return { context.modeScoped, context.modes };
	}

	std::vector<SettingsStore::BoundKey> SettingsStore::ResolveBoundKeys() const
	{
		std::vector<BoundKey> bound;
		if (_keyResolver) {
			for (const auto& setting : KeySettings()) {
				if (const auto code = _keyResolver(setting.name); code != 0) {
					const auto* mod = FindMod(setting.modId);
					HotkeyContext context;
					if (mod) {
						if (const auto* authored = FindSetting(*mod, setting.key)) {
							context = ResolveHotkeyContext(*mod, *authored);
						}
					}
					auto title = mod ? Json::Get(mod->schema, "title", mod->id) : setting.modId;
					if (mod && _textResolver) title = _textResolver(mod->id, "settings.title", title);
					BoundKey entry;
					entry.modId = setting.modId;
					entry.key = setting.key;
					entry.title = std::move(title);
					entry.code = code;
					entry.blocksGameplay = context.blocksGameplay;
					entry.modeScoped = context.modeScoped;
					entry.modes = context.modes;
					bound.push_back(std::move(entry));
				}
			}
			// The game's own bindings: pseudo-entries under
			// the reserved id "@game". Gated on the same resolver as mod
			// settings — without one there is no conflict grouping at all.
			for (const auto& gameBinding : _gameBindings) {
				if (_gameBindingWarningsEnabled && gameBinding.code != 0) {
					BoundKey entry;
					entry.modId = "@game";
					entry.key = gameBinding.event;
					entry.title = gameBinding.title;
					entry.code = gameBinding.code;
					entry.gameBinding = true;
					entry.engineInputContextName = gameBinding.engineInputContextName;
					entry.slot = gameBinding.slot;
					entry.classification = gameBinding.classification;
					entry.modes = gameBinding.definiteModes;
					entry.possibleModes = gameBinding.possibleModes;
					bound.push_back(std::move(entry));
				}
			}
		}
		return bound;
	}

	nlohmann::json SettingsStore::CollectConflicts(const std::vector<BoundKey>& a_bound, std::uint32_t a_code,
		std::string_view a_excludeMod, std::string_view a_excludeKey, const HotkeyContext& a_selfContext)
	{
		nlohmann::json conflicts = nlohmann::json::array();
		for (const auto& other : a_bound) {
			if (other.code != a_code ||
				(Ids::EqualsCaseInsensitiveAscii(other.modId, a_excludeMod) && other.key == a_excludeKey)) continue;
			if (!other.gameBinding) {
				if (!ModesOverlap(a_selfContext.modes, other.modes)) continue;
				conflicts.push_back({
					{ "mod", other.modId },
					{ "key", other.key },
					{ "title", other.title },
				});
				continue;
			}
			if (a_selfContext.blocksGameplay) continue;
			const auto gameBindingModes = static_cast<GameplayModeMask>(other.modes | other.possibleModes);
			if (!ModesOverlap(a_selfContext.modes, gameBindingModes)) continue;
			if (other.classification != ControlMapPolicy::Classification::Core &&
				other.classification != ControlMapPolicy::Classification::Special) continue;
			// `vanillaContext` is a frozen Settings data key. It carries the
			// Starfield engine input-context name for this game binding.
			conflicts.push_back({
				{ "mod", "@game" }, { "key", other.key }, { "title", other.title },
				{ "severity", other.classification == ControlMapPolicy::Classification::Core ? "conflict" : "possible" },
				{ "vanillaContext", other.engineInputContextName }, { "slot", other.slot },
			});
		}
		return conflicts;
	}

	nlohmann::json SettingsStore::ConflictsFor(std::uint32_t a_code, std::string_view a_excludeMod, std::string_view a_excludeKey) const
	{
		if (a_code == 0) {
			return nlohmann::json::array();  // unresolvable: never conflicts (mirrors Data())
		}
		HotkeyContext context;
		if (const auto* mod = FindMod(a_excludeMod)) {
			if (const auto* setting = FindSetting(*mod, a_excludeKey)) {
				context = ResolveHotkeyContext(*mod, *setting);
			}
		}
		return CollectConflicts(ResolveBoundKeys(), a_code, a_excludeMod, a_excludeKey, context);
	}

	nlohmann::json SettingsStore::ConflictsForSetting(std::string_view a_modId, std::string_view a_key) const
	{
		if (!_keyResolver) {
			return nlohmann::json::array();  // no resolver = no conflict grouping (mirrors Data())
		}
		const auto* value = GetValue(a_modId, a_key);
		if (!value || !value->is_string() || value->get_ref<const std::string&>().empty()) {
			return nlohmann::json::array();  // unbound/non-key value: never conflicts
		}
		return ConflictsFor(_keyResolver(value->get_ref<const std::string&>()), a_modId, a_key);
	}

	const nlohmann::json& SettingsStore::DataView() const
	{
		if (_dataCache) {
			return *_dataCache;
		}
		// Informational key-conflict grouping: resolve every
		// key-typed setting's value once, so the annotation walk below is a lookup
		// (a re-resolve would warn once per setting per pass on an unresolvable
		// name). Non-blocking: the renderer badges both sides, the bind stands.
		const std::vector<BoundKey> bound = ResolveBoundKeys();

		nlohmann::json mods = nlohmann::json::array();
		for (const auto& mod : _mods) {
			nlohmann::json schema = mod.schema;
			LocalizeSchema(schema, mod.id, _textResolver);
			if (!bound.empty()) {
				ForEachSetting(schema, [&](nlohmann::json& a_setting) {
					if (Json::Get(a_setting, "type", "") != "key") {
						return false;
					}
					const auto key = Json::Get(a_setting, "key", "");
					const auto self = std::find_if(bound.begin(), bound.end(),
						[&](const BoundKey& a_b) { return a_b.modId == mod.id && a_b.key == key; });
					if (self == bound.end()) {
						return false;  // unresolvable/empty value: never conflicts
					}
					HotkeyContext context;
					context.blocksGameplay = self->blocksGameplay;
					context.modeScoped = self->modeScoped;
					context.modes = self->modes;
					nlohmann::json conflicts = CollectConflicts(bound, self->code, mod.id, key, context);
					if (!conflicts.empty()) {
						a_setting["conflicts"] = std::move(conflicts);
					}
					return false;
				});
			}
			nlohmann::json entry{
				{ "id", mod.id },
				{ "title", Json::Get(schema, "title", mod.id) },
				{ "schema", std::move(schema) },
				{ "values", mod.values },
			};
			// Additive field: drop-in files that also claimed this id and lost
			// first-wins; Mod Settings badges the conflict. Omitted if none.
			if (!mod.shadowed.empty()) {
				entry["shadowed"] = mod.shadowed;
			}
			// Advisory authored-against version (same contract as a view manifest's
			// targetVersion): feeds the "needs update" badge. Omitted if undeclared.
			if (!mod.targetVersion.empty()) {
				entry["targetVersion"] = mod.targetVersion;
			}
			mods.push_back(std::move(entry));
		}
		nlohmann::json data{ { "mods", std::move(mods) } };
		// Localized keycap labels for the current layout (additive, display
		// only): `keyboard: { layout, labels: { name -> label } }`. Views fall
		// back to raw names when absent, so an empty map is simply omitted.
		if (!_keyboardLabels.empty()) {
			nlohmann::json labels = nlohmann::json::object();
			for (const auto& [name, label] : _keyboardLabels) {
				labels[name] = label;
			}
			data["keyboard"] = nlohmann::json{
				{ "layout", _keyboardLayout },
				{ "labels", std::move(labels) },
			};
		}
		if (!_loadErrors.empty()) {
			nlohmann::json errors = nlohmann::json::array();
			for (const auto& e : _loadErrors) {
				nlohmann::json entry{
					{ "kind", e.kind }, { "file", e.file }, { "message", e.message },
				};
				if (!e.mod.empty()) entry["mod"] = e.mod;
				errors.push_back(std::move(entry));
			}
			data["loadErrors"] = std::move(errors);
		}
		_dataCache.emplace(std::move(data));
		return *_dataCache;
	}

	nlohmann::json SettingsStore::Data() const
	{
		return DataView();
	}

	std::string SettingsStore::DataJson() const
	{
		return Json::Dump(DataView());
	}

	SettingsStore::Mod* SettingsStore::FindMod(std::string_view a_modId)
	{
		for (auto& mod : _mods) {
			if (Ids::EqualsCaseInsensitiveAscii(mod.id, a_modId)) {
				return &mod;
			}
		}
		return nullptr;
	}

	const SettingsStore::Mod* SettingsStore::FindMod(std::string_view a_modId) const
	{
		return const_cast<SettingsStore*>(this)->FindMod(a_modId);
	}

	const nlohmann::json* SettingsStore::FindSetting(const Mod& a_mod, std::string_view a_key)
	{
		const nlohmann::json* found = nullptr;
		ForEachSetting(a_mod.schema, [&](const nlohmann::json& a_setting) {
			if (Json::Get(a_setting, "key", "") == a_key) {
				found = &a_setting;
				return true;
			}
			return false;
		});
		return found;
	}

	nlohmann::json SettingsStore::DefaultFor(const nlohmann::json& a_setting)
	{
		const auto def = a_setting.find("default");
		return def != a_setting.end() ? *def : nlohmann::json(nullptr);
	}

	std::optional<nlohmann::json> SettingsStore::Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value)
	{
		const auto type = Json::Get(a_setting, "type", "");

		if (type == "bool") {
			if (a_value.is_boolean()) {
				return a_value;
			}
		} else if (type == "int" || type == "float") {
			if (a_value.is_number()) {
				double v = a_value.get<double>();
				// Applied in order rather than via std::clamp: a schema with
				// min > max is untrusted input, and clamp() on an inverted range
				// is undefined. This pins that case to max, as it always has.
				constexpr double kUnbounded = std::numeric_limits<double>::infinity();
				v = (std::max)(v, Json::Get(a_setting, "min", -kUnbounded));
				v = (std::min)(v, Json::Get(a_setting, "max", kUnbounded));
				if (type == "int") {
					return nlohmann::json(static_cast<std::int64_t>(std::llround(v)));
				}
				return nlohmann::json(v);
			}
		} else if (type == "enum") {
			if (a_value.is_string()) {
				if (const auto* options = Json::GetArray(a_setting, "options")) {
					for (const auto& opt : *options) {
						if (opt.is_string() && opt == a_value) {
							return a_value;
						}
					}
				}
			}
		} else if (type == "flags") {
			// Multi-select over `options`: value is an array of option strings.
			// Resolve, don't reject: non-string elements, unknown options
			// (removed upstream) and duplicates are filtered out — the
			// enum-removal analogue of numeric clamping. Output order follows
			// the declared options so the stored form is canonical.
			if (a_value.is_array()) {
				if (const auto* options = Json::GetArray(a_setting, "options")) {
					nlohmann::json out = nlohmann::json::array();
					for (const auto& opt : *options) {
						if (!opt.is_string()) {
							continue;
						}
						for (const auto& v : a_value) {
							if (v.is_string() && v == opt) {
								out.push_back(opt);
								break;
							}
						}
					}
					return out;
				}
			}
		} else if (type == "string") {
			if (a_value.is_string()) {
				auto s = a_value.get<std::string>();
				// A colour-widget string must be a parseable #rrggbb[aa]; every
				// writer (preset, ABI, Papyrus) is held to it, not just the UI.
				if (Json::Get(a_setting, "widget", "") == "color" && !IsHexColor(s)) {
					return std::nullopt;
				}
				// Per-setting maxLength, capped by the store-wide hard limit.
				// Raising kMaxStringLen requires bumping the renderer and mock
				// clamps in lockstep.
				std::size_t cap = kMaxStringLen;
				if (const auto ml = Json::Get(a_setting, "maxLength", std::int64_t{ 0 }); ml > 0) {
					cap = (std::min)(cap, static_cast<std::size_t>(ml));
				}
				// Codepoint-boundary truncation: `cap` counts bytes but the value is
				// arbitrary player text (IME/CJK/emoji), and a split sequence makes
				// every later dump() of this value throw — Persist, the settings.ack
				// and the subscriber push all run where nothing catches.
				StringUtil::TruncateUtf8(s, cap);
				return nlohmann::json(std::move(s));
			}
		} else if (type == "key") {
			// A rebindable key: a short key-name string (e.g. "F10"). The store
			// only bounds the length; whether the name resolves to a VK is the
			// consumer's job (Runtime::OnSettingChanged via ResolveKeyName).
			// Empty is rejected so a blank can't clobber a working binding, unless
			// the schema sets "allowUnbound": true, where "" is the unbound state
			// (skipped by HotkeyService and conflicts).
			if (a_value.is_string()) {
				auto s = a_value.get<std::string>();
				if (s.empty()) {
					if (Json::Get(a_setting, "allowUnbound", false)) {
						return nlohmann::json(std::move(s));
					}
				} else {
					// A resolvable key name is ASCII, but nothing upstream enforces
					// that, so bound it on a codepoint boundary all the same — an
					// unresolvable name is the consumer's business, a split sequence
					// would be a dump() throw on the persist path.
					constexpr std::size_t kMaxKeyNameLen = 16;
					StringUtil::TruncateUtf8(s, kMaxKeyNameLen);
					return nlohmann::json(std::move(s));
				}
			}
		}
		return std::nullopt;
	}

	bool SettingsStore::Set(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson)
	{
		return SetWithResult(a_modId, a_key, a_valueJson).ok;
	}

	SettingsStore::SetResult SettingsStore::SetWithResult(std::string_view a_modId, std::string_view a_key, std::string_view a_valueJson)
	{
		const auto parsed = Json::Parse(a_valueJson);
		if (!parsed) {
			return { false, "invalid-value" };
		}
		return SetValueWithResult(a_modId, a_key, *parsed);
	}

	SettingsStore::SetResult SettingsStore::SetValueWithResult(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		auto* mod = FindMod(a_modId);
		if (!mod) {
			REX::WARN("SettingsStore: [content] rejected set for unknown mod '{}'", a_modId.substr(0, 64));
			return { false, "unknown-setting" };
		}
		const auto* setting = FindSetting(*mod, a_key);
		if (!setting) {
			REX::WARN("SettingsStore: [content] rejected unknown setting '{}.{}'", a_modId.substr(0, 64), a_key.substr(0, 64));
			return { false, "unknown-setting" };
		}
		// A type this OSF UI runtime doesn't know serves its default read-only. Its own
		// result code, so a view can say "needs a newer OSF UI" rather than
		// "bad value".
		if (!IsKnownType(Json::Get(*setting, "type", ""))) {
			REX::WARN("SettingsStore: [content] rejected set for '{}.{}' — unknown type '{}' is served read-only",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::Get(*setting, "type", "?").substr(0, 32));
			return { false, "read-only" };
		}
		auto valid = Validate(*setting, a_value);
		if (!valid) {
			REX::WARN("SettingsStore: [content] rejected invalid value for '{}.{}' (type {})",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::Get(*setting, "type", "?"));
			return { false, "invalid-value" };
		}

		const std::string key{ a_key };
		mod->values[key] = std::move(*valid);
		InvalidateData();
		MarkDirty(*mod);  // notification immediate; disk write coalesced (PumpPersistence)
		Notify(mod->id, key, mod->values[key]);
		if (Log::DebugEnabled()) {
			const auto shown = Json::Dump(mod->values[key]);
			REX::DEBUG("SettingsStore: set '{}.{}' = {}", mod->id, key,
				shown.substr(0, StringUtil::Utf8TruncateLen(shown, 128)));
		}
		return { true, {} };
	}

	bool SettingsStore::Reset(std::string_view a_modId, std::string_view a_key)
	{
		auto* mod = FindMod(a_modId);
		if (!mod) {
			return false;
		}
		if (a_key.empty()) {
			// Whole mod back to defaults.
			ForEachSetting(mod->schema, [&](const nlohmann::json& a_setting) {
				const auto key = Json::Get(a_setting, "key", "");
				if (!key.empty()) {
					mod->values[key] = DefaultFor(a_setting);
				}
				return false;
			});
		} else {
			const auto* setting = FindSetting(*mod, a_key);
			if (!setting) {
				return false;
			}
			mod->values[std::string(a_key)] = DefaultFor(*setting);
		}

		InvalidateData();
		MarkDirty(*mod);  // sparse persistence drops the reset key(s) from the file
		// Notify for every (possibly) changed value so consumers re-sync.
		for (const auto& [key, value] : mod->values.items()) {
			if (a_key.empty() || key == a_key) {
				Notify(mod->id, key, value);
			}
		}
		REX::INFO("SettingsStore: reset '{}{}' to default(s)", mod->id, a_key.empty() ? "" : ("." + std::string(a_key)));
		return true;
	}

	void SettingsStore::Notify(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value) const
	{
		for (const auto& listener : _listeners) {
			if (listener) {
				listener(a_modId, a_key, a_value);
			}
		}
	}

	void SettingsStore::NotifyRegistryChanged() const
	{
		for (const auto& listener : _registryListeners) {
			if (listener) {
				listener();
			}
		}
	}

	nlohmann::json SettingsStore::SparseValues(const Mod& a_mod)
	{
		// Sparse persistence: only a value != the schema
		// default is the user's; anything equal to the default keeps tracking
		// upstream default changes, and reset-to-default means key removal. Full
		// legacy files shed their frozen defaults the first time through here.
		nlohmann::json sparse = nlohmann::json::object();
		ForEachSetting(a_mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::Get(a_setting, "key", "");
			if (!key.empty()) {
				if (const auto it = a_mod.values.find(key); it != a_mod.values.end() && *it != DefaultFor(a_setting)) {
					sparse[key] = *it;
				}
			}
			return false;
		});
		// Forward-compat opaques ride every rewrite verbatim, so a newer mod's
		// settings survive this OSF UI runtime. Unknown-typed declared keys never enter the
		// loop above (their served value is the default), so no collision here.
		for (const auto& [key, value] : a_mod.preserved.items()) {
			sparse[key] = value;
		}
		// Stamp the schema version only when a mod uses
		// versioning: a v0 (unversioned) mod's file stays byte-for-byte as before,
		// so no existing values file is dirtied by this feature. The schema's
		// `version` is the source of truth; the stamp records "last written under
		// vN", and reverting to v0 drops it — a version move like any other.
		if (const auto v = Json::Get(a_mod.schema, "version", 0); v != 0) {
			sparse[kSchemaVersionKey] = v;
		}
		// Every rewrite carries the encoding version. a_mod.formatVersion is
		// max(known, loaded), so a newer OSF UI runtime's stamp survives this runtime's
		// rewrites. The load-time compare tolerates a missing stamp, so this alone
		// never dirties an existing file.
		sparse[kFormatVersionKey] = a_mod.formatVersion;
		return sparse;
	}

	void SettingsStore::MarkDirty(Mod& a_mod)
	{
		if (!a_mod.dirty) {
			a_mod.dirty = true;
			// The window opens at the first unflushed change and is not pushed
			// back by later ones, so a continuous slider drag still lands on
			// disk every kPersistDelaySeconds, not only when it ends.
			a_mod.dueAt = _now + kPersistDelaySeconds;
		}
	}

	bool SettingsStore::PersistNow(Mod& a_mod) const
	{
		if (!Persist(a_mod)) {
			// Keep the pending write alive, but back off so a read-only or full filesystem does not turn every frame into another write attempt.
			a_mod.dueAt = _now + kPersistDelaySeconds;
			return false;
		}
		a_mod.dirty = false;
		REX::DEBUG("SettingsStore: saved '{}' values", a_mod.id);
		for (const auto& listener : _persistListeners) {
			if (listener) {
				listener(a_mod.id);
			}
		}
		return true;
	}

	void SettingsStore::PumpPersistence(double a_nowSeconds)
	{
		_now = a_nowSeconds;
		for (auto& mod : _mods) {
			if (mod.dirty && _now >= mod.dueAt) {
				(void)PersistNow(mod);
			}
		}
	}

	void SettingsStore::FlushPersistence()
	{
		for (auto& mod : _mods) {
			if (mod.dirty) {
				(void)PersistNow(mod);
			}
		}
	}

	bool SettingsStore::Persist(const Mod& a_mod)
	{
		std::error_code ec;
		std::filesystem::create_directories(a_mod.valuesPath.parent_path(), ec);

		// Temp file + rename so a crash mid-write can't corrupt existing values.
		const auto tmp = std::filesystem::path(a_mod.valuesPath).concat(".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out) {
				REX::ERROR("SettingsStore: cannot write {}", tmp.string());
				return false;
			}
			out << Json::Dump(SparseValues(a_mod), 2);
			out.close();  // flush now so a disk-full / IO error surfaces before the rename
			if (!out) {
				REX::ERROR("SettingsStore: write to {} failed (disk full/IO?); keeping existing values", tmp.string());
				std::filesystem::remove(tmp, ec);
				return false;
			}
		}
		std::filesystem::rename(tmp, a_mod.valuesPath, ec);
		if (ec) {
			REX::ERROR("SettingsStore: cannot replace {} ({})", a_mod.valuesPath.string(), ec.message());
			std::filesystem::remove(tmp, ec);
			return false;
		}
		return true;
	}
}
