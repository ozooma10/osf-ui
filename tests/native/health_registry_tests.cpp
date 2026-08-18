// Native desktop unit tests for the session health registry (protocol 2.0):
// the REAL HealthRegistry + MessageBridge
// driven through actual 2.0 envelopes, with a capturing SendFn standing in for
// the renderer. Covers dedupe/occurrence counting, resolve/reactivate history,
// wire ordering, the payload sanitizer (no absolute paths, no shell targets,
// bounded size), and the delivery path.
//
// Delivery is the part that changed: `diagnostics.get` is GONE — it was a read
// whose real job was to subscribe the caller — and the registry is now the
// `osfui/diagnostics` STATE key. PublishStateAll only reaches views that have
// GREETED the bridge, so every test here boots its views the way the OSF UI runtime does
// (OnViewCreated, then a page-initiated `osfui.hello`), and the module's
// `_subscribers` set is gone with the read that used to fill it.
// Assert-style; process exit code is the failure count.

#include "Diagnostics/HealthRegistry.h"
#include "Bridge/MessageBridge.h"

#include "Core/Log.h"
#include "check.h"

namespace
{

	// One captured native->web envelope, flattened. 2.0 keeps every routing
	// field BESIDE the payload, so they are all top-level here too.
	struct Sent
	{
		std::string    view;
		std::string    kind;     // ready | state | event | reply | error
		std::string    mod;      // state mod
		std::string    key;      // state key
		nlohmann::json payload;  // event/reply/error payload, or a state's VALUE
	};

	std::vector<Sent> g_sent;

	// Capturing transport, shared by both bridge fixtures below.
	void Capture(std::string_view a_view, std::string_view a_json)
	{
		auto msg = nlohmann::json::parse(a_json, nullptr, false);
		// Every envelope the bridge encodes must be valid JSON: the encoders
		// splice pre-dumped text into hand-written wrappers, and a malformed one
		// would reach the renderer, not an exception handler.
		CHECK(!msg.is_discarded());
		const auto kind = msg.value("kind", "");
		g_sent.push_back(Sent{
			.view = std::string(a_view),
			.kind = kind,
			.mod = msg.value("mod", ""),
			.key = msg.value("key", ""),
			.payload = kind == "state" ? msg.value("value", nlohmann::json()) :
										 msg.value("payload", nlohmann::json()),
		});
	}

	// State envelopes for one "<mod>/<key>" pair. Both halves are asserted: a
	// value published under the wrong mod would reach a page's `<mod>/<key>`
	// subscription and nowhere else.
	std::vector<Sent> StateTo(std::string_view a_view, std::string_view a_mod, std::string_view a_key)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.kind == "state" && s.mod == a_mod && s.key == a_key) {
				out.push_back(s);
			}
		}
		return out;
	}

	std::vector<Sent> KindTo(std::string_view a_view, std::string_view a_kind)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.kind == a_kind) {
				out.push_back(s);
			}
		}
		return out;
	}

	// web -> native, exactly as the page helper builds it. A `send` carries no
	// id: it settles nothing.
	void Send(OSFUI::MessageBridge& a_bridge, std::string_view a_view, std::string_view a_name)
	{
		const nlohmann::json envelope = {
			{ "kind", "send" },
			{ "name", std::string(a_name) },
			{ "payload", nlohmann::json::object() },
		};
		a_bridge.HandleWebMessage(a_view, envelope.dump());
	}

	// Boot a view the way the OSF UI runtime does: arm its (closed) event gate, then let
	// the DOCUMENT greet. The page-initiated handshake is the only boot path.
	void Greet(OSFUI::MessageBridge& a_bridge, std::string_view a_view)
	{
		a_bridge.OnViewCreated(a_view);
		Send(a_bridge, a_view, "osfui.hello");
	}

	// The issue with this id in a snapshot, or a null json when absent.
	nlohmann::json IssueById(const nlohmann::json& a_snapshot, std::string_view a_id)
	{
		for (const auto& issue : a_snapshot.at("issues")) {
			if (issue.value("id", "") == a_id) {
				return issue;
			}
		}
		return nlohmann::json{};
	}

	std::vector<std::string> IssueIds(const nlohmann::json& a_snapshot)
	{
		std::vector<std::string> ids;
		for (const auto& issue : a_snapshot.at("issues")) {
			ids.push_back(issue.value("id", ""));
		}
		return ids;
	}
}

