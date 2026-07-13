// Host-side unit tests for the SubscribeSettings bookkeeping (docs/
// mcm-design.md §8.2): the REAL src/api/SettingsSubscriptions.cpp — replay on
// subscribe, queued change dispatch, unsubscribe semantics, and its
// integration with the real SettingsStore + SettingsMirror wired exactly like
// Runtime::BuildModules does — compiled against stubs/pch.h on the desktop
// toolchain. Assert-style; process exit code is the failure count.

#include "api/SettingsSubscriptions.h"
#include "runtime/SettingsStore.h"

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

	struct Record
	{
		std::string mod;
		std::string key;
		std::string valueJson;
	};
	using Trace = std::vector<Record>;

	void Recorder(const char* a_mod, const char* a_key, const char* a_valueJson, void* a_user) noexcept
	{
		static_cast<Trace*>(a_user)->push_back({ a_mod, a_key, a_valueJson });
	}

	// The (key -> valueJson) view of a trace slice, for order-insensitive
	// replay assertions (mirror iteration order is unspecified).
	std::unordered_map<std::string, std::string> ByKey(const Trace& a_trace, std::size_t a_first = 0, std::size_t a_count = SIZE_MAX)
	{
		std::unordered_map<std::string, std::string> out;
		for (std::size_t i = a_first; i < a_trace.size() && i - a_first < a_count; ++i) {
			out[a_trace[i].key] = a_trace[i].valueJson;
		}
		return out;
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
	using OSFUI::SettingsStore;
	using OSFUI::API::SettingsMirror;
	using OSFUI::API::SettingsSubscriptions;
	namespace fs = std::filesystem;

	// --- argument validation + token allocation ---------------------------------
	{
		SettingsSubscriptions subs;
		Trace trace;
		CHECK(subs.Subscribe(nullptr, Recorder, &trace) == 0);
		CHECK(subs.Subscribe("", Recorder, &trace) == 0);
		CHECK(subs.Subscribe("m", nullptr, &trace) == 0);

		const auto t1 = subs.Subscribe("m", Recorder, &trace);
		const auto t2 = subs.Subscribe("m", Recorder, &trace);
		CHECK(t1 != 0 && t2 != 0 && t1 != t2);

		subs.Unsubscribe(0);       // sentinel: no-op, no crash
		subs.Unsubscribe(99999);   // unknown: no-op
	}

	// --- replay on subscribe: once per current value, ONE-shot -------------------
	{
		SettingsMirror mirror;
		mirror.Update("alpha", "enabled", true);
		mirror.Update("alpha", "count", 42);
		mirror.Update("alpha", "name", "hi");

		SettingsSubscriptions subs;
		Trace trace;
		CHECK(subs.Subscribe("alpha", Recorder, &trace) != 0);
		CHECK(trace.empty());  // nothing until the main-thread Pump

		subs.Pump(mirror);
		CHECK(trace.size() == 3);
		const auto values = ByKey(trace);
		CHECK(values.at("enabled") == "true");     // serialized JSON text,
		CHECK(values.at("count") == "42");         // not display strings
		CHECK(values.at("name") == "\"hi\"");
		for (const auto& r : trace) {
			CHECK(r.mod == "alpha");
		}

		subs.Pump(mirror);  // replay is one-shot
		CHECK(trace.size() == 3);
	}

	// --- unknown mod: legal subscribe, empty one-shot replay ---------------------
	{
		SettingsMirror mirror;
		SettingsSubscriptions subs;
		Trace trace;

		// An event with NO subscriber anywhere is dropped at OnChanged...
		subs.OnChanged("ghost", "k", 1);
		CHECK(subs.Subscribe("ghost", Recorder, &trace) != 0);
		subs.Pump(mirror);
		CHECK(trace.empty());  // ...so the late subscriber never sees it

		// The snapshot replay does NOT re-arm when the mod appears later in
		// the mirror alone — late registration is delivered by the store's
		// per-mod replay through OnChanged (integration test below).
		mirror.Update("ghost", "k", 2);
		subs.Pump(mirror);
		CHECK(trace.empty());
	}

	// --- change dispatch: per-mod routing, valueJson text -------------------------
	{
		SettingsMirror mirror;  // empty: replays contribute nothing
		SettingsSubscriptions subs;
		Trace a, b;
		CHECK(subs.Subscribe("alpha", Recorder, &a) != 0);
		CHECK(subs.Subscribe("beta", Recorder, &b) != 0);
		subs.Pump(mirror);  // consume the empty replays

		subs.OnChanged("alpha", "x", 1);
		subs.OnChanged("beta", "y", "s");
		subs.OnChanged("nobody", "z", 3);  // no subscriber: dropped
		subs.Pump(mirror);

		CHECK(a.size() == 1 && a[0].mod == "alpha" && a[0].key == "x" && a[0].valueJson == "1");
		CHECK(b.size() == 1 && b[0].mod == "beta" && b[0].key == "y" && b[0].valueJson == "\"s\"");

		subs.Pump(mirror);  // queue drained
		CHECK(a.size() == 1 && b.size() == 1);
	}

	// --- replay-before-events ordering in one Pump --------------------------------
	{
		SettingsMirror mirror;
		mirror.Update("alpha", "a", 1);

		SettingsSubscriptions subs;
		Trace trace;
		CHECK(subs.Subscribe("alpha", Recorder, &trace) != 0);

		// A commit lands between Subscribe and Pump — Runtime wiring updates
		// the mirror FIRST, then queues the event.
		mirror.Update("alpha", "b", 2);
		subs.OnChanged("alpha", "b", 2);

		subs.Pump(mirror);
		// Replay of the CURRENT values (a=1, b=2) first, then the queued
		// event — b arrives twice with the identical value (documented benign
		// duplicate).
		CHECK(trace.size() == 3);
		const auto replayed = ByKey(trace, 0, 2);
		CHECK(replayed.size() == 2);
		CHECK(replayed.at("a") == "1");
		CHECK(replayed.at("b") == "2");
		CHECK(trace[2].key == "b" && trace[2].valueJson == "2");
	}

	// --- unsubscribe: queued events stop resolving --------------------------------
	{
		SettingsMirror mirror;
		SettingsSubscriptions subs;
		Trace trace;
		const auto token = subs.Subscribe("alpha", Recorder, &trace);
		subs.Pump(mirror);

		subs.OnChanged("alpha", "k", 1);  // queued while subscribed
		subs.Unsubscribe(token);
		subs.Pump(mirror);
		CHECK(trace.empty());
	}

	// --- unsubscribe from INSIDE a callback stops same-Pump delivery --------------
	{
		struct KillCtx
		{
			SettingsSubscriptions* subs{ nullptr };
			std::uint32_t          token{ 0 };
			Trace                  trace;
		};
		const auto killAfterFirst = [](const char* a_mod, const char* a_key, const char* a_valueJson, void* a_user) noexcept {
			auto* ctx = static_cast<KillCtx*>(a_user);
			ctx->trace.push_back({ a_mod, a_key, a_valueJson });
			ctx->subs->Unsubscribe(ctx->token);
		};

		SettingsMirror mirror;
		SettingsSubscriptions subs;
		KillCtx ctx{ &subs };
		ctx.token = subs.Subscribe("alpha", killAfterFirst, &ctx);
		CHECK(ctx.token != 0);
		subs.Pump(mirror);

		subs.OnChanged("alpha", "one", 1);
		subs.OnChanged("alpha", "two", 2);
		subs.Pump(mirror);
		CHECK(ctx.trace.size() == 1);  // second call skipped by the liveness re-check
	}

	// --- re-entrant Subscribe from a callback: no deadlock, replays next Pump -----
	{
		struct GrowCtx
		{
			SettingsSubscriptions* subs{ nullptr };
			Trace*                 other{ nullptr };
			Trace                  trace;
		};
		const auto subscribeMore = [](const char* a_mod, const char* a_key, const char* a_valueJson, void* a_user) noexcept {
			auto* ctx = static_cast<GrowCtx*>(a_user);
			ctx->trace.push_back({ a_mod, a_key, a_valueJson });
			(void)ctx->subs->Subscribe("alpha", Recorder, ctx->other);
		};

		SettingsMirror mirror;
		mirror.Update("alpha", "k", 7);
		SettingsSubscriptions subs;
		Trace other;
		GrowCtx ctx{ &subs, &other };
		CHECK(subs.Subscribe("alpha", subscribeMore, &ctx) != 0);

		subs.Pump(mirror);  // replay -> callback -> re-entrant Subscribe
		CHECK(ctx.trace.size() == 1);
		CHECK(other.empty());  // fresh sub's replay is deferred...

		subs.Pump(mirror);  // ...to the next Pump
		CHECK(other.size() == 1 && other[0].key == "k" && other[0].valueJson == "7");
	}

	// --- integration: real SettingsStore + mirror, exact Runtime wiring -----------
	{
		const auto root = fs::temp_directory_path() / "osfui-settings-subscriptions-tests";
		fs::remove_all(root);
		const auto schemaDir = root / "settings";
		const auto valuesDir = root / "values";
		fs::create_directories(schemaDir);

		SettingsStore store;
		SettingsMirror mirror;
		SettingsSubscriptions subs;
		// Exactly the Runtime::BuildModules wiring (mirror first, then the
		// subscriber feed).
		store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			mirror.Update(a_mod, a_key, a_value);
			subs.OnChanged(a_mod, a_key, a_value);
		});
		store.AddRegistryListener([&] { mirror.Rebuild(store.Data()); });

		store.LoadAll(schemaDir, valuesDir);  // empty dir — runtime registration follows

		// Subscribe BEFORE the mod exists: legal, silent until it registers.
		Trace early;
		CHECK(subs.Subscribe("beta", Recorder, &early) != 0);
		subs.Pump(mirror);
		CHECK(early.empty());

		CHECK(store.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "beta", "title": "Beta",
			"groups": [ { "label": "G", "settings": [
				{ "key": "enabled", "type": "bool",  "default": true },
				{ "key": "scale",   "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 },
				{ "key": "mode",    "type": "enum",  "default": "compact", "options": ["compact", "full"] }
			] } ] })json"),
			SettingsStore::Source::kNative));

		// The store's per-mod registration replay flowed through OnChanged —
		// the load-order-insurance replay, no snapshot involved.
		subs.Pump(mirror);
		CHECK(early.size() == 3);
		const auto initial = ByKey(early);
		CHECK(initial.at("enabled") == "true");
		CHECK(initial.at("scale") == "1.0");
		CHECK(initial.at("mode") == "\"compact\"");

		// Subscribe AFTER registration: the mirror snapshot replay.
		Trace late;
		CHECK(subs.Subscribe("beta", Recorder, &late) != 0);
		subs.Pump(mirror);
		CHECK(late.size() == 3);
		CHECK(ByKey(late) == initial);

		// A Set delivers the CLAMPED committed value to both subscribers.
		early.clear();
		late.clear();
		CHECK(store.Set("beta", "scale", "9.9"));
		subs.Pump(mirror);
		CHECK(early.size() == 1 && early[0].key == "scale" && early[0].valueJson == "2.0");
		CHECK(late.size() == 1 && late[0].valueJson == "2.0");

		// RemoveMod: no change events — values simply stop resolving.
		early.clear();
		late.clear();
		CHECK(store.RemoveMod("beta"));
		subs.Pump(mirror);
		CHECK(early.empty() && late.empty());
		bool b{};
		CHECK(!mirror.GetBool("beta", "enabled", &b));

		fs::remove_all(root);
	}

	std::fprintf(stderr, "settings_subscriptions_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
