#include "runtime/ViewLifecycle.h"

#include <algorithm>

namespace OSFUI
{
	void ViewLifecycle::NoteInstantiated(std::string_view a_id, bool a_pinned, double a_now)
	{
		_entries[std::string(a_id)] = Entry{
			.pinned = a_pinned,
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

	void ViewLifecycle::NoteOpenState(std::string_view a_id, bool a_open, double a_now)
	{
		const auto it = _entries.find(std::string(a_id));
		if (it == _entries.end() || it->second.open == a_open) {
			return;
		}
		auto& entry = it->second;
		entry.open = a_open;
		if (!a_open && !entry.visible) {
			// Closed while already hidden: the close is a fresh player decision,
			// so reclamation eligibility starts a new episode rather than
			// inheriting the pre-close idle time.
			entry.hiddenSince = a_now;
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

	void ViewLifecycle::OnBrowserHostRestart(double a_now)
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
		// Views counting against the hidden cap, oldest-first after the sort;
		// the (hiddenSince, id) key makes equal timestamps deterministic.
		std::vector<std::pair<double, std::string_view>> reclaimable;
		for (const auto& [id, entry] : _entries) {
			if (entry.visible) {
				continue;
			}
			const auto hiddenFor = (std::max)(0.0, a_now - entry.hiddenSince);
			if (!entry.pinned && !entry.open) {
				if (hiddenFor >= kDestroyAfterHiddenSeconds) {
					actions.destroy.push_back(id);
					continue;
				}
				reclaimable.emplace_back(entry.hiddenSince, id);
			}
			if (!entry.suspendRequested && hiddenFor >= kSuspendAfterHiddenSeconds) {
				actions.suspend.push_back(id);
			}
		}
		if (reclaimable.size() > kMaxHiddenReclaimable) {
			std::ranges::sort(reclaimable);
			const auto excess = reclaimable.size() - kMaxHiddenReclaimable;
			for (std::size_t i = 0; i < excess; ++i) {
				actions.destroy.emplace_back(reclaimable[i].second);
			}
			// A view being reclaimed no longer needs the suspend it may also be
			// due for this tick.
			std::erase_if(actions.suspend, [&](const std::string& a_id) {
				return std::ranges::find(actions.destroy, a_id) != actions.destroy.end();
			});
		}
		return actions;
	}
}
