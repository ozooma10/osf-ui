// Host-side unit tests for the session diagnostics registry (bridge protocol
// 1.4): the REAL DiagnosticsModule + MessageBridge driven through actual
// `ui.command` envelopes, with a capturing SendFn standing in for the renderer.
// Covers dedupe/occurrence counting, resolve/reactivate history, the
// subscribe-on-read snapshot and change pushes, wire ordering, and the payload
// sanitizer (no absolute paths, no shell targets, bounded size).
// Assert-style; process exit code is the failure count.

#include "runtime/DiagnosticsModule.h"
#include "runtime/MessageBridge.h"

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

	struct Sent
	{
		std::string    view;
		std::string    type;
		nlohmann::json payload;
	};

	std::vector<Sent> g_sent;

	std::vector<Sent> SentTo(std::string_view a_view, std::string_view a_type)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.type == a_type) {
				out.push_back(s);
			}
		}
		return out;
	}

	void Command(OSFUI::MessageBridge& a_bridge, std::string_view a_view, nlohmann::json a_payload)
	{
		const nlohmann::json envelope = { { "type", "ui.command" }, { "payload", std::move(a_payload) } };
		a_bridge.HandleWebMessage(a_view, envelope.dump());
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
	using namespace OSFUI;
	using Severity = DiagnosticsModule::Severity;

	const auto spec = [](std::string a_id, std::string a_code, Severity a_severity,
						  std::string a_source, std::string a_subject = {},
						  nlohmann::json a_context = nlohmann::json::object()) {
		return DiagnosticsModule::IssueSpec{
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
		DiagnosticsModule diag;
		CHECK(diag.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 1.0));
		CHECK(diag.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 4.0));
		CHECK(diag.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 9.0));

		const auto snapshot = diag.Snapshot();
		CHECK(snapshot.at("issues").size() == 1);
		const auto issue = IssueById(snapshot, "view.load-failed:acme/panel");
		CHECK(issue.value("occurrences", 0u) == 3u);
		CHECK(issue.value("firstAt", -1.0) == 1.0);   // the first sighting is the anchor
		CHECK(issue.value("lastAt", -1.0) == 9.0);
		CHECK(issue.value("status", "") == "active");
		CHECK(issue.value("severity", "") == "error");
		CHECK(issue.value("source", "") == "views");
		CHECK(issue.value("subject", "") == "acme/panel");
		CHECK(!issue.contains("resolvedAt"));

		// A different id is a different condition, even with the same code.
		CHECK(diag.Upsert(spec("view.load-failed:acme/hud", "view.load-failed", Severity::Error, "views", "acme/hud"), 10.0));
		CHECK(diag.Snapshot().at("issues").size() == 2);
	}

	// --- Resolve moves to session history; recurrence reactivates ----------
	{
		DiagnosticsModule diag;
		diag.Upsert(spec("host.focus-stranded", "host.focus-stranded", Severity::Warning, "host"), 2.0);
		CHECK(diag.IsActive("host.focus-stranded"));

		CHECK(diag.Resolve("host.focus-stranded", 5.0));
		CHECK(!diag.IsActive("host.focus-stranded"));
		// Resolving twice is a no-op, so producers may resolve unconditionally
		// every tick without generating a push each time.
		CHECK(!diag.Resolve("host.focus-stranded", 6.0));
		CHECK(!diag.Resolve("nothing.here", 6.0));

		auto resolved = IssueById(diag.Snapshot(), "host.focus-stranded");
		CHECK(resolved.value("status", "") == "resolved");
		CHECK(resolved.value("resolvedAt", -1.0) == 5.0);
		CHECK(resolved.value("occurrences", 0u) == 1u);
		// The record survives: "resolved this session" is the whole point.
		CHECK(diag.Snapshot().at("issues").size() == 1);

		// The same condition coming back reuses the record and keeps its count.
		CHECK(diag.Upsert(spec("host.focus-stranded", "host.focus-stranded", Severity::Warning, "host"), 8.0));
		auto again = IssueById(diag.Snapshot(), "host.focus-stranded");
		CHECK(again.value("status", "") == "active");
		CHECK(again.value("occurrences", 0u) == 2u);
		CHECK(again.value("firstAt", -1.0) == 2.0);
		CHECK(again.value("lastAt", -1.0) == 8.0);
		CHECK(!again.contains("resolvedAt"));
	}

	// --- ResolveMissing sweeps one source against a recomputed set ---------
	{
		DiagnosticsModule diag;
		diag.Upsert(spec("settings.values-parse:acme", "settings.values-parse", Severity::Warning, "settings", "acme"), 1.0);
		diag.Upsert(spec("settings.schema-parse:beta", "settings.schema-parse", Severity::Error, "settings", "beta"), 1.0);
		diag.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 1.0);

		// A reload fixed beta but not acme. The views issue belongs to another
		// producer and must not be swept by the settings reconcile.
		CHECK(diag.ResolveMissing("settings", { "settings.values-parse:acme" }, 7.0));
		CHECK(diag.IsActive("settings.values-parse:acme"));
		CHECK(!diag.IsActive("settings.schema-parse:beta"));
		CHECK(diag.IsActive("view.load-failed:acme/panel"));
		// Idempotent: nothing left to sweep.
		CHECK(!diag.ResolveMissing("settings", { "settings.values-parse:acme" }, 8.0));
	}

	// --- Wire ordering: errors, then warnings, newest first, resolved last --
	{
		DiagnosticsModule diag;
		diag.Upsert(spec("w-old", "compat.needs-newer-osfui", Severity::Warning, "compat"), 1.0);
		diag.Upsert(spec("e-old", "view.load-failed", Severity::Error, "views"), 2.0);
		diag.Upsert(spec("w-new", "host.focus-stranded", Severity::Warning, "host"), 3.0);
		diag.Upsert(spec("e-new", "settings.schema-parse", Severity::Error, "settings"), 4.0);
		diag.Upsert(spec("r-done", "view.load-failed", Severity::Error, "views"), 5.0);
		diag.Resolve("r-done", 6.0);

		const auto ids = IssueIds(diag.Snapshot());
		CHECK(ids == std::vector<std::string>({ "e-new", "e-old", "w-new", "w-old", "r-done" }));
	}

	// --- Sanitizer: no absolute paths, no shell targets, bounded -----------
	{
		CHECK(DiagnosticsModule::RedactPath("acme.json") == "acme.json");
		CHECK(DiagnosticsModule::RedactPath(R"(C:\Users\someone\Documents\My Games\Starfield\acme.json)") == "acme.json");
		CHECK(DiagnosticsModule::RedactPath("/home/someone/.config/osfui/acme.json") == "acme.json");
		CHECK(DiagnosticsModule::RedactPath(R"(\\SERVER\share\mods\acme.json)") == "acme.json");
		CHECK(DiagnosticsModule::RedactPath("https://example.invalid/payload?x=1") == "payload?x=1");
		CHECK(DiagnosticsModule::RedactPath(R"(C:\Windows\System32\)") == "<path>");

		nlohmann::json context{
			{ "file", R"(C:\Modding\Starfield\Data\SFSE\Plugins\OSFUI\settings\acme.json)" },
			{ "line", 42 },
			{ "recovered", true },
			{ "nested", nlohmann::json{ { "drop", "me" } } },  // structured values are refused
			{ "list", nlohmann::json::array({ 1, 2 }) },
		};
		const auto clean = DiagnosticsModule::Sanitize(context);
		CHECK(clean.value("file", "") == "acme.json");
		CHECK(clean.value("line", 0) == 42);
		CHECK(clean.value("recovered", false));
		CHECK(!clean.contains("nested"));
		CHECK(!clean.contains("list"));

		// Long strings are truncated rather than dropped: the head of a parse
		// message is the actionable part.
		nlohmann::json longContext{ { "message", std::string(4000, 'x') } };
		const auto     truncated = DiagnosticsModule::Sanitize(longContext);
		CHECK(truncated.at("message").get<std::string>().size() <=
			DiagnosticsModule::kMaxContextValueChars + 4);  // + the ellipsis' UTF-8 bytes

		// Key count is capped.
		nlohmann::json wide = nlohmann::json::object();
		for (int i = 0; i < 40; ++i) {
			wide["k" + std::to_string(i)] = i;
		}
		CHECK(DiagnosticsModule::Sanitize(wide).size() == DiagnosticsModule::kMaxContextEntries);

		// The same rule applies to the system-information block.
		DiagnosticsModule diag;
		diag.SetSystemInfo(nlohmann::json{
			{ "renderer", "webview2" },
			{ "logFolder", R"(C:\Users\someone\Documents\My Games\Starfield\SFSE\Logs)" },
		});
		const auto system = diag.Snapshot().at("system");
		CHECK(system.value("renderer", "") == "webview2");
		CHECK(system.value("logFolder", "") == "Logs");
	}

	// --- An issue with no id or no code is refused -------------------------
	{
		DiagnosticsModule diag;
		CHECK(!diag.Upsert(spec("", "view.load-failed", Severity::Error, "views"), 1.0));
		CHECK(!diag.Upsert(spec("some.id", "", Severity::Error, "views"), 1.0));
		CHECK(diag.Snapshot().at("issues").empty());
	}

	// --- Subscriber snapshot + change pushes over the real bridge ----------
	{
		g_sent.clear();
		MessageBridge bridge([](std::string_view a_view, std::string_view a_json) {
			const auto parsed = nlohmann::json::parse(a_json);
			g_sent.push_back(Sent{
				std::string(a_view),
				parsed.value("type", ""),
				parsed.value("payload", nlohmann::json::object()),
			});
		});

		DiagnosticsModule diag;
		diag.RegisterCommands(bridge);
		diag.SetSystemInfo(nlohmann::json{ { "version", "1.4.0" } });
		diag.Upsert(spec("settings.values-parse:acme", "settings.values-parse", Severity::Warning, "settings", "acme"), 1.0);

		// Nothing is pushed to a view that never asked.
		diag.Broadcast();
		CHECK(g_sent.empty());

		// diagnostics.get replies with the snapshot AND subscribes the caller.
		Command(bridge, "osfui/settings", nlohmann::json{ { "command", "diagnostics.get" } });
		auto replies = SentTo("osfui/settings", "diagnostics.data");
		CHECK(replies.size() == 1);
		CHECK(replies.back().payload.at("system").value("version", "") == "1.4.0");
		CHECK(replies.back().payload.at("issues").size() == 1);

		// An unchanged snapshot is not re-sent (the reply seeded the dedupe).
		diag.Broadcast();
		CHECK(SentTo("osfui/settings", "diagnostics.data").size() == 1);

		// A change reaches every subscriber, and only subscribers.
		Command(bridge, "acme/panel", nlohmann::json{ { "command", "diagnostics.get" } });
		diag.Upsert(spec("view.load-failed:acme/panel", "view.load-failed", Severity::Error, "views", "acme/panel"), 3.0);
		diag.Broadcast();
		CHECK(SentTo("osfui/settings", "diagnostics.data").size() == 2);
		CHECK(SentTo("acme/panel", "diagnostics.data").size() == 2);
		CHECK(SentTo("osfui/settings", "diagnostics.data").back().payload.at("issues").size() == 2);

		// Resolving is a change too — the card has to move to history live.
		diag.Resolve("view.load-failed:acme/panel", 5.0);
		diag.Broadcast();
		auto latest = SentTo("osfui/settings", "diagnostics.data").back().payload;
		CHECK(IssueById(latest, "view.load-failed:acme/panel").value("status", "") == "resolved");

		// A destroyed view stops receiving pushes; the survivor keeps them.
		const auto before = SentTo("acme/panel", "diagnostics.data").size();
		diag.OnViewDestroyed("acme/panel");
		diag.Upsert(spec("host.focus-stranded", "host.focus-stranded", Severity::Warning, "host"), 7.0);
		diag.Broadcast();
		CHECK(SentTo("acme/panel", "diagnostics.data").size() == before);
		CHECK(SentTo("osfui/settings", "diagnostics.data").size() == 4);

		// A bridge teardown drops every subscriber and the retained pointer.
		diag.OnBridgeDown();
		const auto sealed = g_sent.size();
		diag.Upsert(spec("render.framegen-fallback", "render.framegen-fallback", Severity::Warning, "render"), 9.0);
		diag.Broadcast();
		CHECK(g_sent.size() == sealed);
	}

	std::fprintf(stderr, "diagnostics_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
