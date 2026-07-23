#pragma once

// Wire protocol between the OSF UI plugin (or the standalone POC client) and
// osfui_webview2_host.exe. Both sides compile this header; the transport is one
// message-framed named pipe (Wv2Pipe.h) carrying UTF-8 JSON.
//
// Framing: [u32 little-endian payload byte count][payload]. Each payload is one
// JSON object with a "type" field. Unknown types and unknown fields must be
// ignored, for forward compatibility.
//
// The game side is the pipe server (owns the pipe name + ACL, launches the
// host); the host is the client. The host exits when the pipe breaks or the
// game process handle signals.

#include <cstdint>

namespace osfui::wv2
{
	// Bumped on any incompatible wire change. The host refuses a mismatched
	// client hello and exits (both binaries ship together, so a mismatch means
	// a stale mirrored exe — the launcher versions the mirror dir to avoid it).
	// v2: multi-view — per-view `view` routing on game->host view messages and
	// `view` tagging on host->game page events.
	inline constexpr std::uint32_t kProtocolVersion = 2;

	// Pipe name pattern: \\.\pipe\osfui-wv2-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-wv2-";

	// Window message posted to the game's top-level window to hand keyboard
	// focus back to the game (its WndProc subclass answers with SetFocus).
	// Game side: OverlayInputHook::kRestoreGameFocusMessage — the renderer
	// static_asserts the two stay equal. The host posts it from GotFocus when
	// Chromium grabs focus outside an interactive-menu session.
	inline constexpr std::uint32_t kRestoreGameFocusMessage = 0x8049;

	// Hard cap on one framed message (a resize-storm of eval results should
	// never approach this; protects both sides from a corrupt length prefix).
	inline constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;

	// Shared-texture ring depth. 4 slots: one being written, one in flight,
	// one being composited, plus one spare so a single slow game present does
	// not stall the capture thread on the consume fence. The consumer sizes
	// itself from the `textures` message's slots array (up to its capacity),
	// so this is host-side tuning, not a wire-protocol change.
	inline constexpr std::uint32_t kRingSlots = 4;

	// Fallback for `navigate.logicalHeight` (the view manifest's authoring
	// height) when a client omits it. Mirrors kDefaultViewHeight plugin-side.
	inline constexpr std::uint32_t kDefaultLogicalHeight = 900;

	// Multi-view (v2): the host keeps one composition controller + child
	// ContainerVisual per OSF UI view under a single captured root visual, so
	// one WGC capture / shared-texture ring carries the already-composited
	// stack. `navigate`'s `id` is the view id — the first navigate for an
	// unknown id creates that view. View-scoped game->host messages carry
	// `view:str`; absent or unknown, they fall back to the active view (keeps
	// the single-view POC client working). Page events host->game are tagged
	// with their source `view`.
	//
	// Message types, game -> host:
	// init          { topLevelHwnd:u64, viewsPath:str, virtualHost:str,
	//                 width:u32, height:u32, userDataDir:str, devMode:bool,
	//                 adapterLuidLow:u32, adapterLuidHigh:u32 }
	//               (adapter LUID is the game's D3D12 device; the host creates
	//                its D3D11 capture device on the same physical adapter)
	// navigate      { id:str, entry:str, bridge:bool, logicalHeight:u32 }
	//               (creates view `id` on first sight; logicalHeight is the
	//                manifest's authoring height and drives the view's
	//                rasterization scale = outputHeight/logicalHeight, so the
	//                page lays out at logical size and CSS px scale up to
	//                output pixels. Optional — omitted means kDefaultLogicalHeight.)
	// resize        { width:u32, height:u32 }    (global: every view renders output-sized)
	// prewarm       { view:str }                 (one hidden paint, then suspend again)
	// setHidden     { view:str, hidden:bool }    (child-visual visibility + Chromium suspend)
	// setOrder      { view:str, order:i32 }      (composite z: lower beneath, ties by creation)
	// setActive     { view:str }                 (mouse/focus/synthetic-key target)
	// focus         { focused:bool }             (moves real focus into the active view)
	// mouse         { kind:"move"|"button"|"wheel"|"physicalWheel", x:i32, y:i32,
	//                 button:i32, down:bool, wheel:i32 }   (active view)
	// key           { vk:u32, down:bool }        (synthetic tap into the active view's widget)
	// postWeb       { view:str, json:str }
	// eval          { view:str, id:u64, script:str }
	// accelState    { toggleVk:u32, devReloadVk:u32, captured:bool,
	//                 captureArmed:bool, captureUpVk:u32 }
	// destroyView   { view:str }
	// shutdown      { }
	// frameAck      { serial:u64 }             (consumer acked this frame serial; releases its ring slot)
	// setRenderStats { view:str, enabled:bool } (toggle the render-stats overlay + sampling for a view)
	// renderStatsSample { presentFps, drawFps, freshFps, submitFps, sourceToDrawMs, recordCpuMs, ... }
	//               (periodic render-stats telemetry, forwarded to the views' overlay)
	//
	// Message types, host -> game:
	// hello         { protocolVersion:u32, hostVersion:str, runtimeVersion:str, pid:u32 }
	// ready         { }                          (first controller + capture up)
	// textures      { width:u32, height:u32, slots:[u64...],
	//                 produceFence:u64, consumeFence:u64, keyedMutex:bool,
	//                 adapterLuidLow:u32, adapterLuidHigh:u32 }
	//               (handles already duplicated into the game process; every
	//                textures message invalidates all prior slots)
	// frame         { slot:u32, serial:u64, width:u32, height:u32 }
	// domReady      { view:str }
	// loadEvent     { view:str, failed:bool, url:str, description:str, code:i32 }
	// webMessage    { view:str, json:str }
	// console       { view:str, json:str }       (raw Runtime.consoleAPICalled params)
	// cursor        { id:u32 }                   (active view only; Win32 cursor id, 0 = hidden)
	// accelerator   { vk:u32, down:bool }        (framework-owned key hit inside Chromium)
	// evalResult    { id:u64, result:str }
	// log           { level:i32, text:str }      (host diagnostics into the game log)
	// bye           { reason:str }
}