// Core/Log.h declarations (real impl pulls game deps — stub, as in the other
// suites).
namespace OSFUI::Log
{
	static bool g_debugEnabled = true;

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DebugEnabled() { return g_debugEnabled; }
	void SetDebugLogging(bool a_enabled) { g_debugEnabled = a_enabled; }
}

int main()
{
	using namespace OSFUI;
	using Severity = HealthRegistry::Severity;

	const auto spec = [](std::string a_id, std::string a_code, Severity a_severity,
						  std::string a_source, std::string a_subject = {},
						  nlohmann::json a_context = nlohmann::json::object()) {
		return HealthRegistry::IssueSpec{
			.id = std::move(a_id),
			.code = std::move(a_code),
			.severity = a_severity,
			.source = std::move(a_source),
			.subject = std::move(a_subject),
			.context = std::move(a_context),
		};
	};

	// --- Upsert deduplicates by id and counts occurrences ------------------
	{
		HealthRegistry healthRegistry;
		CHECK(healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 1.0));
		CHECK(healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 4.0));
		CHECK(healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 9.0));

		const auto snapshot = healthRegistry.Snapshot();
		CHECK(snapshot.at("issues").size() == 1);
		const auto issue = IssueById(snapshot, "view.load-failed:acme/panel");
		CHECK(issue.value("occurrences", 0u) == 3u);
		CHECK(issue.value("firstAt", -1.0) == 1.0);   // the first sighting is the anchor
		CHECK(issue.value("lastAt", -1.0) == 9.0);
		CHECK(issue.value("status", "") == "active");
		CHECK(issue.value("severity", "") == "error");
		CHECK(issue.value("source", "") == "views");
		CHECK(issue.value("sourceKind", "") == "platform");
		CHECK(issue.value("subject", "") == "acme/panel");
		CHECK(!issue.contains("resolvedAt"));

		// A different id is a different condition, even with the same code.
		CHECK(healthRegistry.Upsert(spec("view.load-failed:acme/hud", "view.load-failed", Severity::Error, "views", "acme/hud"), 10.0));
		CHECK(healthRegistry.Snapshot().at("issues").size() == 2);
	}

	// A mod report carries explicit ownership; opaque ids such as "host" cannot
	// be distinguished from subsystem names by punctuation.
	{
		HealthRegistry healthRegistry;
		auto reported = spec("host:broken", "host:broken", Severity::Warning, "host");
		reported.sourceKind = HealthRegistry::SourceKind::Mod;
		CHECK(healthRegistry.Upsert(reported, 1.0));
		CHECK(IssueById(healthRegistry.Snapshot(), "host:broken").value("sourceKind", "") == "mod");
	}

	// --- Resolve moves to session history; recurrence reactivates ----------
	{
		HealthRegistry healthRegistry;
		healthRegistry.Upsert(spec("host.ring-truncated", "host.ring-truncated", Severity::Warning, "host"), 2.0);
		CHECK(healthRegistry.IsActive("host.ring-truncated"));

		CHECK(healthRegistry.Resolve("host.ring-truncated", 5.0));
		CHECK(!healthRegistry.IsActive("host.ring-truncated"));
		// Resolving twice is a no-op, so producers may resolve unconditionally
		// every tick without generating a push each time.
		CHECK(!healthRegistry.Resolve("host.ring-truncated", 6.0));
		CHECK(!healthRegistry.Resolve("nothing.here", 6.0));

		auto resolved = IssueById(healthRegistry.Snapshot(), "host.ring-truncated");
		CHECK(resolved.value("status", "") == "resolved");
		CHECK(resolved.value("resolvedAt", -1.0) == 5.0);
		CHECK(resolved.value("occurrences", 0u) == 1u);
		// The record survives: "resolved this session" is the whole point.
		CHECK(healthRegistry.Snapshot().at("issues").size() == 1);

		// The same condition coming back reuses the record and keeps its count.
		CHECK(healthRegistry.Upsert(spec("host.ring-truncated", "host.ring-truncated", Severity::Warning, "host"), 8.0));
		auto again = IssueById(healthRegistry.Snapshot(), "host.ring-truncated");
		CHECK(again.value("status", "") == "active");
		CHECK(again.value("occurrences", 0u) == 2u);
		CHECK(again.value("firstAt", -1.0) == 2.0);
		CHECK(again.value("lastAt", -1.0) == 8.0);
		CHECK(!again.contains("resolvedAt"));
	}

	// --- ReplaceScope is idempotent and sweeps only its explicit owner ------
	{
		HealthRegistry healthRegistry;
		const std::array initial{
			spec("settings.values-parse:acme", "settings.values-parse", Severity::Warning, "settings", "acme"),
			spec("settings.schema-parse:beta", "settings.schema-parse", Severity::Error, "settings", "beta"),
		};
		CHECK(healthRegistry.ReplaceScope("settings-load", initial, 1.0));
		healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 1.0);

		// A reload fixed beta but not acme. The views issue belongs to another
		// producer and must not be swept by the settings reconcile.
		const std::array remaining{
			spec("settings.values-parse:acme", "settings.values-parse", Severity::Warning, "settings", "acme"),
		};
		CHECK(healthRegistry.ReplaceScope("settings-load", remaining, 7.0));
		CHECK(healthRegistry.IsActive("settings.values-parse:acme"));
		CHECK(!healthRegistry.IsActive("settings.schema-parse:beta"));
		CHECK(healthRegistry.IsActive("view.load-failed:acme/panel"));
		const auto unchanged = IssueById(healthRegistry.Snapshot(), "settings.values-parse:acme");
		CHECK(unchanged.value("occurrences", 0u) == 1u);
		CHECK(unchanged.value("lastAt", -1.0) == 1.0);
		// Idempotent: the same current set is not another occurrence or push.
		CHECK(!healthRegistry.ReplaceScope("settings-load", remaining, 8.0));
	}

	// --- Wire ordering: errors, then warnings, newest first, resolved last --
	{
		HealthRegistry healthRegistry;
		healthRegistry.Upsert(spec("w-old", "compat.needs-newer-osfui", Severity::Warning, "compat"), 1.0);
		healthRegistry.Upsert(spec("e-old", "view.load-failed", Severity::Error, "views"), 2.0);
		healthRegistry.Upsert(spec("w-new", "host.ring-truncated", Severity::Warning, "host"), 3.0);
		healthRegistry.Upsert(spec("e-new", "settings.schema-parse", Severity::Error, "settings"), 4.0);
		healthRegistry.Upsert(spec("r-done", "view.load-failed", Severity::Error, "views"), 5.0);
		healthRegistry.Resolve("r-done", 6.0);

		const auto ids = IssueIds(healthRegistry.Snapshot());
		CHECK(ids == std::vector<std::string>({ "e-new", "e-old", "w-new", "w-old", "r-done" }));
	}

	// --- Sanitizer: no absolute paths, no shell targets, bounded -----------
	{
		CHECK(HealthRegistry::RedactPath("acme.json") == "acme.json");
		CHECK(HealthRegistry::RedactPath(R"(C:\Users\someone\Documents\My Games\Starfield\acme.json)") == "acme.json");
		CHECK(HealthRegistry::RedactPath("/home/someone/.config/osfui/acme.json") == "acme.json");
		CHECK(HealthRegistry::RedactPath(R"(\\SERVER\share\mods\acme.json)") == "acme.json");
		CHECK(HealthRegistry::RedactPath("https://example.invalid/payload?x=1") == "payload?x=1");
		CHECK(HealthRegistry::RedactPath(R"(C:\Windows\System32\)") == "<path>");

		nlohmann::json context{
			{ "file", R"(C:\Modding\Starfield\Data\SFSE\Plugins\OSFUI\settings\acme.json)" },
			{ "line", 42 },
			{ "recovered", true },
			{ "nested", nlohmann::json{ { "drop", "me" } } },  // structured values are refused
			{ "list", nlohmann::json::array({ 1, 2 }) },
		};
		const auto clean = HealthRegistry::Sanitize(context);
		CHECK(clean.value("file", "") == "acme.json");
		CHECK(clean.value("line", 0) == 42);
		CHECK(clean.value("recovered", false));
		CHECK(!clean.contains("nested"));
		CHECK(!clean.contains("list"));

		// Long strings are truncated rather than dropped: the head of a parse
		// message is the actionable part.
		nlohmann::json longContext{ { "message", std::string(4000, 'x') } };
		const auto     truncated = HealthRegistry::Sanitize(longContext);
		CHECK(truncated.at("message").get<std::string>().size() <=
			HealthRegistry::kMaxContextValueChars + 4);  // + the ellipsis' UTF-8 bytes

		// A long value made of multi-byte codepoints must be cut on a boundary,
		// not mid-sequence. kMaxContextValueChars counts BYTES, so a CJK message
		// straddles it; a raw resize() left an incomplete sequence and every
		// later dump() of the snapshot threw type_error.316 — on Broadcast(),
		// which runs on the game thread with no handler above it.
		{
			std::string cjk;
			while (cjk.size() <= HealthRegistry::kMaxContextValueChars + 8) {
				cjk += "\xE4\xB8\xAD";  // U+4E2D, 3 bytes
			}
			const auto cut = HealthRegistry::Sanitize(
				nlohmann::json{ { "message", cjk } });
			const auto text = cut.at("message").get<std::string>();
			CHECK(text.size() <= HealthRegistry::kMaxContextValueChars + 4);
			// The head must survive intact and the whole payload must strictly dump.
			CHECK(text.starts_with("\xE4\xB8\xAD"));
			bool strictOk = true;
			try {
				(void)cut.dump();
			} catch (const std::exception&) {
				strictOk = false;
			}
			CHECK(strictOk);
		}

		// Key count is capped.
		nlohmann::json wide = nlohmann::json::object();
		for (int i = 0; i < 40; ++i) {
			wide["k" + std::to_string(i)] = i;
		}
		CHECK(HealthRegistry::Sanitize(wide).size() == HealthRegistry::kMaxContextEntries);

		// The same rule applies to the system-information block.
		HealthRegistry healthRegistry;
		CHECK(healthRegistry.SetSystemInfo(nlohmann::json{
			{ "renderer", "webview2" },
			{ "logFolder", R"(C:\Users\someone\Documents\My Games\Starfield\SFSE\Logs)" },
		}));
		CHECK(!healthRegistry.SetSystemInfo(nlohmann::json{
			{ "renderer", "webview2" },
			{ "logFolder", R"(C:\Users\someone\Documents\My Games\Starfield\SFSE\Logs)" },
		}));
		const auto system = healthRegistry.Snapshot().at("system");
		CHECK(system.value("renderer", "") == "webview2");
		CHECK(system.value("logFolder", "") == "Logs");
	}

	// --- An issue with no id or no code is refused -------------------------
	{
		HealthRegistry healthRegistry;
		CHECK(!healthRegistry.Upsert(spec("", "view.load-failed", Severity::Error, "views"), 1.0));
		CHECK(!healthRegistry.Upsert(spec("some.id", "", Severity::Error, "views"), 1.0));
		CHECK(healthRegistry.Snapshot().at("issues").empty());
	}

	// --- The registry as STATE: greeting replay + change pushes ------------
	{
		g_sent.clear();
		MessageBridge      bridge(Capture);
		HealthRegistry  healthRegistry;
		healthRegistry.AttachBridge(bridge);

		// `diagnostics.get` is gone as a NAME, not merely unused: a stale 1.x
		// view naming it must get `unknown-endpoint`, never a half-working read.
		CHECK(!bridge.HasRequest("diagnostics.get") && !bridge.HasSend("diagnostics.get"));

		// The OSF UI runtime's whole hello obligation for this key
		// (Runtime::OnViewGreeted): publish the CURRENT snapshot straight to the
		// greeting document — deliberately NOT through Broadcast(), for the
		// reason the regression block below pins down.
		bridge.SetHelloHook([&](std::string_view a_view) {
			bridge.PublishState(a_view, "osfui", "diagnostics", healthRegistry.Snapshot());
		});

		healthRegistry.SetSystemInfo(nlohmann::json{ { "version", "2.0.0" } });
		healthRegistry.Upsert(spec("settings.values-parse:acme", "settings.values-parse", Severity::Warning, "settings", "acme"), 1.0);

		// A view that exists but has not greeted receives nothing: state at an
		// ungreeted document is DROPPED, because its greeting replays every
		// current value anyway and a queued value could land after a newer one.
		bridge.OnViewCreated("osfui/settings");
		healthRegistry.Broadcast();
		CHECK(g_sent.empty());

		// The greeting is answered with `ready`, then the snapshot. That
		// ordering is structural, not a convention the call site remembers.
		Send(bridge, "osfui/settings", "osfui.hello");
		{
			const auto snapshots = StateTo("osfui/settings", "osfui", "diagnostics");
			CHECK(snapshots.size() == 1);
			CHECK(snapshots.back().payload.at("system").value("version", "") == "2.0.0");
			CHECK(snapshots.back().payload.at("issues").size() == 1);
			CHECK(g_sent.size() == 2 && g_sent[0].kind == "ready" && g_sent[1].kind == "state");
		}

		// An unchanged snapshot is not re-sent. Note the dedupe was armed by the
		// Broadcast() above, which delivered nothing at all — the content check
		// is on the SNAPSHOT, not on what any view received.
		healthRegistry.Broadcast();
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").size() == 1);

		// A change reaches every greeted view, and only greeted views.
		Greet(bridge, "acme/panel");
		CHECK(StateTo("acme/panel", "osfui", "diagnostics").size() == 1);  // its own replay
		healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 3.0);
		healthRegistry.Broadcast();
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").size() == 2);
		CHECK(StateTo("acme/panel", "osfui", "diagnostics").size() == 2);
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").back().payload.at("issues").size() == 2);

		// Resolving is a change too — the card has to move to history live.
		healthRegistry.Resolve("view.load-failed:acme/panel", 5.0);
		healthRegistry.Broadcast();
		auto latest = StateTo("osfui/settings", "osfui", "diagnostics").back().payload;
		CHECK(IssueById(latest, "view.load-failed:acme/panel").value("status", "") == "resolved");

		// A destroyed view stops receiving pushes; the survivor keeps them. The
		// gate the bridge drops is the subscription, so the registry owns no
		// per-view lifecycle state.
		const auto before = StateTo("acme/panel", "osfui", "diagnostics").size();
		bridge.OnViewDestroyed("acme/panel");
		healthRegistry.Upsert(spec("host.ring-truncated", "host.ring-truncated", Severity::Warning, "host"), 7.0);
		healthRegistry.Broadcast();
		CHECK(StateTo("acme/panel", "osfui", "diagnostics").size() == before);
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").size() == 4);

		// A bridge teardown drops the retained pointer; nothing dangles.
		healthRegistry.DetachBridge();
		const auto sealed = g_sent.size();
		healthRegistry.Upsert(spec("settings.schema-parse:late.mod", "settings.schema-parse", Severity::Error, "settings"), 9.0);
		healthRegistry.Broadcast();
		CHECK(g_sent.size() == sealed);
	}

	// --- REGRESSION: the hello replay must BYPASS the content dedupe -------
	// Broadcast() suppresses an unchanged snapshot, so producers can call it
	// unconditionally after any potential change. A SECOND document that greets
	// later has never been sent anything — and the registry it needs is, by
	// definition, unchanged since the first one connected. Routing the replay
	// through Broadcast() would therefore match _lastSent and hand that document
	// an empty System Health destination for the rest of the session. Runtime::OnViewGreeted
	// publishes Snapshot() directly for exactly this reason; this test fails if
	// anyone ever "simplifies" it back into Broadcast().
	{
		g_sent.clear();
		MessageBridge     bridge(Capture);
		HealthRegistry healthRegistry;
		healthRegistry.AttachBridge(bridge);
		bridge.SetHelloHook([&](std::string_view a_view) {
			bridge.PublishState(a_view, "osfui", "diagnostics", healthRegistry.Snapshot());
		});

		healthRegistry.Upsert(spec("host.ring-truncated", "host.ring-truncated", Severity::Warning, "host"), 1.0);
		// Arms _lastSent with the current snapshot while no view has greeted, so
		// the dedupe is live for everything that follows.
		healthRegistry.Broadcast();
		CHECK(g_sent.empty());

		Greet(bridge, "osfui/settings");
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").size() == 1);

		// The second document, with the registry byte-identical to what the
		// dedupe holds.
		Greet(bridge, "acme/panel");
		{
			const auto replay = StateTo("acme/panel", "osfui", "diagnostics");
			CHECK(replay.size() == 1);
			CHECK(replay.size() == 1 && replay[0].payload.at("issues").size() == 1);
			CHECK(KindTo("acme/panel", "ready").size() == 1);
		}

		// The proof that the replay is a SEPARATE path and not a lucky
		// Broadcast(): one right now still sends nothing to anybody.
		const auto sealed = g_sent.size();
		healthRegistry.Broadcast();
		CHECK(g_sent.size() == sealed);

		// And the replay left the dedupe's bookkeeping alone: a real change
		// afterwards still fans out to both documents.
		healthRegistry.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 4.0);
		healthRegistry.Broadcast();
		CHECK(StateTo("osfui/settings", "osfui", "diagnostics").size() == 2);
		CHECK(StateTo("acme/panel", "osfui", "diagnostics").size() == 2);
	}

	std::fprintf(stderr, "health_registry_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
