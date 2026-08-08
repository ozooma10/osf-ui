#include "runtime/ViewPolicyStore.h"

#include "runtime/Ids.h"
#include "runtime/Json.h"

#include <fstream>

namespace OSFUI
{
	void ViewPolicyStore::Load(std::filesystem::path a_path)
	{
		_path = std::move(a_path);
		_hudOverrides.clear();

		std::error_code ec;
		if (!std::filesystem::exists(_path, ec)) {
			return;
		}
		const auto json = Json::ParseFile(_path);
		if (!json || !json->is_object()) {
			// Same posture as corrupt settings values: quarantine, serve
			// defaults, never guess at what the player meant.
			auto quarantine = _path;
			quarantine += ".bad";
			std::filesystem::remove(quarantine, ec);
			std::filesystem::rename(_path, quarantine, ec);
			REX::ERROR("ViewPolicyStore: {} is not a valid policy file — {}; using defaults",
				_path.string(),
				ec ? "quarantine rename failed, file left in place" :
				     "quarantined to " + quarantine.string());
			return;
		}
		Json::CheckFormatVersion(*json, "formatVersion", kFormatVersion,
			"ViewPolicyStore: " + _path.string());
		const auto* overrides = Json::GetObject(*json, "hudOverrides");
		if (!overrides) {
			return;
		}
		for (const auto& [id, value] : overrides->items()) {
			if (!Ids::IsValidQualifiedViewId(id) || !value.is_boolean()) {
				REX::WARN("ViewPolicyStore: {}: skipping hudOverrides entry '{}' — "
						  "keys are qualified '<modId>/<viewName>' ids and values are booleans",
					_path.string(), id);
				continue;
			}
			_hudOverrides.emplace(id, value.get<bool>());
		}
	}

	bool ViewPolicyStore::HudAutoStart(std::string_view a_viewId, bool a_manifestDefault) const
	{
		const auto it = _hudOverrides.find(std::string(a_viewId));
		return it != _hudOverrides.end() ? it->second : a_manifestDefault;
	}

	bool ViewPolicyStore::HasHudOverride(std::string_view a_viewId) const
	{
		return _hudOverrides.contains(std::string(a_viewId));
	}

	bool ViewPolicyStore::SetHudAutoStart(std::string_view a_viewId, bool a_enabled)
	{
		auto key = std::string(a_viewId);
		const auto previous = _hudOverrides.find(key);
		const bool hadPrevious = previous != _hudOverrides.end();
		const bool previousValue = hadPrevious && previous->second;

		_hudOverrides[std::move(key)] = a_enabled;
		if (Persist()) {
			return true;
		}
		// Roll back so the UI cannot report a choice the next launch won't see.
		if (hadPrevious) {
			_hudOverrides[std::string(a_viewId)] = previousValue;
		} else {
			_hudOverrides.erase(std::string(a_viewId));
		}
		return false;
	}

	bool ViewPolicyStore::Persist() const
	{
		std::error_code ec;
		std::filesystem::create_directories(_path.parent_path(), ec);

		// nlohmann objects iterate key-sorted, so the file is deterministic.
		nlohmann::json overrides = nlohmann::json::object();
		for (const auto& [id, enabled] : _hudOverrides) {
			overrides[id] = enabled;
		}
		const nlohmann::json doc{
			{ "formatVersion", kFormatVersion },
			{ "hudOverrides", std::move(overrides) },
		};

		// Temp file + rename so a crash mid-write can't corrupt the policy.
		const auto tmp = std::filesystem::path(_path).concat(".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out) {
				REX::ERROR("ViewPolicyStore: cannot write {}", tmp.string());
				return false;
			}
			out << Json::Dump(doc, 2);
			out.close();  // flush now so a disk-full / IO error surfaces before the rename
			if (!out) {
				REX::ERROR("ViewPolicyStore: write to {} failed (disk full/IO?); keeping previous policy", tmp.string());
				std::filesystem::remove(tmp, ec);
				return false;
			}
		}
		std::filesystem::rename(tmp, _path, ec);
		if (ec) {
			REX::ERROR("ViewPolicyStore: cannot replace {} ({})", _path.string(), ec.message());
			std::filesystem::remove(tmp, ec);
			return false;
		}
		return true;
	}
}
