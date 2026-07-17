#include "api/SettingsMirror.h"

#include <cstring>  // memcpy — not in the pch umbrella
#include <limits>   // numeric_limits — not in the pch umbrella

namespace OSFUI::API
{
	void SettingsMirror::Update(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		std::lock_guard lock(_mutex);
		_mods[std::string(a_modId)][std::string(a_key)] = a_value;
	}

	void SettingsMirror::Rebuild(const nlohmann::json& a_data)
	{
		// Build the replacement outside the lock so getters are only ever
		// blocked for the swap, not the parse.
		std::unordered_map<std::string, Values> fresh;
		if (const auto mods = a_data.find("mods"); mods != a_data.end() && mods->is_array()) {
			for (const auto& mod : *mods) {
				const auto id = mod.find("id");
				const auto values = mod.find("values");
				if (id == mod.end() || !id->is_string() || values == mod.end() || !values->is_object()) {
					continue;  // defensive: skip a malformed entry, never throw
				}
				Values& slot = fresh[id->get<std::string>()];
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
		// Any number: a float-typed setting legitimately holds integral JSON
		// (the user typed "1"), and the mirror has no schema to say otherwise.
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
		// Contract (OSFUI_API.h): return required length incl. NUL; copy
		// min(a_bufLen), always NUL-terminated. A null/empty buffer is the
		// "how big?" probe.
		if (a_buf && a_bufLen > 0) {
			const auto copied = std::min<std::size_t>(a_bufLen - 1, str.size());
			std::memcpy(a_buf, str.data(), copied);
			a_buf[copied] = '\0';
		}
		// Store caps strings at 4096 so this never overflows in practice, but
		// the mirror shouldn't be the component that assumes that.
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
			out.emplace_back(key, value.dump());
		}
		return out;
	}

	bool SettingsMirror::ResolveNames(std::string_view a_modId, std::string_view a_key, std::string& a_outMod, std::string& a_outKey) const
	{
		std::lock_guard lock(_mutex);
		const Values* values = nullptr;
		if (const auto mod = _mods.find(std::string(a_modId)); mod != _mods.end()) {
			a_outMod = mod->first;
			values = &mod->second;
		} else {
			for (const auto& [id, v] : _mods) {
				if (Ids::EqualsCaseInsensitiveAscii(id, a_modId)) {
					a_outMod = id;
					values = &v;
					break;
				}
			}
		}
		if (!values) {
			return false;
		}
		if (a_key.empty()) {
			a_outKey.clear();
			return true;
		}
		if (const auto it = values->find(std::string(a_key)); it != values->end()) {
			a_outKey = it->first;
			return true;
		}
		for (const auto& [key, v] : *values) {
			if (Ids::EqualsCaseInsensitiveAscii(key, a_key)) {
				a_outKey = key;
				return true;
			}
		}
		return false;
	}

	const nlohmann::json* SettingsMirror::Find(const char* a_modId, const char* a_key) const
	{
		if (!a_modId || !a_key) {
			return nullptr;
		}
		const Values* values = nullptr;
		if (const auto mod = _mods.find(a_modId); mod != _mods.end()) {
			values = &mod->second;
		} else {
			// Case-insensitive fallback — see the header's BSFixedString
			// interning rationale (Papyrus cannot control its casing).
			for (const auto& [id, v] : _mods) {
				if (Ids::EqualsCaseInsensitiveAscii(id, a_modId)) {
					values = &v;
					break;
				}
			}
		}
		if (!values) {
			return nullptr;
		}
		if (const auto value = values->find(a_key); value != values->end()) {
			return &value->second;
		}
		for (const auto& [key, value] : *values) {
			if (Ids::EqualsCaseInsensitiveAscii(key, a_key)) {
				return &value;
			}
		}
		return nullptr;
	}
}
