// Host-side unit tests for the SubscribeHotkey bookkeeping (docs/
// mcm-design.md §9): the REAL src/api/HotkeySubscriptions.cpp — per-(mod, key)
// routing, queued fire dispatch, unsubscribe semantics (including from inside
// a callback), and re-entrant subscribe — compiled against stubs/pch.h on the
// desktop toolchain. Assert-style; process exit code is the failure count.

#include "api/HotkeySubscriptions.h"

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
	};
	using Trace = std::vector<Record>;

	void Recorder(const char* a_mod, const char* a_key, void* a_user) noexcept
	{
		static_cast<Trace*>(a_user)->push_back({ a_mod, a_key });
	}
}

int main()
{
	using OSFUI::API::HotkeySubscriptions;

	// --- argument validation + token allocation ---------------------------------
	{
		HotkeySubscriptions subs;
		Trace trace;
		CHECK(subs.Subscribe(nullptr, "k", Recorder, &trace) == 0);
		CHECK(subs.Subscribe("", "k", Recorder, &trace) == 0);
		CHECK(subs.Subscribe("m", nullptr, Recorder, &trace) == 0);
		CHECK(subs.Subscribe("m", "", Recorder, &trace) == 0);
		CHECK(subs.Subscribe("m", "k", nullptr, &trace) == 0);

		const auto t1 = subs.Subscribe("m", "k", Recorder, &trace);
		const auto t2 = subs.Subscribe("m", "k", Recorder, &trace);
		CHECK(t1 != 0 && t2 != 0 && t1 != t2);

		subs.Unsubscribe(0);      // sentinel: no-op, no crash
		subs.Unsubscribe(99999);  // unknown: no-op
	}

	// --- per-(mod, key) routing; no delivery before Pump -------------------------
	{
		HotkeySubscriptions subs;
		Trace a, b, c;
		CHECK(subs.Subscribe("alpha", "toggleHud", Recorder, &a) != 0);
		CHECK(subs.Subscribe("alpha", "openMenu", Recorder, &b) != 0);
		CHECK(subs.Subscribe("beta", "toggleHud", Recorder, &c) != 0);

		subs.OnFired("alpha", "toggleHud");
		CHECK(a.empty());  // nothing until the main-thread Pump

		subs.Pump();
		CHECK(a.size() == 1 && a[0].mod == "alpha" && a[0].key == "toggleHud");
		CHECK(b.empty());  // same mod, other key: not routed
		CHECK(c.empty());  // same key, other mod: not routed

		subs.Pump();  // queue drained
		CHECK(a.size() == 1);
	}

	// --- no matching subscriber: dropped at OnFired ------------------------------
	{
		HotkeySubscriptions subs;
		Trace trace;
		subs.OnFired("ghost", "k");  // nobody anywhere
		CHECK(subs.Subscribe("ghost", "k", Recorder, &trace) != 0);
		subs.Pump();
		CHECK(trace.empty());  // the late subscriber never sees the early fire
	}

	// --- fan-out to every matching subscriber, FIFO fire order -------------------
	{
		HotkeySubscriptions subs;
		Trace a, b;
		CHECK(subs.Subscribe("alpha", "toggleHud", Recorder, &a) != 0);
		CHECK(subs.Subscribe("alpha", "toggleHud", Recorder, &b) != 0);
		CHECK(subs.Subscribe("alpha", "openMenu", Recorder, &a) != 0);

		subs.OnFired("alpha", "toggleHud");
		subs.OnFired("alpha", "openMenu");
		subs.OnFired("alpha", "toggleHud");
		subs.Pump();

		CHECK(b.size() == 2);
		CHECK(a.size() == 3);
		CHECK(a[0].key == "toggleHud" && a[1].key == "openMenu" && a[2].key == "toggleHud");
	}

	// --- unsubscribe: queued fires stop resolving --------------------------------
	{
		HotkeySubscriptions subs;
		Trace trace;
		const auto token = subs.Subscribe("alpha", "k", Recorder, &trace);
		subs.OnFired("alpha", "k");  // queued while subscribed
		subs.Unsubscribe(token);
		subs.Pump();
		CHECK(trace.empty());
	}

	// --- unsubscribe from INSIDE a callback stops same-Pump delivery -------------
	{
		struct KillCtx
		{
			HotkeySubscriptions* subs{ nullptr };
			std::uint32_t        token{ 0 };
			Trace                trace;
		};
		const auto killAfterFirst = [](const char* a_mod, const char* a_key, void* a_user) noexcept {
			auto* ctx = static_cast<KillCtx*>(a_user);
			ctx->trace.push_back({ a_mod, a_key });
			ctx->subs->Unsubscribe(ctx->token);
		};

		HotkeySubscriptions subs;
		KillCtx ctx{ &subs };
		ctx.token = subs.Subscribe("alpha", "k", killAfterFirst, &ctx);
		CHECK(ctx.token != 0);

		subs.OnFired("alpha", "k");
		subs.OnFired("alpha", "k");
		subs.Pump();
		CHECK(ctx.trace.size() == 1);  // second call skipped by the liveness re-check
	}

	// --- re-entrant Subscribe from a callback: no deadlock, next-fire delivery ---
	{
		struct GrowCtx
		{
			HotkeySubscriptions* subs{ nullptr };
			Trace*               other{ nullptr };
			Trace                trace;
		};
		const auto subscribeMore = [](const char* a_mod, const char* a_key, void* a_user) noexcept {
			auto* ctx = static_cast<GrowCtx*>(a_user);
			ctx->trace.push_back({ a_mod, a_key });
			(void)ctx->subs->Subscribe("alpha", "k", Recorder, ctx->other);
		};

		HotkeySubscriptions subs;
		Trace other;
		GrowCtx ctx{ &subs, &other };
		CHECK(subs.Subscribe("alpha", "k", subscribeMore, &ctx) != 0);

		subs.OnFired("alpha", "k");
		subs.Pump();  // fire -> callback -> re-entrant Subscribe
		CHECK(ctx.trace.size() == 1);
		CHECK(other.empty());  // fires resolve to the set as of this Pump...

		subs.OnFired("alpha", "k");
		subs.Pump();
		CHECK(other.size() == 1 && other[0].mod == "alpha" && other[0].key == "k");
	}

	std::fprintf(stderr, "hotkey_subscriptions_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
