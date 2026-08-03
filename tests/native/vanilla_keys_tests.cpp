// Host-side tests for VanillaKeys (docs\mcm-design.md §9 "vanilla hotkeys",
// v1): the curated defaults table (vanillakeys.json shape) + the engine
// controlmap overlay parser. Bindings live in the controlmap's own code space
// (DIK scan codes), so the overlay consumes hex tokens verbatim — only the
// name resolver (key name -> scan) is injected, faked here. Assert-style;
// process exit code is the failure count.

#include "runtime/VanillaKeys.h"

#include "core/Log.h"

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK(expr)                                                                     \
	do {                                                                                \
		++g_checks;                                                                     \
		if (!(expr)) {                                                                  \
			++g_failures;                                                               \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
		}                                                                               \
	} while (0)

	void WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}

	// Deterministic stand-in for the injected name resolver (name -> scan
	// code, DIK values — the same space the controlmap hex tokens use).
	std::uint32_t FakeNames(std::string_view a_name)
	{
		if (a_name == "F5") return 0x3F;
		if (a_name == "F9") return 0x43;
		if (a_name == "E") return 0x12;
		if (a_name == "Grave") return 0x29;
		return 0;  // "NotAKey" etc.
	}

	// The scan code for an event in Bindings(), or 0 (also 0 when unbound).
	std::uint32_t CodeOf(const OSFUI::VanillaKeys& a_keys, std::string_view a_event)
	{
		for (const auto& b : a_keys.Bindings()) {
			if (b.event == a_event) {
				return b.code;
			}
		}
		return 0;
	}
}

// core/Log.h declarations (real impl pulls game deps — stub, as in the other
// suites).
namespace OSFUI::Log
{
	static bool g_devMode = true;

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DevMode() { return g_devMode; }
	void SetDevMode(bool a_enabled) { g_devMode = a_enabled; }
}

