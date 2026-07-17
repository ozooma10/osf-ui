#include "runtime/SettingsStore.h"

#include <cmath>
#include <unordered_set>

#include "core/Log.h"
#include "runtime/Capabilities.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::size_t kMaxStringLen = 256;
		constexpr std::size_t kMaxModIdLen = Ids::kMaxModIdLen;
		constexpr std::size_t kMaxInputContextIdLen = 64;

		// Reserved meta key in a values file: the schema `version` the file was
		// last written under (mcm-design.md §11). `$`-prefixed so it can never
		// collide with a setting key (the schema walk only sees declared keys)
		// and is invisible to builds that predate versioning.
		constexpr const char* kSchemaVersionKey = "$schemaVersion";

		// Values-file ENCODING version (api-freeze-plan item 8) — the sparse
		// format itself, distinct from the mod's schema `version` above.
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

		// The frozen base type set (api-freeze-plan item 2). A setting whose
		// declared type is outside it is a FORWARD-COMPAT case, not an error:
		// serve the schema default read-only and preserve the saved value
		// opaquely — never the old wipe-on-load.
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
		// return true to stop early. Templated on the json's constness so
		// Data() can annotate its schema COPY through the same walk.
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
	}

	SettingsStore::~SettingsStore()
	{
		FlushPersistence();
	}

	void SettingsStore::LoadAll(const std::filesystem::path& a_schemaDir, const std::filesystem::path& a_valuesDir)
	{
		_mods.clear();
		_valuesDir = a_valuesDir;
		_loaded = true;
		++_generation;

		std::error_code ec;
		if (!std::filesystem::is_directory(a_schemaDir, ec)) {
			REX::WARN("SettingsStore: no schema directory at {} — settings UI will be empty", a_schemaDir.string());
			return;
		}

		// Collect + sort by filename: directory_iterator order is unspecified,
		// and duplicate-id resolution (first wins) must be deterministic.
		std::vector<std::filesystem::path> files;
		for (const auto& entry : std::filesystem::directory_iterator(a_schemaDir, ec)) {
			if (entry.is_regular_file() && entry.path().extension() == ".json") {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());

		for (const auto& path : files) {
			// Drop-in id == filename stem (enforced below), so the stem must
			// pass the id grammar — hard-reject here to name the offending FILE
			// (ValidateSchemaShape would only name the id).
			if (const auto stem = path.stem().string(); !Ids::IsAcceptedModId(stem)) {
				REX::ERROR("SettingsStore: skipping {} — settings files are named '<author>.<modname>.json' "
						   "(lowercase [a-z0-9-] segments, exactly one dot in the mod id); "
						   "dotless ids are reserved for the platform",
					path.string());
				continue;
			}
			auto schema = Json::ParseFile(path);
			if (!schema || !schema->is_object()) {
				REX::WARN("SettingsStore: skipping invalid schema {}", path.string());
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
			// By construction registrations arrive via the main-thread pump,
			// which only runs after Runtime::Initialize → LoadAll. Reject
			// loudly rather than register with no values directory.
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
		// Id grammar (docs/api-freeze-plan.md item 1): <author>.<modname>,
		// lowercase [a-z0-9-] segments, exactly one dot. Dotless ids are
		// platform-reserved by construction ("osfui" is the only built-in), so
		// the old reserved-word list (ui/menu/hud/...) is subsumed — every
		// reserved name is dotless and therefore already invalid.
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
		auto schema = Json::ParseFile(a_path);
		if (!schema || !schema->is_object()) {
			REX::WARN("SettingsStore: hot-reload skipped — {} is not valid JSON", a_path.string());
			return false;
		}
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
		// Drop-ins: the id MUST equal the filename stem (documented contract,
		// docs/schema + mcm-design.md §8.1) — warn and override, so a file
		// can't hijack another mod's id and MO2's per-file VFS priority stays
		// the arbiter of who owns settings/<id>.json.
		if (a_source == Source::kDropIn && !a_idHint.empty() && id != a_idHint) {
			REX::WARN("SettingsStore: schema id '{}' must equal the filename stem — using '{}'",
				id.substr(0, kMaxModIdLen), a_idHint);
			id = a_idHint;
		}
		a_schema["id"] = id;  // the document the web layer sees carries the effective id
		if (!ValidateSchemaShape(a_schema)) {
			return false;
		}
		// Author-shipped file: unknown top-level keys are the NORMAL compatible
		// case (a newer schema on an older host — item 8), so devMode INFO only.
		if (Log::DevMode()) {
			Json::ReportUnknownKeys(a_schema,
				{ "id", "title", "description", "version", "requires", "accent",
					"presets", "inputContexts", "groups" },
				"SettingsStore: schema '" + id + "'", /*a_warn=*/false);
		}

		// Source precedence on id collision (mcm-design.md §14.1): a runtime
		// (DLL) registration replaces a drop-in file — a mod upgrading tiers
		// must not require users to hand-delete the stale JSON — and replaces
		// its own earlier registration (dev iteration). A drop-in never
		// replaces anything: duplicate drop-in ids resolve first-wins (MO2's
		// per-file VFS priority is the intended arbiter, §8.1).
		Mod* existing = FindMod(id);
		if (existing) {
			if (a_source == Source::kDropIn &&
				!(a_dropInReplace && existing->source == Source::kDropIn)) {
				// First-wins (api-freeze-plan item 1): ERROR naming both files,
				// and record the loser so Data() can surface the conflict.
				const auto kept = existing->source == Source::kNative
				                      ? std::string("the runtime registration")
				                      : (existing->schemaPath.empty() ? std::string("the first-loaded schema")
				                                                      : existing->schemaPath.string());
				REX::ERROR("SettingsStore: duplicate schema id '{}' — keeping {}, ignoring {}",
					id, kept, a_sourcePath.empty() ? std::string("the drop-in file") : a_sourcePath.string());
				// Conflict record only for file-vs-file collisions; a native
				// registration outranking its own drop-in file is the normal
				// tier-upgrade path, not a conflict to badge.
				if (!a_sourcePath.empty() && existing->source == Source::kDropIn) {
					const auto loser = a_sourcePath.filename().string();
					if (std::find(existing->shadowed.begin(), existing->shadowed.end(), loser) == existing->shadowed.end()) {
						existing->shadowed.push_back(loser);
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
				// The overlay below reads the values FILE; land any unflushed
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

		// Capability gate (api-freeze-plan item 2): a schema may declare
		// "requires": ["type:flags", ...]. Anything this host doesn't satisfy
		// ⇒ register as an inert STUB card — the Mods surface renders "needs
		// a newer OSF UI", no values are loaded/served, and the values file
		// stays byte-untouched for the host that CAN satisfy it. A malformed
		// entry counts as unmet (it names something we can't understand).
		if (const auto req = mod.schema.find("requires"); req != mod.schema.end() && req->is_array()) {
			for (const auto& r : *req) {
				const auto cap = r.is_string() ? r.get<std::string>() : std::string{};
				if (cap.empty() || !Caps::Has(cap)) {
					mod.missingRequires.push_back(cap.empty() ? "(malformed)" : cap);
				}
			}
		}
		if (!mod.missingRequires.empty()) {
			mod.stub = true;
			std::string list;
			for (const auto& c : mod.missingRequires) {
				list += (list.empty() ? "" : ", ") + c;
			}
			REX::WARN("SettingsStore: '{}' requires capabilities this OSF UI lacks ({}) — "
					  "registered as a stub; values file left untouched",
				mod.id, list);
			if (existing) {
				*existing = std::move(mod);
			} else {
				_mods.push_back(std::move(mod));
			}
			++_generation;
			if (a_notify) {
				NotifyRegistryChanged();  // nothing to replay — a stub serves no values
			}
			return true;
		}

		// Persisted values over schema defaults. Replacement rebuilds from the
		// same file — every committed Set persists, so nothing is lost, and
		// added/removed/retyped keys resolve exactly like a fresh load.
		nlohmann::json saved = nlohmann::json::object();
		if (auto parsed = Json::ParseFile(mod.valuesPath); parsed && parsed->is_object()) {
			saved = std::move(*parsed);
		}

		// Version bookkeeping (mcm-design.md §11): the schema's declared
		// `version` (default 0) is stamped into the values file as the
		// reserved `$schemaVersion` meta key. `$`-prefixed keys are invisible
		// to the schema walk below (no setting may be named `$…`), so an older
		// build simply ignores it. Log a version move for support triage; the
		// alias adoption below is what actually carries values across a rename.
		const auto schemaVersion = static_cast<std::int64_t>(Json::GetInt(mod.schema, "version", 0));
		if (const auto it = saved.find(kSchemaVersionKey); it != saved.end() && it->is_number_integer()) {
			if (const auto fileVersion = it->get<std::int64_t>(); fileVersion != schemaVersion) {
				REX::INFO("SettingsStore: '{}' values migrating v{} -> v{}", mod.id, fileVersion, schemaVersion);
			}
		}
		// Values-file FORMAT stamp (api-freeze-plan item 8, distinct from the
		// schema version above: this one is the ENCODING's). Written on every
		// rewrite; a file from a newer OSF UI keeps its higher stamp verbatim
		// (max below) so round-tripping through this host never "downgrades"
		// it. Migrations would run here (none exist yet — the hook is the point).
		if (const auto v = Json::GetInt(saved, kFormatVersionKey, kValuesFormatVersion); v > kValuesFormatVersion) {
			REX::INFO("SettingsStore: '{}' values file declares format v{} (this build writes v{}) — written by a newer OSF UI; unknown content rides in the preserved bag",
				mod.id, v, kValuesFormatVersion);
			mod.formatVersion = v;
		}

		// Keys this schema accounts for: declared keys and (for KNOWN-typed
		// settings) their aliases. Whatever the file holds OUTSIDE this set
		// is a forward-compat unknown and goes to the opaque preserved bag.
		std::unordered_set<std::string> accounted;

		std::size_t count = 0;
		ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::GetString(a_setting, "key", "");
			if (key.empty()) {
				return false;
			}
			++count;
			accounted.insert(key);

			// Unknown-typed setting (item 2): serve the schema default
			// read-only; the saved value is preserved verbatim below, never
			// served, never wiped. Its aliases stay UN-accounted on purpose —
			// this host can't adopt them, and dropping them would strand a
			// rename for the newer host that can.
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
			// Renamed key (mcm-design.md §11): the current key is absent (or
			// its value no longer validates), so adopt the first declared
			// `alias` present in the file that DOES validate. The old key is
			// not schema-declared, so SparseValues drops it and the next write
			// lands under the new name — a one-way, declarative rename with no
			// version arithmetic.
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

		// Preservation (item 2): saved keys no loaded schema fact accounts
		// for — a setting that left the schema, or one from a newer schema
		// than this host has seen — round-trip opaquely instead of being
		// pruned. The two stamps ($schemaVersion, $formatVersion) stay owned
		// by their logic above; any OTHER $-key (a future format's meta) is
		// preserved like unknown settings.
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

		// Prune-to-default on load (mcm-design.md §8.1 + §11): when the file
		// differs from its sparse form — a legacy full file, a saved value
		// that now equals an updated default, an adopted alias, clamping, or
		// a stale `$schemaVersion` — schedule a rewrite so those knobs track
		// upstream defaults again instead of staying frozen forever, and so
		// the version stamp advances. Preserved unknowns are PART of the
		// sparse form, so a file whose only oddity is content this host
		// doesn't understand loads clean (no rewrite churn). A MISSING
		// `$formatVersion` alone is also clean (item 8: the stamp lands on the
		// next real write) — stamping is never the sole reason to rewrite.
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
		++_generation;

		if (a_notify) {
			// Replay so consumers that subscribed before this mod registered
			// sync without a separate read (mcm-design.md §10) — then announce
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
		_mods.erase(it);
		++_generation;
		NotifyRegistryChanged();
		return true;
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
		if (!mod || mod->stub) {
			return {};  // a stub's schema is inert — no feature may act on it
		}
		const auto* setting = FindSetting(*mod, a_key);
		return setting ? Json::GetString(*setting, "type", "") : std::string{};
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

	SettingsStore::InputContext SettingsStore::ResolveInputContext(const Mod& a_mod, const nlohmann::json& a_setting)
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
					bound.push_back({ setting.modId, setting.key,
						mod ? Json::GetString(mod->schema, "title", mod->id) : setting.modId, vk, blocksGameplay });
				}
			}
			// The game's own bindings (mcm-design.md §9 "vanilla hotkeys"):
			// pseudo-entries under the reserved id "@game". Gated on the same
			// resolver as mod settings — without one there is no conflict
			// grouping at all, so vanilla data would never be consulted.
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

	nlohmann::json SettingsStore::Data() const
	{
		// Informational key-conflict grouping (mcm-design.md §9): resolve
		// every key-typed setting's current value ONCE, so the annotation walk
		// below is a lookup, not a re-resolve (an unresolvable name would
		// otherwise warn once per setting per pass). Never blocking — the
		// renderer badges both sides of a collision; the bind stands.
		const std::vector<BoundKey> bound = ResolveBoundKeys();

		nlohmann::json mods = nlohmann::json::array();
		for (const auto& mod : _mods) {
			nlohmann::json schema = mod.schema;
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
				{ "title", Json::GetString(mod.schema, "title", mod.id) },
				{ "schema", std::move(schema) },
				{ "values", mod.values },
			};
			// Additive (api-freeze-plan item 1): drop-in files that also claimed
			// this id and lost first-wins — the Mods surface renders a conflict
			// badge. Omitted in the (normal) no-conflict case.
			if (!mod.shadowed.empty()) {
				entry["shadowed"] = mod.shadowed;
			}
			// Additive (item 2): requires-gated stub — the Mods surface renders
			// a "needs a newer OSF UI" card instead of controls.
			if (mod.stub) {
				entry["stub"] = true;
				entry["missingRequires"] = mod.missingRequires;
			}
			mods.push_back(std::move(entry));
		}
		nlohmann::json data{ { "mods", std::move(mods) } };
		// The game's own bindings as a top-level table (mcm-design.md §9
		// "vanilla hotkeys" + the keybinds view): the FULL curated map, not
		// just colliding entries — a bindings overview needs both. Additive
		// field (protocol 1.0); omitted when the feature is off/empty, and
		// views ignore unknown top-level fields.
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
		return data;
	}

	std::string SettingsStore::DataJson() const
	{
		return Data().dump();
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
			// Multi-select over `options` (item 2): value = array of option
			// strings. Resolve, don't reject: non-string elements, unknown
			// options (e.g. removed upstream), and duplicates are filtered
			// out — the enum-removal analogue of numeric clamping. Order
			// follows the DECLARED options so the stored form is canonical.
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
				// A colour-widget string must be a parseable #rrggbb[aa] — any
				// writer (preset, ABI, Papyrus), not just the UI, is held to it.
				if (Json::GetString(a_setting, "widget", "") == "color" && !IsHexColor(s)) {
					return std::nullopt;
				}
				// Per-setting maxLength, capped by the store-wide hard limit
				// (raising kMaxStringLen is the native slice; bump the renderer
				// + mock clamps in lockstep).
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
			// A rebindable key: a short key NAME string (e.g. "F10"). The store
			// stays feature-agnostic — it only bounds the length; whether the name
			// actually resolves to a VK is enforced by the consumer
			// (Runtime::OnSettingChanged via ResolveKeyName). Empty is rejected so
			// a blank never clobbers a working binding — UNLESS the schema opts in
			// with "allowUnbound": true, where "" is the deliberate unbound state
			// (skipped by HotkeyService/conflicts; the UI renders an unbind ✕).
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
		auto* mod = FindMod(a_modId);
		if (!mod) {
			REX::WARN("SettingsStore: rejected set for unknown mod '{}'", a_modId.substr(0, 64));
			return { false, "unknown-setting" };
		}
		if (mod->stub) {
			REX::WARN("SettingsStore: rejected set for '{}' — registered as a requires-gated stub", mod->id);
			return { false, "read-only" };
		}
		const auto* setting = FindSetting(*mod, a_key);
		if (!setting) {
			REX::WARN("SettingsStore: rejected unknown setting '{}.{}'", a_modId.substr(0, 64), a_key.substr(0, 64));
			return { false, "unknown-setting" };
		}
		// A type this host doesn't know serves its default read-only (item 2)
		// — surfaced as its own code so a view can say "needs a newer OSF UI"
		// instead of "bad value".
		if (!Caps::Has("type:" + Json::GetString(*setting, "type", ""))) {
			REX::WARN("SettingsStore: rejected set for '{}.{}' — unknown type '{}' is served read-only",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::GetString(*setting, "type", "?").substr(0, 32));
			return { false, "read-only" };
		}
		const auto parsed = Json::Parse(a_valueJson, "settings value");
		if (!parsed) {
			return { false, "invalid-value" };
		}
		auto valid = Validate(*setting, *parsed);
		if (!valid) {
			REX::WARN("SettingsStore: rejected invalid value for '{}.{}' (type {})",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::GetString(*setting, "type", "?"));
			return { false, "invalid-value" };
		}

		const std::string key{ a_key };
		mod->values[key] = std::move(*valid);
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
		if (!mod || mod->stub) {
			return false;  // a stub serves nothing and must not touch the values file
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
		// Sparse persistence (mcm-design.md §8.1): only a value ≠ the schema
		// default is the user's; whatever equals the default keeps tracking
		// upstream default changes, and reset-to-default = key removal. Legacy
		// full files shed their frozen defaults the first time they pass
		// through here.
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
		// Forward-compat opaques (item 2) ride every rewrite verbatim — a
		// newer mod's settings survive this host instead of being wiped.
		// (Unknown-typed declared keys never enter the loop above: their
		// served value IS the default, so there is no collision here.)
		for (const auto& [key, value] : a_mod.preserved.items()) {
			sparse[key] = value;
		}
		// Stamp the schema version (mcm-design.md §11) ONLY when a mod actually
		// uses versioning: a v0 (unversioned) mod's file stays byte-for-byte
		// as before, so no existing values file is dirtied just by this
		// feature landing. The schema's `version` is the sole source of truth
		// — the stamp records "last written under vN"; a schema reverting to
		// v0 drops the stamp (a genuine downgrade), which the load-time
		// compare treats as any other version move.
		if (const auto v = Json::GetInt(a_mod.schema, "version", 0); v != 0) {
			sparse[kSchemaVersionKey] = v;
		}
		// Format stamp (item 8): every rewrite carries the encoding version —
		// a_mod.formatVersion is max(known, loaded), so a newer host's stamp
		// survives this host's rewrites. The load-time compare tolerates a
		// missing stamp, so this alone never dirties an existing file.
		sparse[kFormatVersionKey] = a_mod.formatVersion;
		return sparse;
	}

	void SettingsStore::MarkDirty(Mod& a_mod)
	{
		if (!a_mod.dirty) {
			a_mod.dirty = true;
			// The window opens at the FIRST unflushed change and is not pushed
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
