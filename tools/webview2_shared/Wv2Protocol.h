#pragma once

// Shared wire protocol between the OSF UI plugin (or the standalone POC
// client) and osfui_webview2_host.exe. Both sides compile this header; the
// transport is one message-framed named pipe (Wv2Pipe.h) carrying UTF-8 JSON.
//
// Framing: [u32 little-endian payload byte count][payload]. Payloads are one
// JSON object with a "type" field. Unknown types must be ignored (forward
// compatibility), unknown fields likewise.
//
// Roles: the GAME side is the pipe SERVER (it owns the pipe name + ACL and
// launches the host); the HOST is the pipe CLIENT. The host exits when the
// pipe breaks or the game process handle signals.

#include <cstdint>

namespace osfui::wv2
{
	// Bumped on any incompatible wire change. The host refuses a mismatched
	// client hello and exits (both binaries ship together, so a mismatch means
	// a stale mirrored exe — the launcher versions the mirror dir to avoid it).
	// v2: multi-view — per-view `view` routing on game->host view messages and
	// `view` tagging on host->game page events (see below).
	inline constexpr std::uint32_t kProtocolVersion = 2;

	// Pipe name pattern: \\.\pipe\osfui-wv2-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-wv2-";

	// Hard cap on one framed message (a resize-storm of eval results should
	// never approach this; protects both sides from a corrupt length prefix).
	inline constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;

	// Shared-texture ring depth. 3 slots: one being written, one in flight,
	// one being composited.
	inline constexpr std::uint32_t kRingSlots = 3;

	// Multi-view (v2): the host keeps one composition controller + child
	// ContainerVisual per OSF UI view under ONE captured root visual, so a
	// single WGC capture / shared-texture ring carries the already-composited
	// stack. `navigate`'s `id` IS the view id — the first navigate for an
	// unknown id creates that view. View-scoped game->host messages carry
	// `view:str`; when it is absent or unknown they fall back to the ACTIVE
	// view (keeps the single-view POC client working). Page events host->game
	// are tagged with their source `view`.
	//
	// --- message types, game -> host ---
	// init          { topLevelHwnd:u64, viewsPath:str, virtualHost:str,
	//                 width:u32, height:u32, userDataDir:str, devMode:bool }
	// navigate      { id:str, entry:str, bridge:bool }   (creates view `id` on first sight)
	// resize        { width:u32, height:u32 }    (global: every view renders output-sized)
	// setHidden     { view:str, hidden:bool }    (child-visual visibility + Chromium suspend)
	// setOrder      { view:str, order:i32 }      (composite z: lower beneath, ties by creation)
	// setActive     { view:str }                 (mouse/focus/synthetic-key target)
	// focus         { focused:bool }             (moves real focus into the ACTIVE view)
	// mouse         { kind:"move"|"button"|"wheel", x:i32, y:i32,
	//                 button:i32, down:bool, wheel:i32 }   (active view)
	// key           { vk:u32, down:bool }        (synthetic tap into the active view's widget)
	// postWeb       { view:str, json:str }
	// eval          { view:str, id:u64, script:str }
	// accelState    { toggleVk:u32, devReloadVk:u32, captured:bool,
	//                 captureArmed:bool, captureUpVk:u32 }
	// destroyView   { view:str }
	// shutdown      { }
	//
	// --- message types, host -> game ---
	// hello         { protocolVersion:u32, hostVersion:str, runtimeVersion:str, pid:u32 }
	// ready         { }                          (first controller + capture up)
	// textures      { width:u32, height:u32, slots:[u64...],
	//                 produceFence:u64, consumeFence:u64, keyedMutex:bool }
	//               (handles already duplicated INTO the game process; every
	//                textures message invalidates all prior slots)
	// sharedMemory  { name:str, width:u32, height:u32, slots:u32 }   (CPU fallback ring)
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
