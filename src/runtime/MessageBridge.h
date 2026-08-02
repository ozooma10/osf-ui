#pragma once

#include <chrono>
#include <deque>
#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

// Narrow native <-> web bridge (protocol 2.0, docs/mod-api-2.0-design.md).
//
// Four verbs, chosen by semantics, with exactly one envelope shape each:
//
//   web -> native   { kind: "send",    name, payload }
//                   { kind: "request", name, id, payload }
//
//   native -> web   { kind: "ready",   payload }
//                   { kind: "state",   mod, key, value }
//                   { kind: "event",   name, payload }
//                   { kind: "reply",   id, payload }
//                   { kind: "error",   id, payload: { code, message } }
//
// Routing metadata (kind/name/id/mod/key) lives BESIDE the payload, never
// inside it, so a payload field can never override routing — the 1.x wire put
// `command` inside the payload and the helper built it with Object.assign.
//
// The registry is explicit and split by kind: `RegisterSend` for pure
// notifications, `RegisterRequest` for endpoints that settle exactly once.
// Kind enforcement is structural: a request naming a send endpoint answers
// `wrong-endpoint-kind`; a send naming a request endpoint is dropped and
// surfaced (Surface(), below). The only auto-ack is the deliberately narrow
// native ABI 1.x RegisterCommand compatibility path; strict platform and
// RegisterRequest endpoints never infer success. There is no generic "call
// native function" escape hatch.
// See docs/security-model.md.
//
// The handshake is PAGE-INITIATED and is the only boot path: a fresh document
// sends `osfui.hello`, and the bridge answers `ready`, replays state through
// the hook Runtime installs, then opens that view's event gate. First open,
// F5, dev hot reload and crash-recovery reload are all the same sequence, so
// nothing here has to guess whether a greeting was consumed.

namespace OSFUI
{
	class MessageBridge
	{
	public:
		// Transport for native -> web text: deliver `a_json` to view `a_viewId`.
		using SendFn = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

		// Handler for one `send` endpoint: a pure notification. Nothing is
		// settled and nothing is echoed — wanting an outcome means it is a
		// request.
		using SendHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		// Handler for one `request` endpoint. MUST settle exactly once, either
		// synchronously (Respond/Reject) or by taking ownership of the
		// correlation id with Defer() and settling later through
		// RespondTo/RejectTo. A handler that returns having done neither is a
		// platform bug and answers `internal`.
		using RequestHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		// Called when a document greets the bridge, after `ready` has been sent
		// and before that view's event gate opens. Runtime installs this to
		// replay platform state keys plus the owning mod's retained state; the
		// ordering guarantee (ready < state < events) is therefore structural
		// rather than a convention call sites have to remember.
		using HelloHook = std::function<void(std::string_view a_viewId)>;

		// Host-detected faults, routed back to the view as a dev-only
		// `osfui.debug.error` event. Runtime installs this; without it the
		// bridge only logs. `a_viewFault` says whether the VIEW caused it: only
		// those are counted for the release-mode `view.protocol-misuse`
		// diagnostic. A backend that never answered is reported to the waiting
		// page too, but it is not the page's fault and must not earn it a
		// health card.
		using SurfaceFn = std::function<void(std::string_view a_viewId, std::string_view a_code,
			std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault)>;

		explicit MessageBridge(SendFn a_send);

		// ---- endpoint registry -------------------------------------------
		// Register (or replace) the handler for an exact endpoint name. Strict
		// send/request names are disjoint. A compatibility command accepts both
		// verbs so ABI 1.0-1.7 RegisterCommand callers keep their established
		// injected-requestId + auto-ack behavior.
		void RegisterSend(std::string a_name, SendHandler a_handler);
		bool RegisterRequest(std::string a_name, RequestHandler a_handler);
		void RegisterCompatCommand(std::string a_name, SendHandler a_handler);

		// No-ops if absent. Used by the native plugin API (src/api) for hot
		// cleanup / re-sync.
		void UnregisterSend(std::string_view a_name);
		void UnregisterRequest(std::string_view a_name);
		void UnregisterCompatCommand(std::string_view a_name);

		[[nodiscard]] bool HasSend(std::string_view a_name) const;
		[[nodiscard]] bool HasRequest(std::string_view a_name) const;
		[[nodiscard]] bool HasCompatCommand(std::string_view a_name) const;

		// ---- inbound ------------------------------------------------------
		// Entry point for web -> native messages (raw JSON text) from a given
		// source view. Malformed or non-whitelisted input is rejected, logged
		// and surfaced, never fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// ---- settlement (request handlers) --------------------------------
		// Answer the in-flight request with `a_payload`. Ignored (and warned)
		// outside a request handler or after settlement.
		void Respond(const nlohmann::json& a_payload);
		// Fail the in-flight request with a stable machine `a_code`
		// ("unknown-view", "capture-busy", ...) and a human sentence.
		void Reject(std::string_view a_code, std::string_view a_message = {});
		// Take ownership of the correlation id: nothing is sent now, and the
		// handler settles later via RespondTo/RejectTo with the returned token.
		// Deferred requests are tracked, bounded, deadline-swept
		// (`no-response`) and reaped when their view goes away.
		//
		// The token is minted HERE and is unique process-wide; it is not the
		// page's correlation id. Pages number their own requests from a
		// per-document counter, so every view's first request is "q1" — keying
		// deferrals by that id alone let one document's reply settle another's
		// promise. The page id survives on the Pending entry as the wire echo.
		// Returns "" when called outside an unsettled request (nothing to
		// settle later).
		[[nodiscard]] std::string Defer();

