#include "runtime/SettingsStore.h"

#include <array>
#include <cmath>
#include <unordered_set>

#include "core/Log.h"
#include "core/Version.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::size_t kMaxStringLen = 256;
		constexpr std::size_t kMaxModIdLen = Ids::kMaxModIdLen;
		constexpr std::size_t kMaxInputContextIdLen = 64;

		// Reserved meta key: the schema `version` the file was last written under
		// (mcm-design.md §11). `$`-prefixed so it can never collide with a setting
		// key, and is ignored by builds that predate versioning.
		constexpr const char* kSchemaVersionKey = "$schemaVersion";

		// Values-file encoding version, distinct from the mod's schema `version`.
		// Stamped on every rewrite; a newer host's higher stamp round-trips
		// (Mod::formatVersion keeps the max).
		constexpr const char*  kFormatVersionKey = "$formatVersion";
		constexpr std::int64_t kValuesFormatVersion = 1;

		bool IsValidInputContextId(std::string_view a_id)
		{
			if (a_id.empty() || a_id.size() > kMaxInputContextIdLen) {
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

		bool IsHexColor(std::string_view a_s)
		{
			if ((a_s.size() != 7 && a_s.size() != 9) || a_s.front() != '#') {
				return false;
			}
			return std::all_of(a_s.begin() + 1, a_s.end(), [](unsigned char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
			});
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

		std::string StableId(const nlohmann::json& a_object, std::string_view a_field, std::size_t a_index)
		{
			auto id = Json::GetString(a_object, a_field, "");
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

			if (auto contexts = a_schema.find("inputContexts"); contexts != a_schema.end() && contexts->is_array()) {
				for (std::size_t i = 0; i < contexts->size(); ++i) {
					auto& context = (*contexts)[i];
					if (context.is_object()) {
						const auto id = StableId(context, "id", i);
						ResolveField(context, "label", "inputContexts." + id + ".label", a_modId, a_resolver);
					}
				}
			}

			if (auto presets = a_schema.find("presets"); presets != a_schema.end() && presets->is_array()) {
				for (std::size_t i = 0; i < presets->size(); ++i) {
					auto& preset = (*presets)[i];
					if (!preset.is_object()) continue;
					const auto id = StableId(preset, "id", i);
					const auto root = "presets." + id;
					ResolveField(preset, "label", root + ".label", a_modId, a_resolver);
					ResolveField(preset, "description", root + ".description", a_modId, a_resolver);
				}
			}

			auto groups = a_schema.find("groups");
			if (groups == a_schema.end() || !groups->is_array()) {
				return;
			}
			for (std::size_t groupIndex = 0; groupIndex < groups->size(); ++groupIndex) {
				auto& group = (*groups)[groupIndex];
				if (!group.is_object()) continue;
				const auto groupId = StableId(group, "id", groupIndex);
				ResolveField(group, "label", "groups." + groupId + ".label", a_modId, a_resolver);
				auto items = group.find("settings");
				if (items == group.end() || !items->is_array()) continue;
				for (std::size_t itemIndex = 0; itemIndex < items->size(); ++itemIndex) {
					auto& item = (*items)[itemIndex];
					if (!item.is_object()) continue;
					const auto type = Json::GetString(item, "type", "");
					const auto key = Json::GetString(item, "key", "");
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
				}
			}
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
		_loadErrors.clear();
		_valuesDir = a_valuesDir;
		_loaded = true;
		++_generation;

		std::error_code ec;
		if (!std::filesystem::is_directory(a_schemaDir, ec)) {
			REX::WARN("SettingsStore: no schema directory at {} — settings UI will be empty", a_schemaDir.string());
			return;
		}

		// Sort by filename: directory_iterator order is unspecified, and
		// duplicate-id resolution (first wins) must be deterministic.
		std::vector<std::filesystem::path> files;
		for (const auto& entry : std::filesystem::directory_iterator(a_schemaDir, ec)) {
			if (entry.is_regular_file() && entry.path().extension() == ".json") {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());

		for (const auto& path : files) {
			// Drop-in id == filename stem, so the stem must pass the id grammar.
			// Rejected here, not in ValidateSchemaShape, so the log names the file.
			if (const auto stem = path.stem().string(); !Ids::IsAcceptedModId(stem)) {
				REX::ERROR("SettingsStore: skipping {} — settings files are named '<author>.<modname>.json' "
						   "(lowercase [a-z0-9-] segments, exactly one dot in the mod id); "
						   "dotless ids are reserved for the platform",
					path.string());
				RecordLoadError("schema-name", path.filename().string(), "",
					"file name is not a mod id — settings files are named '<author>.<modname>.json'");
				continue;
			}
			std::string parseError;
			auto schema = Json::ParseFile(path, parseError);
			if (!schema || !schema->is_object()) {
				const auto why = schema ? std::string("not a JSON object") : parseError;
				REX::ERROR("SettingsStore: skipping {} — {}", path.string(), why);
				RecordLoadError("schema-parse", path.filename().string(), "", why);
				continue;
			}
			// Startup load: notifications defer to the OnStart NotifyAll.
			AddSchema(std::move(*schema), Source::kDropIn, path.stem().string(), /*a_notify=*/false, /*a_dropInReplace=*/false, path);
		}

		REX::INFO("SettingsStore: {} mod schema(s) registered from {}", _mods.size(), a_schemaDir.string());
	}

	bool SettingsStore::RegisterSchema(nlohmann::json a_schema, Source a_source)
	{
		if (!_loaded) {
			// Registrations arrive via the main-thread pump, which only runs
			// after Runtime::Initialize → LoadAll. Reject rather than register
			// with no values directory.
			REX::WARN("SettingsStore: RegisterSchema before LoadAll — rejected");
			return false;
		}
		return AddSchema(std::move(a_schema), a_source, /*a_idHint=*/"", /*a_notify=*/true);
	}

	bool SettingsStore::ValidateSchemaShape(const nlohmann::json& a_schema)
	{
		if (!a_schema.is_object()) {
			REX::WARN("SettingsStore: rejected schema — not a JSON object");
			return false;
		}
		const auto id = Json::GetString(a_schema, "id", "");
		if (id.empty()) {
			REX::WARN("SettingsStore: rejected schema with no id");
			return false;
		}
		// Id grammar: <author>.<modname>, lowercase [a-z0-9-] segments, exactly
		// one dot. Dotless ids are platform-reserved ("osfui" is the only
		// built-in), which subsumes the old reserved-word list (ui/menu/hud/...).
		if (!Ids::IsAcceptedModId(id)) {
			REX::ERROR("SettingsStore: rejected schema id '{}' — mod ids are '<author>.<modname>' "
					   "(lowercase [a-z0-9-] segments, exactly one dot, max {} chars); "
					   "dotless ids are reserved for the platform",
				id.substr(0, kMaxModIdLen), kMaxModIdLen);
			return false;
		}
		return true;
	}

	bool SettingsStore::ReloadDropInFile(const std::filesystem::path& a_path)
	{
		std::string parseError;
		auto schema = Json::ParseFile(a_path, parseError);
		if (!schema || !schema->is_object()) {
			const auto why = schema ? std::string("not a JSON object") : parseError;
			REX::WARN("SettingsStore: hot-reload skipped — {}: {}", a_path.string(), why);
			// Record (replace-or-add) and re-broadcast so an open Mods surface
			// shows the parse error now, not on the next menu open. No generation
			// bump: the registry shape is unchanged.
			RecordLoadError("schema-parse", a_path.filename().string(), "", why);
			NotifyRegistryChanged();
			return false;
		}
		// A fixed file drops its banner entry: a successful AddSchema below
		// re-broadcasts; if it is refused (a runtime registration outranks the
		// file) the stale entry is still gone on the next fetch.
		EraseLoadErrorsForFile(a_path.filename().string());
		return AddSchema(std::move(*schema), Source::kDropIn, a_path.stem().string(),
			/*a_notify=*/true, /*a_dropInReplace=*/true, a_path);
	}

	bool SettingsStore::AddSchema(nlohmann::json a_schema, Source a_source, std::string a_idHint, bool a_notify, bool a_dropInReplace, std::filesystem::path a_sourcePath)
	{
		if (!a_schema.is_object()) {
			REX::WARN("SettingsStore: rejected schema — not a JSON object");
			return false;
		}
		auto id = Json::GetString(a_schema, "id", a_idHint);
		// Drop-ins: the id must equal the filename stem (documented contract,
		// mcm-design.md §8.1) — warn and override, so a file can't hijack another
		// mod's id and MO2's per-file VFS priority stays the arbiter of who owns
		// settings/<id>.json.
		if (a_source == Source::kDropIn && !a_idHint.empty() && id != a_idHint) {
			REX::WARN("SettingsStore: schema id '{}' must equal the filename stem — using '{}'",
				id.substr(0, kMaxModIdLen), a_idHint);
			id = a_idHint;
		}
		a_schema["id"] = id;  // the document the web layer sees carries the effective id
		if (!ValidateSchemaShape(a_schema)) {
			return false;
		}
		// Unknown top-level keys are the normal compatible case (a newer schema on
		// an older host), so devMode only.
		if (Log::DevMode()) {
			Json::ReportUnknownKeys(a_schema,
				{ "id", "title", "description", "version", "targetVersion", "accent",
					"presets", "inputContexts", "groups" },
				"SettingsStore: schema '" + id + "'", /*a_warn=*/false);
		}

		// Source precedence on id collision (mcm-design.md §14.1): a runtime (DLL)
		// registration replaces a drop-in file (so upgrading tiers needs no
		// hand-deleted JSON) and its own earlier registration (dev iteration).
		// A drop-in replaces nothing; duplicate drop-in ids are first-wins, with
		// MO2's per-file VFS priority as the intended arbiter (§8.1).
		Mod* existing = FindMod(id);
		if (existing) {
			if (a_source == Source::kDropIn &&
				!(a_dropInReplace && existing->source == Source::kDropIn)) {
				// First-wins: log both files and record the loser so Data() can
				// surface the conflict.
				const auto kept = existing->source == Source::kNative
				                      ? std::string("the runtime registration")
				                      : (existing->schemaPath.empty() ? std::string("the first-loaded schema")
				                                                      : existing->schemaPath.string());
				REX::ERROR("SettingsStore: duplicate schema id '{}' — keeping {}, ignoring {}",
					id, kept, a_sourcePath.empty() ? std::string("the drop-in file") : a_sourcePath.string());
				// File-vs-file collisions only; a native registration outranking
				// its own drop-in file is the tier-upgrade path, not a conflict.
				if (!a_sourcePath.empty() && existing->source == Source::kDropIn) {
					const auto loser = a_sourcePath.filename().string();
					if (std::find(existing->shadowed.begin(), existing->shadowed.end(), loser) == existing->shadowed.end()) {
						existing->shadowed.push_back(loser);
						InvalidateData();
					}
				}
				return false;
			}
			if (a_source == Source::kDropIn) {
				REX::INFO("SettingsStore: hot-reloading drop-in schema '{}'", id);
			} else {
				REX::WARN("SettingsStore: runtime registration replaces {} schema for id '{}'",
					existing->source == Source::kDropIn ? "drop-in" : "earlier runtime", id);
			}
			if (existing->dirty) {
				// The overlay below reads the values file; land any unflushed
				// write-behind changes first or the replacement reverts them.
				PersistNow(*existing);
			}
		}

		WarnInputContexts(a_schema, id);

		Mod mod;
		mod.id = std::move(id);
		mod.schema = std::move(a_schema);
		mod.valuesPath = _valuesDir / (mod.id + ".json");
		mod.schemaPath = std::move(a_sourcePath);
		mod.source = a_source;
		mod.values = nlohmann::json::object();
		mod.preserved = nlohmann::json::object();
		if (existing) {
			mod.shadowed = std::move(existing->shadowed);  // conflicts outlive a replacement/hot-reload
		}

		// Advisory host-version target, same field and semantics as a view
		// manifest's. Never gates loading: a schema authored for a newer OSF UI
		// loads best-effort (unknown types serve read-only defaults, unknown keys
		// are preserved). Carried in settings.data for the "needs update" badge.
		if (auto target = Json::GetString(mod.schema, "targetVersion", ""); !target.empty()) {
			std::array<std::uint32_t, 3> targetParts{};
			if (ParseDottedVersion(target, targetParts)) {
				mod.targetVersion = std::move(target);
				if (kPluginVersionParts < targetParts) {
					REX::WARN("SettingsStore: '{}' targets OSF UI {} but this is {} — update OSF UI",
						mod.id, mod.targetVersion, kPluginVersion);
				}
			} else {
				REX::WARN("SettingsStore: '{}' targetVersion '{}' is not '<major>[.<minor>[.<patch>]]' — ignored",
					mod.id, target);
			}
		}

		// Persisted values over schema defaults; a replacement rebuilds from the
		// same file, so added/removed/retyped keys resolve like a fresh load.
		// A corrupt file is never silently discarded (mcm-design.md §14.2: under
		// sparse persistence that is indistinguishable from "user reset
		// everything") — quarantine to <id>.json.bad, serve defaults, and surface
		// the reason in Data()'s loadErrors.
		EraseLoadErrorsForMod(mod.id);  // a clean overlay clears a stale record
		nlohmann::json saved = nlohmann::json::object();
		std::error_code fsEc;
		if (std::filesystem::exists(mod.valuesPath, fsEc)) {
			std::string parseError;
			auto parsed = Json::ParseFile(mod.valuesPath, parseError);
			if (parsed && parsed->is_object()) {
				saved = std::move(*parsed);
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

		// Version bookkeeping (mcm-design.md §11): the schema's `version`
		// (default 0) is stamped as `$schemaVersion`. `$`-prefixed keys are
		// invisible to the schema walk below (no setting may be named `$…`), so
		// older builds ignore it. The log line is triage only; alias adoption
		// below is what carries values across a rename.
		const auto schemaVersion = static_cast<std::int64_t>(Json::GetInt(mod.schema, "version", 0));
		if (const auto it = saved.find(kSchemaVersionKey); it != saved.end() && it->is_number_integer()) {
			if (const auto fileVersion = it->get<std::int64_t>(); fileVersion != schemaVersion) {
				REX::INFO("SettingsStore: '{}' values migrating v{} -> v{}", mod.id, fileVersion, schemaVersion);
			}
		}
		// Encoding-format stamp, written on every rewrite. A file from a newer
		// OSF UI keeps its higher stamp, so round-tripping through this host never
		// downgrades it. Format migrations would run here; none exist yet.
		if (const auto v = Json::GetInt(saved, kFormatVersionKey, kValuesFormatVersion); v > kValuesFormatVersion) {
			REX::INFO("SettingsStore: '{}' values file declares format v{} (this build writes v{}) — written by a newer OSF UI; unknown content rides in the preserved bag",
				mod.id, v, kValuesFormatVersion);
			mod.formatVersion = v;
		}

		// Keys this schema accounts for: declared keys and, for known-typed
		// settings, their aliases. Whatever the file holds outside this set is a
		// forward-compat unknown and goes to the opaque preserved bag.
		std::unordered_set<std::string> accounted;

		std::size_t count = 0;
		ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::GetString(a_setting, "key", "");
			if (key.empty()) {
				return false;
			}
			++count;
			accounted.insert(key);

			// Unknown-typed setting: serve the schema default read-only; the
			// saved value is preserved verbatim below, never served or wiped.
			// Its aliases stay un-accounted on purpose — this host can't adopt
			// them, and dropping them would strand a rename for the host that can.
			if (!IsKnownType(Json::GetString(a_setting, "type", ""))) {
				if (const auto it = saved.find(key); it != saved.end()) {
					mod.preserved[key] = *it;
				}
				mod.values[key] = DefaultFor(a_setting);
				return false;
			}

			if (const auto aliases = a_setting.find("aliases"); aliases != a_setting.end() && aliases->is_array()) {
				for (const auto& alias : *aliases) {
					if (alias.is_string()) {
						accounted.insert(alias.get<std::string>());
					}
				}
			}
			if (const auto it = saved.find(key); it != saved.end()) {
				if (auto valid = Validate(a_setting, *it)) {
					mod.values[key] = std::move(*valid);
					return false;
				}
			}
			// Renamed key (mcm-design.md §11): the current key is absent or no
			// longer validates, so adopt the first declared `alias` in the file
			// that does. The old key is not schema-declared, so SparseValues drops
			// it and the next write lands under the new name.
			if (const auto aliases = a_setting.find("aliases"); aliases != a_setting.end() && aliases->is_array()) {
				for (const auto& alias : *aliases) {
					if (!alias.is_string()) {
						continue;
					}
					const auto it = saved.find(alias.get<std::string>());
					if (it == saved.end()) {
						continue;
					}
					if (auto valid = Validate(a_setting, *it)) {
						REX::INFO("SettingsStore: '{}.{}' adopted from alias '{}'", mod.id, key, alias.get<std::string>());
						mod.values[key] = std::move(*valid);
						return false;
					}
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
		if (!mod.preserved.empty() && Log::DevMode()) {
			REX::DEBUG("SettingsStore: '{}' preserving {} entr{} this host can't understand (kept verbatim, not served)",
				mod.id, mod.preserved.size(), mod.preserved.size() == 1 ? "y" : "ies");
		}

		// Prune-to-default on load (mcm-design.md §8.1 + §11): a file differing
		// from its sparse form — full legacy file, saved value now equal to an
		// updated default, adopted alias, clamping, stale `$schemaVersion` —
		// schedules a rewrite so those knobs track upstream defaults again.
		// Preserved unknowns are part of the sparse form, and a missing
		// `$formatVersion` alone is clean: stamping never triggers a rewrite.
		if (const auto expected = SparseValues(mod); saved != expected) {
			auto stampedOnly = saved;
			stampedOnly[kFormatVersionKey] = mod.formatVersion;
			if (stampedOnly != expected) {
				MarkDirty(mod);
			}
		}

		REX::INFO("SettingsStore: loaded mod '{}' ('{}', {} settings)",
			mod.id, Json::GetString(mod.schema, "title", mod.id), count);

		if (existing) {
			*existing = std::move(mod);
		} else {
			_mods.push_back(std::move(mod));
		}
		InvalidateData();
		++_generation;

		if (a_notify) {
			// Replay so consumers that subscribed before this mod registered
			// sync without a separate read (mcm-design.md §10), then announce
			// the shape change so the web layer re-broadcasts the registry.
			NotifyMod(existing ? existing->id : _mods.back().id);
			NotifyRegistryChanged();
		}
		return true;
	}

	bool SettingsStore::RemoveMod(std::string_view a_modId)
	{
		const auto it = std::find_if(_mods.begin(), _mods.end(),
			[&](const Mod& a_mod) { return a_mod.id == a_modId; });
		if (it == _mods.end()) {
			return false;
		}
		if (it->dirty) {
			PersistNow(*it);  // the kept values file must carry the last changes
		}
		REX::INFO("SettingsStore: removed mod '{}' (values file kept)", it->id);
		EraseLoadErrorsForMod(it->id);  // its banner entry leaves with it
		_mods.erase(it);
		InvalidateData();
		++_generation;
		NotifyRegistryChanged();
		return true;
	}

	void SettingsStore::RecordLoadError(std::string a_kind, std::string a_file, std::string a_mod, std::string a_message)
	{
		InvalidateData();
		for (auto& e : _loadErrors) {
			if (e.kind == a_kind && e.file == a_file && e.mod == a_mod) {
				e.message = std::move(a_message);
				return;
			}
		}
		_loadErrors.push_back({ std::move(a_kind), std::move(a_file), std::move(a_mod), std::move(a_message) });
	}

	bool SettingsStore::EraseLoadErrorsForFile(std::string_view a_file)
	{
		const auto count = std::erase_if(_loadErrors,
			[&](const LoadError& a_e) { return a_e.mod.empty() && a_e.file == a_file; });
		if (count > 0) {
			InvalidateData();
		}
		return count > 0;
	}

	bool SettingsStore::EraseLoadErrorsForMod(std::string_view a_modId)
	{
		const auto count = std::erase_if(_loadErrors,
			[&](const LoadError& a_e) { return !a_e.mod.empty() && a_e.mod == a_modId; });
		if (count > 0) {
			InvalidateData();
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
		return setting ? Json::GetString(*setting, "type", "") : std::string{};
	}

	std::optional<std::string> SettingsStore::CanonicalEnumValue(std::string_view a_modId, std::string_view a_key, std::string_view a_value) const
	{
		const auto* mod = FindMod(a_modId);
		const auto* setting = mod ? FindSetting(*mod, a_key) : nullptr;
		if (!setting || Json::GetString(*setting, "type", "") != "enum") {
			return std::nullopt;
		}
		const auto options = setting->find("options");
		if (options == setting->end() || !options->is_array()) {
			return std::nullopt;
		}
		for (const auto& opt : *options) {
			if (opt.is_string() && Ids::EqualsCaseInsensitiveAscii(opt.get_ref<const std::string&>(), a_value)) {
				return opt.get<std::string>();
			}
		}
		return std::nullopt;
	}

	std::optional<SettingsStore::Source> SettingsStore::GetSource(std::string_view a_modId) const
	{
		const auto* mod = FindMod(a_modId);
		return mod ? std::optional(mod->source) : std::nullopt;
	}

	std::vector<SettingsStore::KeySetting> SettingsStore::KeySettings() const
	{
		std::vector<KeySetting> out;
		for (const auto& mod : _mods) {
			ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
				if (Json::GetString(a_setting, "type", "") == "key") {
					const auto key = Json::GetString(a_setting, "key", "");
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

	SettingsStore::InputContext SettingsStore::ResolveInputContext(const Mod& a_mod, const nlohmann::json& a_setting) const
	{
		InputContext fallback;
		const auto ref = Json::GetString(a_setting, "inputContext", "");
		if (ref.empty() || ref == "gameplay" || !IsValidInputContextId(ref)) {
			return fallback;
		}

		const auto contexts = a_mod.schema.find("inputContexts");
		if (contexts == a_mod.schema.end() || !contexts->is_array()) {
			return fallback;
		}
		std::unordered_set<std::string> seen;
		for (const auto& context : *contexts) {
			if (!context.is_object()) {
				continue;
			}
			const auto id = Json::GetString(context, "id", "");
			if (id == "gameplay" || !IsValidInputContextId(id) || !seen.insert(id).second) {
				continue;
			}
			if (id == ref) {
				auto label = Json::GetString(context, "label", id);
				if (label.empty()) {
					label = id;
				}
				if (_textResolver) {
					label = _textResolver(a_mod.id, "inputContexts." + id + ".label", label);
				}
				return { id, std::move(label), Json::GetBool(context, "blocksGameplay", false) };
			}
		}
		return fallback;
	}

	void SettingsStore::WarnInputContexts(const nlohmann::json& a_schema, std::string_view a_modId)
	{
		const auto contexts = a_schema.find("inputContexts");
		if (contexts == a_schema.end()) {
			return;
		}
		if (!contexts->is_array()) {
			REX::WARN("SettingsStore: '{}.inputContexts' must be an array -- key contexts fall back to gameplay", a_modId);
			return;
		}
		std::unordered_set<std::string> seen;
		for (const auto& context : *contexts) {
			const auto id = context.is_object() ? Json::GetString(context, "id", "") : std::string{};
			if (id == "gameplay") {
				REX::WARN("SettingsStore: '{}.inputContexts' cannot redefine reserved context 'gameplay' -- ignoring it", a_modId);
				continue;
			}
			if (!IsValidInputContextId(id)) {
				REX::WARN("SettingsStore: '{}' has an invalid input context id -- ignoring it", a_modId);
				continue;
			}
			if (!seen.insert(id).second) {
				REX::WARN("SettingsStore: '{}' defines input context '{}' more than once -- keeping the first", a_modId, id);
			}
		}
	}

	std::vector<SettingsStore::BoundKey> SettingsStore::ResolveBoundKeys() const
	{
		std::vector<BoundKey> bound;
		if (_keyResolver) {
			for (const auto& setting : KeySettings()) {
				if (const auto vk = _keyResolver(setting.name); vk != 0) {
					const auto* mod = FindMod(setting.modId);
					bool blocksGameplay = false;
					if (mod) {
						if (const auto* authored = FindSetting(*mod, setting.key)) {
							blocksGameplay = ResolveInputContext(*mod, *authored).blocksGameplay;
						}
					}
					auto title = mod ? Json::GetString(mod->schema, "title", mod->id) : setting.modId;
					if (mod && _textResolver) title = _textResolver(mod->id, "settings.title", title);
					bound.push_back({ setting.modId, setting.key, std::move(title), vk, blocksGameplay });
				}
			}
			// The game's own bindings (mcm-design.md §9): pseudo-entries under
			// the reserved id "@game". Gated on the same resolver as mod
			// settings — without one there is no conflict grouping at all.
			for (const auto& vanilla : _vanillaKeys) {
				if (vanilla.vk != 0) {
					bound.push_back({ "@game", vanilla.event, vanilla.title, vanilla.vk, false });
				}
			}
		}
		return bound;
	}

	nlohmann::json SettingsStore::ConflictsFor(std::uint32_t a_vk, std::string_view a_excludeMod, std::string_view a_excludeKey) const
	{
		nlohmann::json conflicts = nlohmann::json::array();
		if (a_vk == 0) {
			return conflicts;  // unresolvable: never conflicts (mirrors Data())
		}
		bool blocksGameplay = false;
		if (const auto* mod = FindMod(a_excludeMod)) {
			if (const auto* setting = FindSetting(*mod, a_excludeKey)) {
				blocksGameplay = ResolveInputContext(*mod, *setting).blocksGameplay;
			}
		}
		for (const auto& other : ResolveBoundKeys()) {
			if (other.vk == a_vk && (other.modId != a_excludeMod || other.key != a_excludeKey) &&
				!(blocksGameplay && other.modId == "@game")) {
				conflicts.push_back({
					{ "mod", other.modId },
					{ "key", other.key },
					{ "title", other.title },
				});
			}
		}
		return conflicts;
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
		// Informational key-conflict grouping (mcm-design.md §9): resolve every
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
					if (Json::GetString(a_setting, "type", "") != "key") {
						return false;
					}
					const auto key = Json::GetString(a_setting, "key", "");
					const auto self = std::find_if(bound.begin(), bound.end(),
						[&](const BoundKey& a_b) { return a_b.modId == mod.id && a_b.key == key; });
					if (self == bound.end()) {
						return false;  // unresolvable/empty value: never conflicts
					}
					nlohmann::json conflicts = nlohmann::json::array();
					for (const auto& other : bound) {
						if (other.vk == self->vk && (other.modId != mod.id || other.key != key) &&
							!(self->blocksGameplay && other.modId == "@game")) {
							conflicts.push_back({
								{ "mod", other.modId },
								{ "key", other.key },
								{ "title", other.title },
							});
						}
					}
					if (!conflicts.empty()) {
						a_setting["conflicts"] = std::move(conflicts);
					}
					return false;
				});
			}
			nlohmann::json entry{
				{ "id", mod.id },
				{ "title", Json::GetString(schema, "title", mod.id) },
				{ "schema", std::move(schema) },
				{ "values", mod.values },
			};
			// Additive field: drop-in files that also claimed this id and lost
			// first-wins; the Mods surface badges the conflict. Omitted if none.
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
		// The game's own bindings as a top-level table (mcm-design.md §9 + the
		// keybinds view): the full curated map, not just colliding entries.
		// Additive field (protocol 1.0), omitted when off/empty.
		if (!_vanillaKeys.empty()) {
			nlohmann::json vanilla = nlohmann::json::array();
			for (const auto& v : _vanillaKeys) {
				if (v.vk != 0 && !v.name.empty()) {
					vanilla.push_back({
						{ "event", v.event },
						{ "title", v.title },
						{ "name", v.name },
					});
				}
			}
			if (!vanilla.empty()) {
				data["vanillaKeys"] = std::move(vanilla);
			}
		}
		// Additive field: artifacts that failed to load, so the Mods surface can
		// say so instead of a mod silently vanishing (§14.2). Omitted when clean.
		if (!_loadErrors.empty()) {
			nlohmann::json errors = nlohmann::json::array();
			for (const auto& e : _loadErrors) {
				nlohmann::json entry{
					{ "kind", e.kind },
					{ "file", e.file },
					{ "message", e.message },
				};
				if (!e.mod.empty()) {
					entry["mod"] = e.mod;
				}
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
		return DataView().dump();
	}

	SettingsStore::Mod* SettingsStore::FindMod(std::string_view a_modId)
	{
		for (auto& mod : _mods) {
			if (mod.id == a_modId) {
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
			if (Json::GetString(a_setting, "key", "") == a_key) {
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
		const auto type = Json::GetString(a_setting, "type", "");

		if (type == "bool") {
			if (a_value.is_boolean()) {
				return a_value;
			}
		} else if (type == "int" || type == "float") {
			if (a_value.is_number()) {
				double v = a_value.get<double>();
				if (const auto lo = a_setting.find("min"); lo != a_setting.end() && lo->is_number()) {
					v = (std::max)(v, lo->get<double>());
				}
				if (const auto hi = a_setting.find("max"); hi != a_setting.end() && hi->is_number()) {
					v = (std::min)(v, hi->get<double>());
				}
				if (type == "int") {
					return nlohmann::json(static_cast<std::int64_t>(std::llround(v)));
				}
				return nlohmann::json(v);
			}
		} else if (type == "enum") {
			if (a_value.is_string()) {
				const auto options = a_setting.find("options");
				if (options != a_setting.end() && options->is_array()) {
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
				const auto options = a_setting.find("options");
				if (options != a_setting.end() && options->is_array()) {
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
				if (Json::GetString(a_setting, "widget", "") == "color" && !IsHexColor(s)) {
					return std::nullopt;
				}
				// Per-setting maxLength, capped by the store-wide hard limit.
				// Raising kMaxStringLen requires bumping the renderer and mock
				// clamps in lockstep.
				std::size_t cap = kMaxStringLen;
				if (const auto ml = a_setting.find("maxLength"); ml != a_setting.end() && ml->is_number_integer()) {
					if (const auto v = ml->get<std::int64_t>(); v > 0) {
						cap = (std::min)(cap, static_cast<std::size_t>(v));
					}
				}
				if (s.size() > cap) {
					s.resize(cap);
				}
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
					if (Json::GetBool(a_setting, "allowUnbound", false)) {
						return nlohmann::json(std::move(s));
					}
				} else {
					constexpr std::size_t kMaxKeyNameLen = 16;
					if (s.size() > kMaxKeyNameLen) {
						s.resize(kMaxKeyNameLen);
					}
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
		const auto parsed = Json::Parse(a_valueJson, "settings value");
		if (!parsed) {
			return { false, "invalid-value" };
		}
		return SetValueWithResult(a_modId, a_key, *parsed);
	}

	SettingsStore::SetResult SettingsStore::SetValueWithResult(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		auto* mod = FindMod(a_modId);
		if (!mod) {
			REX::WARN("SettingsStore: rejected set for unknown mod '{}'", a_modId.substr(0, 64));
			return { false, "unknown-setting" };
		}
		const auto* setting = FindSetting(*mod, a_key);
		if (!setting) {
			REX::WARN("SettingsStore: rejected unknown setting '{}.{}'", a_modId.substr(0, 64), a_key.substr(0, 64));
			return { false, "unknown-setting" };
		}
		// A type this host doesn't know serves its default read-only. Its own
		// result code, so a view can say "needs a newer OSF UI" rather than
		// "bad value".
		if (!IsKnownType(Json::GetString(*setting, "type", ""))) {
			REX::WARN("SettingsStore: rejected set for '{}.{}' — unknown type '{}' is served read-only",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::GetString(*setting, "type", "?").substr(0, 32));
			return { false, "read-only" };
		}
		auto valid = Validate(*setting, a_value);
		if (!valid) {
			REX::WARN("SettingsStore: rejected invalid value for '{}.{}' (type {})",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::GetString(*setting, "type", "?"));
			return { false, "invalid-value" };
		}

		const std::string key{ a_key };
		mod->values[key] = std::move(*valid);
		InvalidateData();
		MarkDirty(*mod);  // notification immediate; disk write coalesced (PumpPersistence)
		Notify(mod->id, key, mod->values[key]);
		if (Log::DevMode()) {
			REX::DEBUG("SettingsStore: set '{}.{}' = {}", mod->id, key, mod->values[key].dump().substr(0, 128));
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
				const auto key = Json::GetString(a_setting, "key", "");
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
		// Sparse persistence (mcm-design.md §8.1): only a value != the schema
		// default is the user's; anything equal to the default keeps tracking
		// upstream default changes, and reset-to-default means key removal. Full
		// legacy files shed their frozen defaults the first time through here.
		nlohmann::json sparse = nlohmann::json::object();
		ForEachSetting(a_mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::GetString(a_setting, "key", "");
			if (!key.empty()) {
				if (const auto it = a_mod.values.find(key); it != a_mod.values.end() && *it != DefaultFor(a_setting)) {
					sparse[key] = *it;
				}
			}
			return false;
		});
		// Forward-compat opaques ride every rewrite verbatim, so a newer mod's
		// settings survive this host. Unknown-typed declared keys never enter the
		// loop above (their served value is the default), so no collision here.
		for (const auto& [key, value] : a_mod.preserved.items()) {
			sparse[key] = value;
		}
		// Stamp the schema version (mcm-design.md §11) only when a mod uses
		// versioning: a v0 (unversioned) mod's file stays byte-for-byte as before,
		// so no existing values file is dirtied by this feature. The schema's
		// `version` is the source of truth; the stamp records "last written under
		// vN", and reverting to v0 drops it — a version move like any other.
		if (const auto v = Json::GetInt(a_mod.schema, "version", 0); v != 0) {
			sparse[kSchemaVersionKey] = v;
		}
		// Every rewrite carries the encoding version. a_mod.formatVersion is
		// max(known, loaded), so a newer host's stamp survives this host's
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

	void SettingsStore::PersistNow(Mod& a_mod) const
	{
		a_mod.dirty = false;  // a write failure is logged in Persist; the next change retries
		if (!Persist(a_mod)) {
			return;
		}
		REX::INFO("SettingsStore: saved '{}' values", a_mod.id);
		for (const auto& listener : _persistListeners) {
			if (listener) {
				listener(a_mod.id);
			}
		}
	}

	void SettingsStore::PumpPersistence(double a_nowSeconds)
	{
		_now = a_nowSeconds;
		for (auto& mod : _mods) {
			if (mod.dirty && _now >= mod.dueAt) {
				PersistNow(mod);
			}
		}
	}

	void SettingsStore::FlushPersistence()
	{
		for (auto& mod : _mods) {
			if (mod.dirty) {
				PersistNow(mod);
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
			out << SparseValues(a_mod).dump(2);
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
