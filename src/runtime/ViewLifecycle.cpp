#include "runtime/ViewLifecycle.h"

#include <algorithm>

namespace OSFUI
{
	void ViewLifecycle::NoteLoaded(std::string_view a_id, bool a_warm, double a_now)
	{
		_entries[std::string(a_id)] = Entry{
			.warm = a_warm,
			.visible = false,
			.suspendRequested = false,
			.hiddenSince = a_now,
		};
	}

	void ViewLifecycle::NoteVisibility(std::string_view a_id, bool a_visible, double a_now)
	{
		const auto it = _entries.find(std::string(a_id));
		if (it == _entries.end() || it->second.visible == a_visible) {
			return;
		}
		auto& entry = it->second;
		entry.visible = a_visible;
		entry.suspendRequested = false;
		if (!a_visible) {
			entry.hiddenSince = a_now;
		}
	}

	void ViewLifecycle::NoteActivity(std::string_view a_id, double a_now)
	{
		const auto it = _entries.find(std::string(a_id));
		if (it == _entries.end()) {
			return;
		}
		it->second.suspendRequested = false;
		if (!it->second.visible) {
			it->second.hiddenSince = a_now;
		}
	}

	void ViewLifecycle::NoteSuspendRequested(std::string_view a_id)
	{
		if (const auto it = _entries.find(std::string(a_id)); it != _entries.end()) {
			it->second.suspendRequested = true;
		}
	}

	void ViewLifecycle::NoteDestroyed(std::string_view a_id)
	{
		_entries.erase(std::string(a_id));
	}

	void ViewLifecycle::OnHostRestart(double a_now)
	{
		for (auto& [id, entry] : _entries) {
			(void)id;
			entry.suspendRequested = false;
			if (!entry.visible) {
				entry.hiddenSince = a_now;
			}
		}
	}

	ViewLifecycle::DueActions ViewLifecycle::CollectDueActions(double a_now) const
	{
		DueActions actions;
		for (const auto& [id, entry] : _entries) {
			if (entry.visible) {
				continue;
			}
			const auto hiddenFor = (std::max)(0.0, a_now - entry.hiddenSince);
			if (!entry.warm && hiddenFor >= kDestroyAfterHiddenSeconds) {
				actions.destroy.push_back(id);
				continue;
			}
			if (!entry.suspendRequested && hiddenFor >= kSuspendAfterHiddenSeconds) {
				actions.suspend.push_back(id);
			}
		}
		return actions;
	}
}
