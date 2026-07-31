#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
	// Main-thread-only policy for loaded overlay views. Runtime owns whether a
	// view is live/visible; the WebView2 host owns the asynchronous mechanics of
	// satisfying a suspend request.
	class ViewLifecycle
	{
	public:
		static constexpr double kSuspendAfterHiddenSeconds = 90.0;
		static constexpr double kDestroyAfterHiddenSeconds = 1500.0;
		// Besides the idle TTL, at most this many non-warm closed views may sit
		// hidden at once; the least-recently-hidden extras are reclaimed early so
		// a session that visits many menus doesn't accumulate their pages.
		static constexpr std::size_t kMaxHiddenReclaimable = 4;

		struct DueActions
		{
			std::vector<std::string> suspend;
			std::vector<std::string> destroy;
		};

		// A newly created renderer view starts hidden. ApplyMenuPolicy follows with
		// the authoritative visibility and may immediately change it to visible.
		void NoteLoaded(std::string_view a_id, bool a_warm, double a_now);
		void NoteVisibility(std::string_view a_id, bool a_visible, double a_now);
		// Open surfaces (menus in the open stack, shown HUDs) are exempt from
		// reclamation even while policy hides them beneath other menus. Closing
		// while hidden starts a fresh grace period — the close is a fresh player
		// decision, not the tail of the old idle episode.
		void NoteOpenState(std::string_view a_id, bool a_open, double a_now);
		// A fresh document invalidates any host-side suspend latch. Hidden reloads
		// therefore start a fresh grace period; visible reloads stay visible.
		void NoteActivity(std::string_view a_id, double a_now);
		void NoteSuspendRequested(std::string_view a_id);
		void NoteDestroyed(std::string_view a_id);
		// A replacement host has no suspend latches. Hidden recreated documents get
		// a fresh grace period before the game asks the replacement to suspend them.
		void OnHostRestart(double a_now);

		[[nodiscard]] DueActions CollectDueActions(double a_now) const;

	private:
		struct Entry
		{
			bool   warm{ false };
			bool   visible{ false };
			bool   open{ false };
			bool   suspendRequested{ false };
			double hiddenSince{ 0.0 };
		};

		std::unordered_map<std::string, Entry> _entries;
	};
}