int main()
{
	using OSFUI::VanillaKeys;
	namespace fs = std::filesystem;

	const auto root = fs::temp_directory_path() / "osfui-vanilla-keys-tests";
	fs::remove_all(root);

	// --- LoadDefaults: shape tolerance + name resolution --------------------------
	{
		WriteFile(root / "vanillakeys.json", R"json({
			"$comment": "curated",
			"bindings": [
				{ "event": "QuickSave", "label": "Quicksave", "key": "F5" },
				{ "event": "QuickLoad", "label": "Quickload", "key": "F9" },
				{ "event": "Activate", "label": "Interact", "key": "E" },
				{ "event": "Console", "label": "Console", "key": "Grave" },
				{ "event": "Broken", "label": "Unresolvable", "key": "NotAKey" },
				{ "event": "NoKey", "label": "Missing key" },
				{ "event": "NoLabel", "key": "E" },
				42
			] })json");

		VanillaKeys keys;
		CHECK(keys.LoadDefaults(root / "vanillakeys.json", FakeNames));
		CHECK(keys.Bindings().size() == 4);  // bad rows skipped, not fatal
		CHECK(CodeOf(keys, "QuickSave") == 0x3F);
		CHECK(CodeOf(keys, "Console") == 0x29);

		// Missing / invalid files: false, and no stale data survives.
		CHECK(!keys.LoadDefaults(root / "nope.json", FakeNames));
		CHECK(keys.Bindings().empty());
		WriteFile(root / "bad.json", "[1, 2]");
		CHECK(!keys.LoadDefaults(root / "bad.json", FakeNames));
	}

	// --- OverlayControlMap: the engine text format --------------------------------
	{
		VanillaKeys keys;
		WriteFile(root / "vanillakeys.json", R"json({ "bindings": [
			{ "event": "QuickSave", "label": "Quicksave", "key": "F5" },
			{ "event": "QuickLoad", "label": "Quickload", "key": "F9" },
			{ "event": "Activate", "label": "Interact", "key": "E" },
			{ "event": "Console", "label": "Console", "key": "Grave" }
		] })json");
		CHECK(keys.LoadDefaults(root / "vanillakeys.json", FakeNames));

		// Rebind, unbind, chord, unknown event, comments, alternates, and a
		// second occurrence (menu context) that must NOT win over the first.
		WriteFile(root / "ControlMap_Custom.txt",
			"// Main Gameplay\n"
			"QuickSave\t0x14\t0xff\t0xff\n"                  // F5 -> T
			"Activate\t0xff\t0xff\t0x1000\n"                 // unbound on keyboard
			"QuickLoad\t0x1d+0x14,0x3f\t0xff\t0xff\n"        // chord skipped; alt 0x3f (F5) wins
			"SomeMenuThing\t0x29\t0xff\t0xff\n"              // not in the table: ignored
			"\n"
			"// Menu Context\n"
			"QuickSave\t0x29\t0xff\t0xff\n");                // later context: ignored

		CHECK(keys.OverlayControlMap(root / "ControlMap_Custom.txt") == 3);
		CHECK(CodeOf(keys, "QuickSave") == 0x14);   // DIK_T, from the FIRST occurrence
		CHECK(CodeOf(keys, "Activate") == 0);       // unbound
		CHECK(CodeOf(keys, "QuickLoad") == 0x3F);   // the parseable alternate (DIK_F5)
		CHECK(CodeOf(keys, "Console") == 0x29);     // untouched

		// A second overlay (engine order: Data override, then user remaps)
		// applies on top of the first.
		WriteFile(root / "ControlMap.txt", "console\t0xC8\t0xff\t0xff\n");  // event match is case-insensitive
		CHECK(keys.OverlayControlMap(root / "ControlMap.txt") == 1);
		CHECK(CodeOf(keys, "Console") == 0xC8);     // DIK_UP applied verbatim (extended)

		// Missing overlay file: silent no-op.
		CHECK(keys.OverlayControlMap(root / "absent.txt") == 0);
	}

	// --- OverlayUserFile: the additive user overlay (api-freeze item 7) -----------
	{
		VanillaKeys keys;
		WriteFile(root / "vanillakeys.json", R"json({ "formatVersion": 1, "bindings": [
			{ "event": "QuickSave", "label": "Quicksave", "key": "F5" },
			{ "event": "QuickLoad", "label": "Quickload", "key": "F9" },
			{ "event": "Activate", "label": "Interact", "key": "E" },
			{ "event": "Console", "label": "Console", "key": "Grave" }
		] })json");
		CHECK(keys.LoadDefaults(root / "vanillakeys.json", FakeNames));

		// add (new row), replace (rebind + optional relabel), suppress (drop),
		// plus every typo diagnostic: unknown replace/suppress events, an
		// unresolvable key, an add colliding with an existing event.
		WriteFile(root / "vanillakeys.user.json", R"json({
			"formatVersion": 1,
			"add": [
				{ "event": "PhotoMode", "label": "Photo mode", "key": "F9" },
				{ "event": "QuickSave", "label": "Duplicate", "key": "E" },
				{ "event": "BadKey", "label": "Nope", "key": "NotAKey" },
				{ "label": "" }
			],
			"replace": [
				{ "event": "quicksave", "key": "E", "label": "Quicksave (moved)" },
				{ "event": "Ghost", "key": "E" },
				{ "event": "QuickLoad", "key": "NotAKey" }
			],
			"suppress": [ "Console", "AlsoGhost" ],
			"unexpectedKey": 1
		})json");

		CHECK(keys.OverlayUserFile(root / "vanillakeys.user.json", FakeNames) == 3);  // add + replace + suppress
		CHECK(keys.Bindings().size() == 4);            // 4 - Console + PhotoMode
		CHECK(CodeOf(keys, "Console") == 0);           // suppressed = gone
		CHECK(CodeOf(keys, "PhotoMode") == 0x43);      // added (F9)
		CHECK(CodeOf(keys, "QuickSave") == 0x12);      // replaced (case-insensitive event match)
		CHECK(CodeOf(keys, "QuickLoad") == 0x43);      // unresolvable replace: unchanged
		{
			bool relabeled = false;
			for (const auto& b : keys.Bindings()) {
				relabeled = relabeled || b.label == "Quicksave (moved)";
			}
			CHECK(relabeled);
		}
		for (const auto* needle : { "unknown key 'unexpectedKey'", "replaces unknown event 'Ghost'",
		                            "suppresses unknown event 'AlsoGhost'", "unresolvable key 'NotAKey'",
		                            "adds event 'QuickSave' which already exists" }) {
			bool logged = false;
			for (const auto& entry : REX::test::Entries()) {
				logged = logged || entry.find(needle) != std::string::npos;
			}
			CHECK(logged);
		}

		// Missing overlay: silent no-op. Malformed: warned no-op.
		CHECK(keys.OverlayUserFile(root / "absent.user.json", FakeNames) == 0);
		WriteFile(root / "badly.user.json", "[]");
		CHECK(keys.OverlayUserFile(root / "badly.user.json", FakeNames) == 0);
		CHECK(keys.Bindings().size() == 4);
	}

	fs::remove_all(root);
	std::fprintf(stderr, "vanilla_keys_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
