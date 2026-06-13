#include "runtime/SettingsStore.h"

#include <cmath>

#include "core/Log.h"
#include "runtime/Json.h"

namespace SWUI
{
	namespace
	{
		constexpr std::size_t kMaxStringLen = 256;

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
					if (setting.is_object() && std::forward<Fn>(a_fn)(setting)) {
						return;
					}
				}
			}
		}
	}

	bool SettingsStore::Load(const std::filesystem::path& a_schemaPath, const std::filesystem::path& a_valuesPath)
	{
		_loaded = false;
		_valuesPath = a_valuesPath;
		_values = nlohmann::json::object();

		auto schema = Json::ParseFile(a_schemaPath);
		if (!schema || !schema->is_object()) {
			REX::WARN("SettingsStore: no valid schema at {} — settings UI will be empty", a_schemaPath.string());
			return false;
		}
		_schema = std::move(*schema);

		// Persisted values (may be absent on first run).
		nlohmann::json saved = nlohmann::json::object();
		if (auto parsed = Json::ParseFile(a_valuesPath); parsed && parsed->is_object()) {
			saved = std::move(*parsed);
		}

		// Current value for each setting = saved (validated) else default.
		std::size_t count = 0;
		ForEachSetting(_schema, [&](const nlohmann::json& a_setting) {
			const auto key = Json::GetString(a_setting, "key", "");
			if (key.empty()) {
				return false;
			}
			++count;
			if (const auto it = saved.find(key); it != saved.end()) {
				if (auto valid = Validate(a_setting, *it)) {
					_values[key] = std::move(*valid);
					return false;
				}
			}
			const auto def = a_setting.find("default");
			_values[key] = (def != a_setting.end()) ? *def : nlohmann::json(nullptr);
			return false;
		});

		_loaded = true;
		REX::INFO("SettingsStore: loaded schema '{}' ({} settings) from {} (values: {})",
			Json::GetString(_schema, "title", "Settings"), count, a_schemaPath.string(), a_valuesPath.string());
		return true;
	}

	std::string SettingsStore::DataJson() const
	{
		nlohmann::json data{
			{ "schema", _loaded ? _schema : nlohmann::json::object() },
			{ "values", _values },
		};
		return data.dump();
	}

	const nlohmann::json* SettingsStore::FindSetting(std::string_view a_key) const
	{
		const nlohmann::json* found = nullptr;
		ForEachSetting(_schema, [&](const nlohmann::json& a_setting) {
			if (Json::GetString(a_setting, "key", "") == a_key) {
				found = &a_setting;
				return true;
			}
			return false;
		});
		return found;
	}

	std::optional<nlohmann::json> SettingsStore::Validate(const nlohmann::json& a_setting, const nlohmann::json& a_value) const
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
				if (s.size() > kMaxStringLen) {
					s.resize(kMaxStringLen);
				}
				return nlohmann::json(std::move(s));
			}
		}
		return std::nullopt;
	}

	bool SettingsStore::Set(std::string_view a_key, std::string_view a_valueJson)
	{
		if (!_loaded) {
			return false;
		}
		const auto* setting = FindSetting(a_key);
		if (!setting) {
			REX::WARN("SettingsStore: rejected unknown setting key '{}'", a_key.substr(0, 64));
			return false;
		}
		const auto parsed = Json::Parse(a_valueJson, "settings value");
		if (!parsed) {
			return false;
		}
		auto valid = Validate(*setting, *parsed);
		if (!valid) {
			REX::WARN("SettingsStore: rejected invalid value for '{}' (type {})",
				a_key.substr(0, 64), Json::GetString(*setting, "type", "?"));
			return false;
		}

		_values[std::string(a_key)] = std::move(*valid);
		const bool ok = Persist();
		if (ok && Log::DevMode()) {
			REX::DEBUG("SettingsStore: set '{}' = {}", a_key, _values[std::string(a_key)].dump().substr(0, 128));
		}
		return ok;
	}

	bool SettingsStore::Persist() const
	{
		std::error_code ec;
		std::filesystem::create_directories(_valuesPath.parent_path(), ec);

		// Write to a temp file then rename, so a crash mid-write can't corrupt
		// the existing values.
		const auto tmp = std::filesystem::path(_valuesPath).concat(".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out) {
				REX::ERROR("SettingsStore: cannot write {}", tmp.string());
				return false;
			}
			out << _values.dump(2);
		}
		std::filesystem::rename(tmp, _valuesPath, ec);
		if (ec) {
			REX::ERROR("SettingsStore: cannot replace {} ({})", _valuesPath.string(), ec.message());
			std::filesystem::remove(tmp, ec);
			return false;
		}
		return true;
	}
}
