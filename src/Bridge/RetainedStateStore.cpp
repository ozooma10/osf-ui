#include "Bridge/RetainedStateStore.h"

#include "Core/StringUtil.h"

namespace OSFUI
{
	bool RetainedStateStore::Set(std::string_view a_mod, std::string_view a_key, nlohmann::json a_value,
		bool a_sessionScoped)
	{
		if (a_mod.empty() || a_key.empty()) {
			return false;
		}
		const auto folded = StringUtil::ToLowerAscii(a_mod);
		auto       it = _mods.find(folded);
		if (it == _mods.end()) {
			// Check capacity before creating a caller-supplied mod bucket.
			if (_mods.size() >= kMaxMods) {
				REX::WARN("RetainedStateStore: holding state for the maximum {} mods — "
						  "'{}.{}' is delivered but not retained",
					kMaxMods, a_mod, a_key);
				return false;
			}
			it = _mods.emplace(folded, std::vector<Entry>{}).first;
		}
		auto&      entries = it->second;
		const auto wanted = StringUtil::ToLowerAscii(a_key);
		for (auto& entry : entries) {
			if (StringUtil::EqualsCaseInsensitiveAscii(entry.key, wanted)) {
				// Latest value and spelling win.
				entry.key = std::string(a_key);
				entry.value = std::move(a_value);
				entry.sessionScoped = a_sessionScoped;
				return true;
			}
		}
		if (entries.size() >= kMaxKeysPerMod) {
			REX::WARN("RetainedStateStore: '{}' holds the maximum {} retained keys — '{}' is delivered but not retained",
				a_mod, kMaxKeysPerMod, a_key);
			return false;
		}
		entries.push_back(Entry{ .key = std::string(a_key), .value = std::move(a_value),
			.sessionScoped = a_sessionScoped });
		return true;
	}

	const std::vector<RetainedStateStore::Entry>* RetainedStateStore::Find(std::string_view a_mod) const
	{
		const auto it = _mods.find(StringUtil::ToLowerAscii(a_mod));
		return it == _mods.end() ? nullptr : &it->second;
	}

	void RetainedStateStore::RemoveMod(std::string_view a_mod)
	{
		_mods.erase(StringUtil::ToLowerAscii(a_mod));
	}

	void RetainedStateStore::ClearSessionScoped()
	{
		for (auto it = _mods.begin(); it != _mods.end();) {
			auto& entries = it->second;
			std::erase_if(entries, [](const Entry& a_entry) { return a_entry.sessionScoped; });
			it = entries.empty() ? _mods.erase(it) : std::next(it);
		}
	}

	void RetainedStateStore::Clear()
	{
		_mods.clear();
	}
}
