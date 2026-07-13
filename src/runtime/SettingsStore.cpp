#include "runtime/SettingsStore.h"

#include <cmath>

#include "core/Log.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::size_t kMaxStringLen = 256;
		constexpr std::size_t kMaxModIdLen = 64;

		// A mod id becomes a filename (<valuesDir>/<id>.json) and a web asset
		// path segment (views/<id>/...): restrict it to a safe charset and
		// reject traversal so a schema-supplied id can never escape either
		// confinement (docs/security-model.md).
		bool IsValidModId(std::string_view a_id)
		{
			if (a_id.empty() || a_id.size() > kMaxModIdLen) {
				return false;
			}
			if (a_id.front() == '.' || a_id.find("..") != std::string_view::npos) {
				return false;
			}
			for (const char c : a_id) {
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
				if (!ok) {
					return false;
				}
			}
			return true;
		}

		// Framework bridge namespaces. The renderer only fires an action whose
		// command is prefixed "<modId>.", so a mod id equal to one of these
		// would make "namespaced" commands collide with framework commands
		// (menu.close, settings.reset, ...). The framework's own id ("osfui")
		// is not listed: its schema loads through the same drop-in path.
		bool IsReservedModId(std::string_view a_id)
		{
			for (const std::string_view r : { "ui", "menu", "hud", "settings", "views", "game", "runtime" }) {
				if (a_id == r) {
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
		// return true to stop early.
		template <class Fn>
		void ForEachSetting(const nlohmann::json& a_schema, Fn&& a_fn)
		{
			const auto groups = a_schema.find("groups");
			if (groups == a_schema.end() || !groups->is_array()) {
				return;
			}
			for (const auto& group : *groups) {
				const auto settings = group.find("settings");
				if (settings == group.end() || !settings->is_array()) {
					continue;
				}
				for (const auto& setting : *settings) {
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
			auto schema = Json::ParseFile(path);
			if (!schema || !schema->is_object()) {
				REX::WARN("SettingsStore: skipping invalid schema {}", path.string());
				continue;
			}
			// Startup load: notifications defer to the OnStart NotifyAll.
			AddSchema(std::move(*schema), Source::kDropIn, path.stem().string(), /*a_notify=*/false);
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
		if (!IsValidModId(id)) {
			REX::WARN("SettingsStore: rejected schema id '{}' — ids are 1-{} chars of [A-Za-z0-9._-] "
					  "with no leading '.' and no '..' (the id names the values file and asset folder)",
				id.substr(0, kMaxModIdLen), kMaxModIdLen);
			return false;
		}
		if (IsReservedModId(id)) {
			REX::WARN("SettingsStore: rejected schema id '{}' — reserved framework namespace", id);
			return false;
		}
		return true;
	}

	bool SettingsStore::AddSchema(nlohmann::json a_schema, Source a_source, std::string a_idHint, bool a_notify)
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

		// Source precedence on id collision (mcm-design.md §14.1): a runtime
		// (DLL) registration replaces a drop-in file — a mod upgrading tiers
		// must not require users to hand-delete the stale JSON — and replaces
		// its own earlier registration (dev iteration). A drop-in never
		// replaces anything: duplicate drop-in ids resolve first-wins (MO2's
		// per-file VFS priority is the intended arbiter, §8.1).
		Mod* existing = FindMod(id);
		if (existing) {
			if (a_source == Source::kDropIn) {
				REX::WARN("SettingsStore: duplicate schema id '{}' — keeping the {} one, ignoring the drop-in file",
					id, existing->source == Source::kNative ? "runtime-registered" : "first-loaded");
				return false;
			}
			REX::WARN("SettingsStore: runtime registration replaces {} schema for id '{}'",
				existing->source == Source::kDropIn ? "drop-in" : "earlier runtime", id);
			if (existing->dirty) {
				// The overlay below reads the values FILE; land any unflushed
				// write-behind changes first or the replacement reverts them.
				PersistNow(*existing);
			}
		}

		Mod mod;
		mod.id = std::move(id);
		mod.schema = std::move(a_schema);
		mod.valuesPath = _valuesDir / (mod.id + ".json");
		mod.source = a_source;
		mod.values = nlohmann::json::object();

		// Persisted values over schema defaults. Replacement rebuilds from the
		// same file — every committed Set persists, so nothing is lost, and
		// added/removed/retyped keys resolve exactly like a fresh load.
		nlohmann::json saved = nlohmann::json::object();
		if (auto parsed = Json::ParseFile(mod.valuesPath); parsed && parsed->is_object()) {
			saved = std::move(*parsed);
		}
		std::size_t count = 0;
		ForEachSetting(mod.schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::GetString(a_setting, "key", "");
			if (key.empty()) {
				return false;
			}
			++count;
			if (const auto it = saved.find(key); it != saved.end()) {
				if (auto valid = Validate(a_setting, *it)) {
					mod.values[key] = std::move(*valid);
					return false;
				}
			}
			mod.values[key] = DefaultFor(a_setting);
			return false;
		});

		// Prune-to-default on load (mcm-design.md §8.1): when the file differs
		// from its sparse form — a legacy full file, a saved value that now
		// equals an updated default, junk/retyped keys, clamping — schedule a
		// rewrite so those knobs track upstream defaults again instead of
		// staying frozen forever.
		if (saved != SparseValues(mod)) {
			MarkDirty(mod);
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
		if (!mod) {
			return {};
		}
		const auto* setting = FindSetting(*mod, a_key);
		return setting ? Json::GetString(*setting, "type", "") : std::string{};
	}

	std::optional<SettingsStore::Source> SettingsStore::GetSource(std::string_view a_modId) const
	{
		const auto* mod = FindMod(a_modId);
		return mod ? std::optional(mod->source) : std::nullopt;
	}

	nlohmann::json SettingsStore::Data() const
	{
		nlohmann::json mods = nlohmann::json::array();
		for (const auto& mod : _mods) {
			mods.push_back({
				{ "id", mod.id },
				{ "title", Json::GetString(mod.schema, "title", mod.id) },
				{ "schema", mod.schema },
				{ "values", mod.values },
			});
		}
		return nlohmann::json{ { "mods", std::move(mods) } };
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
			// a blank never clobbers a working binding.
			if (a_value.is_string()) {
				auto s = a_value.get<std::string>();
				if (!s.empty()) {
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
		auto* mod = FindMod(a_modId);
		if (!mod) {
			REX::WARN("SettingsStore: rejected set for unknown mod '{}'", a_modId.substr(0, 64));
			return false;
		}
		const auto* setting = FindSetting(*mod, a_key);
		if (!setting) {
			REX::WARN("SettingsStore: rejected unknown setting '{}.{}'", a_modId.substr(0, 64), a_key.substr(0, 64));
			return false;
		}
		const auto parsed = Json::Parse(a_valueJson, "settings value");
		if (!parsed) {
			return false;
		}
		auto valid = Validate(*setting, *parsed);
		if (!valid) {
			REX::WARN("SettingsStore: rejected invalid value for '{}.{}' (type {})",
				a_modId.substr(0, 64), a_key.substr(0, 64), Json::GetString(*setting, "type", "?"));
			return false;
		}

		const std::string key{ a_key };
		mod->values[key] = std::move(*valid);
		MarkDirty(*mod);  // notification immediate; disk write coalesced (PumpPersistence)
		Notify(mod->id, key, mod->values[key]);
		if (Log::DevMode()) {
			REX::DEBUG("SettingsStore: set '{}.{}' = {}", mod->id, key, mod->values[key].dump().substr(0, 128));
		}
		return true;
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