		// Settle a deferred request by the token Defer() returned. A stale
		// token (already settled, expired or reaped with its view) is ignored —
		// late and duplicate replies are never delivered twice.
		void RespondTo(std::string_view a_token, const nlohmann::json& a_payload);
		void RespondJsonTo(std::string_view a_token, std::string_view a_payloadJson);
		void RejectTo(std::string_view a_token, std::string_view a_code, std::string_view a_message = {});

		// ---- outbound pushes ----------------------------------------------
		// One-shot happening, delivered at most once and never replayed. Held
		// in a bounded per-view queue until that view greets the bridge, which
		// is what preserves the native ABI's message-before-first-paint
		// guarantee now that the handshake is page-initiated.
		void Emit(std::string_view a_viewId, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(std::string_view a_viewId, std::string_view a_name, std::string_view a_payloadJson);
		void Emit(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, std::string_view a_payloadJson);

		// Named value, latest-wins, complete per key. Delivered only to views
		// that have greeted the bridge: a document that has not yet said hello
		// receives every current value through the replay instead, so there is
		// nothing to queue and no ordering to get wrong.
		void PublishState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			const nlohmann::json& a_value);
		void PublishJsonState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			std::string_view a_valueJson);
		void PublishState(const std::unordered_set<std::string>& a_viewIds, std::string_view a_mod,
			std::string_view a_key, const nlohmann::json& a_value);

		// Every view that has greeted the bridge. Platform state and platform
		// events go to all of them: the 1.x subscriber sets existed only because
		// a read had to double as a subscribe, and each one was a thing to
		// prune, to clear on bridge rebuild, and to forget on reload.
		void PublishStateAll(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);
		void EmitAll(std::string_view a_name, const nlohmann::json& a_payload);

		// ---- view lifecycle ------------------------------------------------
		// Arm a closed event gate for a freshly created (or reloaded) view.
		// Events emitted between here and that document's hello are queued.
		void OnViewCreated(std::string_view a_viewId);
		// Drop the gate, its queued events, and every deferred request the view
		// still owns.
		void OnViewDestroyed(std::string_view a_viewId);
		// True once the view's document has greeted the bridge.
		[[nodiscard]] bool HasGreeted(std::string_view a_viewId) const;

		void SetHelloHook(HelloHook a_hook) { _onHello = std::move(a_hook); }
		void SetSurfaceFn(SurfaceFn a_fn) { _surface = std::move(a_fn); }

		// Main thread, once per tick: expire deferred requests past the host
		// deadline with `no-response`.
		void Tick(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

		// ---- in-flight context ---------------------------------------------
		// Source view of the in-flight message (empty when none). Lets an
		// endpoint default to its caller, e.g. a view hiding itself without
		// knowing its own id.
		[[nodiscard]] std::string_view CurrentSource() const { return _currentSource; }
		// Correlation id of the in-flight request ("" when a send is in flight
		// or nothing is).
		[[nodiscard]] std::string_view CurrentRequestId() const { return _currentRequestId; }
		// Endpoint name of the in-flight message.
		[[nodiscard]] std::string_view CurrentName() const { return _currentName; }

		// Report a host-detected fault to a view (errors the page would
		// otherwise never hear about). Public so the API layer can route
		// plugin-side faults through the same sink. Pass a_viewFault = false
		// when the view did nothing wrong (a backend missed its deadline), so
		// the report reaches the page without counting against it.
		void Surface(std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_detail = nlohmann::json::object(), bool a_viewFault = true);

	private:
		struct Pending
		{
			std::string                           view;
			std::string                           requestId;  // the PAGE's id, echoed on the wire
			std::string                           name;
			std::chrono::steady_clock::time_point deadline;
		};
		struct Gate
		{
			// Two flags, not one, because state and events open at different
			// moments: state must flow DURING the replay, events only after it,
			// so that an event raised by a replay listener cannot overtake the
			// backlog queued before the document greeted us.
			bool                    greeted{ false };     // state may flow
			bool                    eventsOpen{ false };  // events may flow
			std::deque<std::string> queued;               // encoded event envelopes
		};

		[[nodiscard]] static std::string EncodeEvent(std::string_view a_name, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeState(std::string_view a_mod, std::string_view a_key, std::string_view a_valueJson);
		[[nodiscard]] static std::string EncodeReply(std::string_view a_requestId, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeError(std::string_view a_requestId, std::string_view a_code, std::string_view a_message);

		void HandleHello(std::string_view a_viewId);
		void DispatchSend(const std::string& a_name, const nlohmann::json& a_payload);
		void DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload);
		void SendReady(std::string_view a_viewId);
		void DeliverEvent(std::string_view a_viewId, const std::string& a_encoded, std::string_view a_name);
		// Folds a settlement to the in-flight caller into that message's single
		// completion trace instead of logging it as its own line.
		void NoteTracedReply(std::string_view a_what);

		SendFn                                            _send;
		std::unordered_map<std::string, SendHandler>      _sends;
		std::unordered_map<std::string, RequestHandler>   _requests;
		std::unordered_map<std::string, SendHandler>      _compatCommands;
		std::unordered_map<std::string, Gate>             _gates;    // view id -> event gate
		std::unordered_map<std::string, Pending>          _pending;  // host token -> deferred request
		std::uint64_t                                     _nextDeferToken{ 1 };
		HelloHook                                         _onHello;
		SurfaceFn                                         _surface;

		std::string _currentSource;     // source view of the in-flight message (reply target)
		std::string _currentRequestId;  // correlation id of the in-flight request ("" = none)
		std::string _currentName;       // endpoint name of the in-flight message
		bool        _settled{ false };  // the in-flight request was answered, rejected or deferred
		bool        _inMessage{ false };  // inside HandleWebMessage dispatch (arms trace folding)
		std::string _trace;               // what went back while _inMessage

		std::unordered_set<std::string> _warnedUnknownEndpoints;  // warn-once-per-name log dedupe
	};
}
