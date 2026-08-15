#include "Views/ViewLifecycle.h"

#include <cassert>
#include <iostream>

using OSFUI::ViewLifecycle;

namespace
{
	bool Contains(const std::vector<std::string>& a_values, std::string_view a_value)
	{
		return std::ranges::find(a_values, a_value) != a_values.end();
	}
}

int main()
{
	ViewLifecycle lifecycle;
	lifecycle.NoteInstantiated("acme.mod/panel", false, 10.0);
	assert(lifecycle.CollectDueActions(99.999).suspend.empty());
	assert(Contains(lifecycle.CollectDueActions(100.0).suspend, "acme.mod/panel"));
	lifecycle.NoteSuspendRequested("acme.mod/panel");
	assert(lifecycle.CollectDueActions(101.0).suspend.empty());

	// Repeated hidden policy applications are idempotent and do not postpone
	// either threshold.
	lifecycle.NoteVisibility("acme.mod/panel", false, 90.0);
	assert(Contains(lifecycle.CollectDueActions(1510.0).destroy, "acme.mod/panel"));

	// A real show/hide edge starts a new episode.
	lifecycle.NoteVisibility("acme.mod/panel", true, 1511.0);
	assert(lifecycle.CollectDueActions(4000.0).destroy.empty());
	lifecycle.NoteVisibility("acme.mod/panel", false, 2000.0);
	assert(lifecycle.CollectDueActions(2089.999).suspend.empty());
	assert(Contains(lifecycle.CollectDueActions(2090.0).suspend, "acme.mod/panel"));

	// A hidden reload invalidates the browser-host latch and gets a fresh grace period.
	lifecycle.NoteSuspendRequested("acme.mod/panel");
	lifecycle.NoteActivity("acme.mod/panel", 2100.0);
	assert(lifecycle.CollectDueActions(2189.999).suspend.empty());
	assert(Contains(lifecycle.CollectDueActions(2190.0).suspend, "acme.mod/panel"));

	// Pinned views suspend but are never reclaimed.
	lifecycle.NoteInstantiated("osfui/settings", true, 0.0);
	assert(Contains(lifecycle.CollectDueActions(90.0).suspend, "osfui/settings"));
	lifecycle.NoteSuspendRequested("osfui/settings");
	assert(!Contains(lifecycle.CollectDueActions(5000.0).destroy, "osfui/settings"));

	// A browser-host restart loses latches and restarts hidden grace periods.
	lifecycle.OnBrowserHostRestart(6000.0);
	assert(lifecycle.CollectDueActions(6089.999).suspend.empty());
	const auto restarted = lifecycle.CollectDueActions(6090.0);
	assert(Contains(restarted.suspend, "acme.mod/panel"));
	assert(Contains(restarted.suspend, "osfui/settings"));

	// An out-of-band show (Runtime::SetViewHidden, the `setViewHidden` bridge
	// operation) reports through NoteVisibility exactly like a policy layer, so a
	// view revealed outside the menu framework is neither suspended nor idle-
	// reclaimed while it is on screen.
	lifecycle.NoteInstantiated("acme.mod/overlay", false, 7000.0);
	lifecycle.NoteSuspendRequested("acme.mod/overlay");  // latched while hidden
	lifecycle.NoteVisibility("acme.mod/overlay", true, 7010.0);
	assert(!Contains(lifecycle.CollectDueActions(9990.0).destroy, "acme.mod/overlay"));
	assert(!Contains(lifecycle.CollectDueActions(9990.0).suspend, "acme.mod/overlay"));
	// Re-hiding it starts a fresh grace period AND a fresh suspend handshake
	// (the show cleared the stale latch the browser host had already refused).
	lifecycle.NoteVisibility("acme.mod/overlay", false, 9990.0);
	assert(!Contains(lifecycle.CollectDueActions(10079.999).suspend, "acme.mod/overlay"));
	assert(Contains(lifecycle.CollectDueActions(10080.0).suspend, "acme.mod/overlay"));

	lifecycle.NoteDestroyed("acme.mod/panel");
	assert(!Contains(lifecycle.CollectDueActions(9000.0).destroy, "acme.mod/panel"));

	// --- hidden non-core LRU cap ---------------------------------------------
	ViewLifecycle lru;
	lru.NoteInstantiated("osfui/handoff", true, 0.0);  // pinned: never counts, never reclaimed
	lru.NoteInstantiated("m.a/hud", false, 0.0);
	lru.NoteInstantiated("m.b/hud", false, 1.0);
	lru.NoteInstantiated("m.c/hud", false, 2.0);
	lru.NoteInstantiated("m.d/hud", false, 3.0);
	// Exactly at the cap: nothing reclaimed.
	assert(lru.CollectDueActions(10.0).destroy.empty());
	// A fifth hidden view evicts the least recently hidden.
	lru.NoteInstantiated("m.e/hud", false, 4.0);
	{
		const auto due = lru.CollectDueActions(10.0);
		assert(due.destroy.size() == 1 && due.destroy[0] == "m.a/hud");
	}
	// Visible views never count against the cap...
	lru.NoteVisibility("m.a/hud", true, 11.0);
	assert(lru.CollectDueActions(12.0).destroy.empty());
	// ...and re-hiding makes that view the newest episode; the next-oldest goes.
	lru.NoteVisibility("m.a/hud", false, 13.0);
	{
		const auto due = lru.CollectDueActions(14.0);
		assert(due.destroy.size() == 1 && due.destroy[0] == "m.b/hud");
	}
	// Open-but-hidden views (for example, a HUD beneath the active menu)
	// are exempt from both the cap and the idle TTL, but still suspendable.
	lru.NoteOpenState("m.b/hud", true, 14.0);
	assert(lru.CollectDueActions(15.0).destroy.empty());
	{
		const auto due = lru.CollectDueActions(5000.0);  // TTL-due for the rest
		assert(!Contains(due.destroy, "m.b/hud"));
		assert(due.destroy.size() == 4);
		assert(Contains(due.suspend, "m.b/hud"));
	}
	// Closing while hidden starts a fresh episode instead of inheriting the
	// pre-close idle time.
	lru.NoteOpenState("m.b/hud", false, 5000.0);
	assert(!Contains(lru.CollectDueActions(5001.0).destroy, "m.b/hud"));
	assert(Contains(lru.CollectDueActions(6500.0).destroy, "m.b/hud"));

	// Equal hidden timestamps resolve deterministically (lexicographic id), and
	// a view picked for reclamation is dropped from that tick's suspend list.
	ViewLifecycle tie;
	for (const auto* id : { "m.z/hud", "m.y/hud", "m.x/hud", "m.w/hud", "m.v/hud" }) {
		tie.NoteInstantiated(id, false, 0.0);
	}
	{
		const auto due = tie.CollectDueActions(95.0);  // suspend-due AND one over cap
		assert(due.destroy.size() == 1 && due.destroy[0] == "m.v/hud");
		assert(!Contains(due.suspend, "m.v/hud"));
		assert(due.suspend.size() == 4);
	}

	std::cout << "view_lifecycle_tests: ok\n";
	return 0;
}
