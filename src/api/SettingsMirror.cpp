#include "api/SettingsMirror.h"

#include <cstring>  // memcpy — not in the pch umbrella
#include <limits>   // numeric_limits — not in the pch umbrella

#include "runtime/Json.h"

namespace OSFUI::API
{
	void SettingsMirror::Update(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		std::lock_guard lock(_mutex);
		_mods[std::string(a_modId)][std::string(a_key)] = a_value;
	}

	void SettingsMirror::Rebuild(const nlohmann::json& a_data)
	{
		// Build outside the lock so getters block only for the swap, not the parse.
		std::unordered_map<std::string, Values> fresh;
		if (const auto* mods = Json::GetArray(a_data, "mods")) {
			for (const auto& mod : *mods) {
				const auto  id = Json::Get(mod, "id", "");
				const auto* values = Json::GetObject(mod, "values");
				if (id.empty() || !values) {
					continue;  // skip a malformed entry rather than throw
				}
				Values& slot = fresh[id];
				for (const auto& [key, value] : values->items()) {
					slot.insert_or_assign(key, value);
				}
			}
		}
		std::lock_guard lock(_mutex);
		_mods.swap(fresh);
	}

	bool SettingsMirror::GetBool(const char* a_modId, const char* a_key, bool* a_out) const
	{
		std::lock_guard lock(_mutex);
		const auto* value = Find(a_modId, a_key);
		if (!value || !value->is_boolean() || !a_out) {
			return false;
		}
		*a_out = value->get<bool>();
		return true;
	}

	bool SettingsMirror::GetInt(const char* a_modId, const char* a_key, std::int64_t* a_out) const
	{
		std::lock_guard lock(_mutex);
		const auto* value = Find(a_modId, a_key);
		if (!value || !value->is_number_integer() || !a_out) {
			return false;
		}
		// A uint64 value above int64 max can't be represented in the out param.
		if (value->is_number_unsigned() &&
			value->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
			return false;
		}
		*a_out = value->get<std::int64_t>();
		return true;
	}

	bool SettingsMirror::GetFloat(const char* a_modId, const char* a_key, double* a_out) const
	{
		std::lock_guard lock(_mutex);
		const auto* value = Find(a_modId, a_key);
		// Any number: a float-typed setting can hold integral JSON (the user
		// typed "1"), and the mirror has no schema to say otherwise.
		if (!value || !value->is_number() || !a_out) {
			return false;
		}
		*a_out = value->get<double>();
		return true;
	}

	std::uint32_t SettingsMirror::GetString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) const
	{
		std::lock_guard lock(_mutex);
		const auto* value = Find(a_modId, a_key);
		if (!value || !value->is_string()) {
			return 0;
		}
		const auto& str = value->get_ref<const std::string&>();
		// Contract (OSFUI_API.h): return required length incl. NUL; copy at most
		// a_bufLen, always NUL-terminated. A null/empty buffer is the size probe.
		if (a_buf && a_bufLen > 0) {
			const auto copied = std::min<std::size_t>(a_bufLen - 1, str.size());
			std::memcpy(a_buf, str.data(), copied);
			a_buf[copied] = '\0';
		}
		// Store caps strings at 4096, so this can't overflow in practice; the
		// mirror doesn't rely on that.
		const std::uint64_t required = static_cast<std::uint64_t>(str.size()) + 1;
		return required > std::numeric_limits<std::uint32_t>::max()
		           ? std::numeric_limits<std::uint32_t>::max()
		           : static_cast<std::uint32_t>(required);
	}

	std::vector<std::pair<std::string, std::string>> SettingsMirror::SnapshotMod(std::string_view a_modId) const
	{
		std::vector<std::pair<std::string, std::string>> out;
		std::lock_guard lock(_mutex);
		const auto mod = _mods.find(std::string(a_modId));
		if (mod == _mods.end()) {
			return out;
		}
		out.reserve(mod->second.size());
		for (const auto& [key, value] : mod->second) {
			out.emplace_back(key, Json::Dump(value));
		}
		return out;
	}

	bool SettingsMirror::ResolveNames(std::string_view a_modId, std::string_view a_key, std::string& a_outMod, std::string& a_outKey) const
	{
		std::lock_guard lock(_mutex);
		const auto* mod = LookupMod(a_modId);
		if (!mod) {
			return false;
		}
		a_outMod = mod->first;
		// Empty key resolves the mod only (whole-mod Reset); do not look up a key.
		if (a_key.empty()) {
			a_outKey.clear();
			return true;
		}
		const auto* entry = LookupKey(mod->second, a_key);
		if (!entry) {
			return false;
		}
		a_outKey = entry->first;
		return true;
	}

	const nlohmann::json* SettingsMirror::Find(const char* a_modId, const char* a_key) const
	{
		// LOAD-BEARING null check: these are raw C-ABI pointers from third-party
		// plugin code, unlike BSFixedString::c_str(), which can never be null
		// (it falls back to an interned EMPTY). Check before any string_view is
		// built from them.
		if (!a_modId || !a_key) {
			return nullptr;
		}
		const auto* mod = LookupMod(a_modId);
		if (!mod) {
			return nullptr;
		}
		const auto* entry = LookupKey(mod->second, a_key);
		return entry ? &entry->second : nullptr;
	}

	const std::pair<const std::string, SettingsMirror::Values>* SettingsMirror::LookupMod(std::string_view a_modId) const
	{
		if (const auto it = _mods.find(std::string(a_modId)); it != _mods.end()) {
			return &*it;
		}
		// Case-insensitive fallback: Papyrus cannot control BSFixedString casing
		// (see header).
		for (const auto& entry : _mods) {
			if (Ids::EqualsCaseInsensitiveAscii(entry.first, a_modId)) {
				return &entry;
			}
		}
		return nullptr;
	}

	const std::pair<const std::string, nlohmann::json>* SettingsMirror::LookupKey(const Values& a_values, std::string_view a_key)
	{
		if (const auto it = a_values.find(std::string(a_key)); it != a_values.end()) {
			return &*it;
		}
		for (const auto& entry : a_values) {
			if (Ids::EqualsCaseInsensitiveAscii(entry.first, a_key)) {
				return &entry;
			}
		}
		return nullptr;
	}
}
