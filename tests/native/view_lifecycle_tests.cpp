#include "runtime/ViewLifecycle.h"

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
	lifecycle.NoteLoaded("acme.mod/panel", false, 10.0);
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

	// A hidden reload invalidates the host latch and gets a fresh grace period.
	lifecycle.NoteSuspendRequested("acme.mod/panel");
	lifecycle.NoteActivity("acme.mod/panel", 2100.0);
	assert(lifecycle.CollectDueActions(2189.999).suspend.empty());
	assert(Contains(lifecycle.CollectDueActions(2190.0).suspend, "acme.mod/panel"));

	// Warm views suspend but are never reclaimed.
	lifecycle.NoteLoaded("osfui/settings", true, 0.0);
	assert(Contains(lifecycle.CollectDueActions(90.0).suspend, "osfui/settings"));
	lifecycle.NoteSuspendRequested("osfui/settings");
	assert(!Contains(lifecycle.CollectDueActions(5000.0).destroy, "osfui/settings"));

	// A host restart loses latches and restarts hidden grace periods.
	lifecycle.OnHostRestart(6000.0);
	assert(lifecycle.CollectDueActions(6089.999).suspend.empty());
	const auto restarted = lifecycle.CollectDueActions(6090.0);
	assert(Contains(restarted.suspend, "acme.mod/panel"));
	assert(Contains(restarted.suspend, "osfui/settings"));

	lifecycle.NoteDestroyed("acme.mod/panel");
	assert(!Contains(lifecycle.CollectDueActions(9000.0).destroy, "acme.mod/panel"));

	std::cout << "view_lifecycle_tests: ok\n";
	return 0;
}
