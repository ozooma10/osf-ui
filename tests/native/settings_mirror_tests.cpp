// Native desktop unit tests for the native ABI settings mirror:
// the REAL src/API/SettingsMirror.cpp — plus its integration with the real
// SettingsStore, wired exactly like Runtime::BuildModules does — compiled
// against stubs/pch.h on the desktop toolchain. Assert-style; process exit
// code is the failure count.

#include "API/SettingsMirror.h"
#include "Settings/SettingsStore.h"

#include "Core/Log.h"
#include "check.h"

namespace
{

}

// Core/Log.h declarations (real impl pulls game deps — stub, as in the other
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
	using OSFUI::SettingsStore;
	using OSFUI::API::SettingsMirror;
	namespace fs = std::filesystem;

	// --- standalone: typed getters over Update-fed values ---------------------
	{
		SettingsMirror mirror;
		mirror.Update("t.alpha", "enabled", true);
		mirror.Update("t.alpha", "count", 42);
		mirror.Update("t.alpha", "scale", 1.5);
		mirror.Update("t.alpha", "name", "hello");

		bool b{};
		std::int64_t i{};
		double f{};
		CHECK(mirror.GetBool("t.alpha", "enabled", &b) && b == true);
		CHECK(mirror.GetInt("t.alpha", "count", &i) && i == 42);
		CHECK(mirror.GetFloat("t.alpha", "scale", &f) && f == 1.5);

		char buf[16] = {};
		CHECK(mirror.GetString("t.alpha", "name", buf, sizeof(buf)) == 6);  // "hello" + NUL
		CHECK(std::string(buf) == "hello");
	}

	// --- value-shape mismatches ------------------------------------------------
	{
		SettingsMirror mirror;
		mirror.Update("m", "b", true);
		mirror.Update("m", "i", 7);
		mirror.Update("m", "f", 2.5);
		mirror.Update("m", "s", "text");

		bool b{};
		std::int64_t i{};
		double f{};
		char buf[8] = {};
		CHECK(!mirror.GetBool("m", "i", &b));   // int is not a bool
		CHECK(!mirror.GetInt("m", "f", &i));    // 2.5 is not integral
		CHECK(!mirror.GetInt("m", "b", &i));    // bool is not a number
		CHECK(mirror.GetFloat("m", "i", &f) && f == 7.0);  // integral JSON is a number (documented)
		CHECK(mirror.GetString("m", "i", buf, sizeof(buf)) == 0);
		CHECK(!mirror.GetBool("m", "s", &b));
	}

	// --- unknown / null arguments never resolve or crash ------------------------
	{
		SettingsMirror mirror;
		mirror.Update("m", "k", 1);

		bool b{};
		std::int64_t i{};
		char buf[8] = {};
		CHECK(!mirror.GetBool("nosuch", "k", &b));
		CHECK(!mirror.GetInt("m", "nosuch", &i));
		CHECK(!mirror.GetInt(nullptr, "k", &i));
		CHECK(!mirror.GetInt("m", nullptr, &i));
		CHECK(!mirror.GetInt("m", "k", nullptr));
		CHECK(mirror.GetString("m", "k", buf, sizeof(buf)) == 0);  // int, not string
		CHECK(mirror.GetString(nullptr, nullptr, buf, sizeof(buf)) == 0);
	}

	// --- GetString buffer semantics ---------------------------------------------
	{
		SettingsMirror mirror;
		mirror.Update("m", "s", "abcdef");  // required = 7

		// Size probe: null buffer still reports the required length.
		CHECK(mirror.GetString("m", "s", nullptr, 0) == 7);

		// Truncation: copies bufLen-1 chars, NUL-terminated, still reports 7.
		char small[4] = { 'x', 'x', 'x', 'x' };
		CHECK(mirror.GetString("m", "s", small, sizeof(small)) == 7);
		CHECK(std::string(small) == "abc");

		// Exact fit.
		char exact[7] = {};
		CHECK(mirror.GetString("m", "s", exact, sizeof(exact)) == 7);
		CHECK(std::string(exact) == "abcdef");

		// Empty string is representable: required = 1.
		mirror.Update("m", "empty", "");
		char tiny[2] = { 'x', 'x' };
		CHECK(mirror.GetString("m", "empty", tiny, sizeof(tiny)) == 1);
		CHECK(tiny[0] == '\0');
	}

	// --- GetInt unsigned-overflow guard ------------------------------------------
	{
		SettingsMirror mirror;
		mirror.Update("m", "huge", std::uint64_t{ 0xFFFF'FFFF'FFFF'FFFFull });
		std::int64_t i{};
		CHECK(!mirror.GetInt("m", "huge", &i));  // not representable, refused

		mirror.Update("m", "negative", -5);
		CHECK(mirror.GetInt("m", "negative", &i) && i == -5);
	}

	// --- Rebuild: replaces wholesale, tolerates malformed input -------------------
	{
		SettingsMirror mirror;
		mirror.Update("old", "k", 1);
		mirror.Rebuild(nlohmann::json{
			{ "mods", nlohmann::json::array({
				{ { "id", "fresh" }, { "values", { { "k", 2 } } } },
				{ { "id", 42 }, { "values", { { "k", 3 } } } },       // bad id: skipped
				{ { "id", "novalues" } },                              // missing values: skipped
			}) },
		});

		std::int64_t i{};
		CHECK(!mirror.GetInt("old", "k", &i));           // wholesale replace pruned it
		CHECK(mirror.GetInt("fresh", "k", &i) && i == 2);
		CHECK(!mirror.GetInt("novalues", "k", &i));

		mirror.Rebuild(nlohmann::json::array({ 1, 2 }));  // not even an object: empty mirror, no throw
		CHECK(!mirror.GetInt("fresh", "k", &i));
	}

	// --- integration: real SettingsStore feeding the mirror (Runtime wiring) ------
	{
		const auto root = fs::temp_directory_path() / "osfui-settings-mirror-tests";
		fs::remove_all(root);
		const auto schemaDir = root / "settings";
		const auto valuesDir = root / "values";
		fs::create_directories(schemaDir);

		SettingsStore store;
		SettingsMirror mirror;
		// Exactly the Runtime::BuildModules wiring.
		store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			mirror.Update(a_mod, a_key, a_value);
		});
		store.AddRegistryListener([&] { mirror.Rebuild(store.Data()); });

		store.LoadAll(schemaDir, valuesDir);  // empty dir — native registration follows
		CHECK(store.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.beta", "title": "Beta",
			"groups": [ { "label": "G", "settings": [
				{ "key": "enabled", "type": "bool",  "default": true },
				{ "key": "scale",   "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 },
				{ "key": "mode",    "type": "enum",  "default": "compact", "options": ["compact", "full"] }
			] } ] })json"),
			SettingsStore::Source::kNative));

		// RegisterSchema's per-mod replay populated the mirror without any read step.
		bool b{};
		double f{};
		char buf[16] = {};
		CHECK(mirror.GetBool("t.beta", "enabled", &b) && b == true);
		CHECK(mirror.GetFloat("t.beta", "scale", &f) && f == 1.0);
		CHECK(mirror.GetString("t.beta", "mode", buf, sizeof(buf)) == 8);
		CHECK(std::string(buf) == "compact");

		// A Set lands the CLAMPED value in the mirror — the reconciled truth,
		// not the caller's raw input.
		CHECK(store.Set("t.beta", "scale", "9.9"));
		CHECK(mirror.GetFloat("t.beta", "scale", &f) && f == 2.0);

		// Reset restores the default in the mirror too.
		CHECK(store.Reset("t.beta", "scale"));
		CHECK(mirror.GetFloat("t.beta", "scale", &f) && f == 1.0);

		// RemoveMod → registry listener → Rebuild: the mod stops resolving.
		CHECK(store.RemoveMod("t.beta"));
		CHECK(!mirror.GetBool("t.beta", "enabled", &b));

		fs::remove_all(root);
	}

	// --- LookupMod / LookupKey: exact-then-case-insensitive -------------------
	// The shared lookup behind ResolveNames and Find. Papyrus interns strings as
	// BSFixedString, which hands back the FIRST-seen casing process-wide, so a
	// script's literal spelling is unreliable and the mirror has to fall back to
	// an ASCII-case-insensitive scan. Both properties below are load-bearing and
	// were previously unexercised: an exact match must beat a case-variant
	// sibling, and an empty key must resolve the mod alone (whole-mod Reset).
	{
		SettingsMirror mirror;
		mirror.Update("t.gamma", "Speed", 1);
		mirror.Update("t.gamma", "speed", 2);  // key differing only in case
		mirror.Update("T.GAMMA", "Speed", 3);  // mod differing only in case

		std::string mod, key;

		// Empty key resolves the mod ONLY: a_outKey is CLEARED, and this must
		// succeed even though no key lookup happens. Both outputs are seeded with
		// a sentinel first — `key` starts empty, so asserting key.empty() against a
		// default-constructed string would pass even if clear() were never called.
		mod = "SENTINEL";
		key = "SENTINEL";
		CHECK(mirror.ResolveNames("t.gamma", "", mod, key) && mod == "t.gamma" && key.empty());
		// ...including when the mod itself only matches case-insensitively.
		mod = "SENTINEL";
		key = "SENTINEL";
		CHECK(mirror.ResolveNames("T.Gamma", "", mod, key) && mod != "SENTINEL" && key.empty());

		// Exact beats case-variant, in BOTH directions — neither spelling may be
		// shadowed by the other's CI match.
		CHECK(mirror.ResolveNames("t.gamma", "Speed", mod, key) && mod == "t.gamma" && key == "Speed");
		CHECK(mirror.ResolveNames("T.GAMMA", "Speed", mod, key) && mod == "T.GAMMA" && key == "Speed");
		CHECK(mirror.ResolveNames("t.gamma", "speed", mod, key) && mod == "t.gamma" && key == "speed");

		// A case-variant with no exact twin falls back and folds to the authored
		// spelling — the whole point of the fallback.
		CHECK(mirror.ResolveNames("t.gamma", "SPEED", mod, key) && mod == "t.gamma" &&
			(key == "Speed" || key == "speed"));

		// Unknown mod and unknown key both fail. Sentinels again: a false return
		// must not be accompanied by a half-written output the caller might read.
		mod = "SENTINEL";
		key = "SENTINEL";
		CHECK(!mirror.ResolveNames("t.nope", "Speed", mod, key) && mod == "SENTINEL" && key == "SENTINEL");
		// An unknown key fails even though the MOD resolved — and in that case
		// a_outMod has legitimately been written, so only a_outKey must remain unchanged.
		key = "SENTINEL";
		CHECK(!mirror.ResolveNames("t.gamma", "nosuchkey", mod, key) && key == "SENTINEL");

		// The same exact-then-CI path through the typed getters (which go via
		// Find, the const char* entry point).
		std::int64_t got{};
		CHECK(mirror.GetInt("t.gamma", "Speed", &got) && got == 1);
		CHECK(mirror.GetInt("T.GAMMA", "Speed", &got) && got == 3);
		CHECK(mirror.GetInt("t.GAMMA", "SPEED", &got));  // both sides CI: resolves to one of them
		CHECK(!mirror.GetInt("t.gamma", "nosuchkey", &got));
	}

	std::fprintf(stderr, "settings_mirror_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
