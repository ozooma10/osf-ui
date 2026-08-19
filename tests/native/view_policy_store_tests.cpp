#include "Views/ViewPolicyStore.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using OSFUI::ViewPolicyStore;

namespace
{
	const std::filesystem::path kDir{ ".build/view-policy-tests" };
	const std::filesystem::path kPath = kDir / "view-policy.json";

	void WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}
}

int main()
{
	std::filesystem::remove_all(kDir);

	// Missing file: defaults rule in both directions, no overrides recorded.
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(store.HudAutoStart("acme.mod/hud", true));
		assert(!store.HudAutoStart("acme.mod/hud", false));
		assert(!store.HasHudOverride("acme.mod/hud"));
	}

	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(store.SetHudAutoStart("acme.mod/hud", false));
		assert(!store.HudAutoStart("acme.mod/hud", true));

		ViewPolicyStore reloaded;
		reloaded.Load(kPath);
		assert(reloaded.HasHudOverride("acme.mod/hud"));
		assert(!reloaded.HudAutoStart("acme.mod/hud", true));
		assert(reloaded.SetHudAutoStart("acme.mod/hud", true));
		assert(reloaded.HudAutoStart("acme.mod/hud", false));
	}

	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(store.SetHudAutoStart("gone.mod/compass", true));
		assert(store.SetHudAutoStart("acme.mod/hud", false));

		ViewPolicyStore reloaded;
		reloaded.Load(kPath);
		assert(reloaded.HasHudOverride("gone.mod/compass"));
		assert(reloaded.HudAutoStart("gone.mod/compass", false));
	}

	// Invalid keys and non-boolean values are skipped; opaque mixed-case mod ids load.
	WriteFile(kPath, R"({
		"formatVersion": 1,
		"hudOverrides": {
			"acme.mod/hud": true,
			"../evil": true,
			"UPPER.Case/hud": false,
			"acme.mod/other": "yes"
		}
	})");
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(store.HasHudOverride("acme.mod/hud"));
		assert(!store.HasHudOverride("../evil"));
		assert(store.HasHudOverride("UPPER.Case/hud"));
		assert(!store.HudAutoStart("UPPER.Case/hud", true));
		assert(!store.HasHudOverride("acme.mod/other"));
	}

	WriteFile(kPath, R"({
		"formatVersion": 99,
		"someFutureField": { "x": 1 },
		"hudOverrides": { "acme.mod/hud": false }
	})");
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(!store.HudAutoStart("acme.mod/hud", true));
	}

	WriteFile(kPath, "{ not json !!!");
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(!store.HasHudOverride("acme.mod/hud"));
		assert(!std::filesystem::exists(kPath));
		assert(std::filesystem::exists(kDir / "view-policy.json.bad"));
		assert(store.SetHudAutoStart("acme.mod/hud", true));
		assert(std::filesystem::exists(kPath));
	}

	// Valid JSON that is not an object takes the same quarantine path.
	WriteFile(kPath, "[1, 2, 3]");
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(!store.HasHudOverride("acme.mod/hud"));
		assert(std::filesystem::exists(kDir / "view-policy.json.bad"));
	}

	std::filesystem::remove_all(kDir);
	{
		ViewPolicyStore store;
		store.Load(kPath);
		assert(store.SetHudAutoStart("acme.mod/hud", true));
		std::filesystem::remove(kPath);
		std::filesystem::create_directories(kPath);  // poison further writes
		// A previous value is restored...
		assert(!store.SetHudAutoStart("acme.mod/hud", false));
		assert(store.HudAutoStart("acme.mod/hud", false));  // still the persisted true
		// ...and a never-persisted id rolls back to absent.
		assert(!store.SetHudAutoStart("acme.mod/fresh", true));
		assert(!store.HasHudOverride("acme.mod/fresh"));
	}

	std::filesystem::remove_all(kDir);
	std::cout << "view_policy_store_tests: ok\n";
	return 0;
}
