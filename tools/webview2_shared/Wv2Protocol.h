#pragma once

// Wire protocol between the OSF UI plugin (or the standalone POC client) and
// the browser-host executable, osfui_webview2_host.exe. Both sides compile this header; the transport is one
// message-framed named pipe (Wv2Pipe.h) carrying UTF-8 JSON.
//
// Framing: [u32 little-endian payload byte count][payload]. Each payload is one
// JSON object with a "type" field. Unknown types and unknown fields must be
// ignored, for forward compatibility.
//
// The game side is the pipe server (owns the pipe name + ACL, launches the
// browser host); the browser host is the client. The browser host exits when the pipe breaks or the
// game process handle signals.

#include <cstdint>

namespace osfui::wv2
{
	// Bumped on any incompatible wire change. The browser host announces this in its
	// hello; the GAME side (WebView2HostWebRenderer) is the only validator and
	// abandons a mismatched browser host (both binaries ship together, so a mismatch
	// means a stale mirrored exe — the launcher versions the mirror dir to
	// avoid it).
	// v2: multi-view — per-view `view` routing on game->browser-host view messages and
	// `view` tagging on browser-host->game page events.
	// v3: presentation epochs — every closed->open transition gets a new epoch;
	// the browser host stamps captured frames only after the requested view is actually
	// revealed. The game rejects frames from an earlier/hidden presentation.
	// v4: verified named-pipe peers, bounded hello, and browser-host heartbeats make a
	// connected-but-stalled or impersonating browser host a terminal recoverable failure.
	// v5: game-directed, best-effort suspension for idle hidden views.
	// v6: physical key identity — `accelerator` messages carry `scan` (DIK
	// convention, input/ScanCode.h) alongside `vk`, and accelState's
	// toggleVk/captureUpVk become toggleScan/captureUpScan.
	inline constexpr std::uint32_t kBrowserHostProtocolVersion = 6;

	inline constexpr std::uint32_t kHelloTimeoutMs = 10000;
	inline constexpr std::uint32_t kHeartbeatIntervalMs = 1000;
	inline constexpr std::uint32_t kHeartbeatTimeoutMs = 10000;

	// Pipe name pattern: \\.\pipe\osfui-wv2-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-wv2-";

	// Window message posted to the game's top-level window to hand keyboard
	// focus back to the game (its WndProc subclass answers with SetFocus).
	// Game side: OverlayInputHook::kRestoreGameFocusMessage — the renderer
	// static_asserts the two stay equal. The browser host posts it from GotFocus when
	// Chromium grabs focus outside an input-capturing menu session.
	inline constexpr std::uint32_t kRestoreGameFocusMessage = 0x8049;

	// Hard cap on one framed message (nothing legitimate approaches this;
	// protects both sides from a corrupt length prefix).
	inline constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;

	// Shared-texture ring depth. 4 slots: one being written, one in flight,
	// one being composited, plus one spare so a single slow game present does
	// not stall the capture thread on the consume fence. The consumer sizes
	// itself from the `textures` message's slots array (up to its capacity),
	// so this is browser-host tuning, not a wire-protocol change.
	inline constexpr std::uint32_t kRingSlots = 4;

	// Fallback for `navigate.logicalHeight` (the view manifest's authoring
	// height) when a client omits it. Mirrors kDefaultViewHeight plugin-side.
	inline constexpr std::uint32_t kDefaultLogicalHeight = 900;

	// Multi-view (v2): the browser host keeps one composition controller + child
	// ContainerVisual per OSF UI view under a single captured root visual, so
	// one WGC capture / shared-texture ring carries the already-composited
	// stack. `navigate`'s `id` is the view id — the first navigate for an
	// unknown id creates that view. View-scoped game->browser-host messages carry
	// `view:str`; absent or unknown, they fall back to the input-target view (keeps
	// the single-view POC client working). Page events browser-host->game are tagged
	// with their source `view`.
	//
	// THE MESSAGE SHAPES LIVE IN Wv2Messages.h, as structs both binaries
	// compile — not in a comment here. They used to be described in prose at
	// this spot, and the prose had drifted: it omitted `navigate.legacyApi` and
	// `init.hidden`, both of which had been on the wire for releases. A struct
	// cannot drift from the code that fills it.
	//
	// This header stays dependency-free (<cstdint> only) because Wv2Pipe.cpp and
	// the wv2-pipe-tests target include it and have no nlohmann; Wv2Messages.h
	// is where the JSON dependency is allowed.
}
