#pragma once

#include "Views/ViewManifest.h"

namespace OSFUI
{
	struct RendererConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };

		// Plugin data root (Paths::DataDir()). Web renderer implementations resolve packaged assets
		// such as bin/osfui_webview2_host.exe under here, keeping render/
		// decoupled from Core/Paths.
		std::filesystem::path dataDir;

	};

	// Cursor shape a page wants (CSS `cursor`), mirrored onto the OS pointer
	// while the overlay captures input (Input/HardwareCursor). Web cursors with
	// no stock Win32 equivalent collapse to the nearest listed one (kArrow when
	// nothing fits). kNone means `cursor: none`.
	enum class CursorShape
	{
		kArrow,
		kCross,
		kHand,
		kIBeam,
		kWait,
		kHelp,
		kNotAllowed,
		kSizeWE,
		kSizeNS,
		kSizeNESW,
		kSizeNWSE,
		kSizeAll,
		kNone,
	};

	// ICoreWebView2CompositionController::get_SystemCursorId returns the Win32
	// OCR_* integer IDs. Keep this platform-neutral so the translation can be
	// tested without loading WebView2 or the game.
	[[nodiscard]] constexpr CursorShape CursorShapeFromSystemCursorId(std::uint32_t a_id) noexcept
	{
		switch (a_id) {
		case 0:     return CursorShape::kNone;
		case 32513: return CursorShape::kIBeam;       // OCR_IBEAM
		case 32514: return CursorShape::kWait;        // OCR_WAIT
		case 32515: return CursorShape::kCross;       // OCR_CROSS
		case 32642: return CursorShape::kSizeNWSE;    // OCR_SIZENWSE
		case 32643: return CursorShape::kSizeNESW;    // OCR_SIZENESW
		case 32644: return CursorShape::kSizeAll;     // OCR_SIZEALL
		case 32645: return CursorShape::kSizeWE;      // OCR_SIZEWE
		case 32646: return CursorShape::kSizeNS;      // OCR_SIZENS
		case 32648: return CursorShape::kNotAllowed;  // OCR_NO
		case 32649: return CursorShape::kHand;        // OCR_HAND
		case 32651: return CursorShape::kHelp;        // OCR_HELP
		default:    return CursorShape::kArrow;
		}
	}

	// One shared-texture frame produced by the out-of-process renderer.
	struct FrameBufferView
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint64_t frameIndex{ 0 };
		// Slot in the shared-texture ring announced through SharedRingHandler.
		// frameIndex doubles as the produce-fence value for that slot.
		std::uint32_t sharedSlot{ 0 };
	};

	// Cross-process shared-texture ring produced by the browser host and
	// announced by the web renderer on the game thread. Handles are already
	// valid in this process (duplicated by the producer) and owned by the
	// consumer once delivered: open with ID3D12Device::OpenSharedHandle, then
	// CloseHandle. Sync is a produce fence (wait for value == frameIndex before
	// sampling a slot) and a consume fence (signal frameIndex after the GPU read
	// completes, so the producer may rewrite the slot). A new announcement
	// (higher generation) invalidates every prior slot.
	struct SharedRingDesc
	{
		// Capacity only — the producer announces the actual ring depth per
		// generation (slotCount); entries past it stay null. Keeps the depth a
		// producer-side tuning knob instead of a cross-binary constant.
		static constexpr std::size_t kMaxSlots = 8;

		void*         slotHandles[kMaxSlots]{};
		std::uint32_t slotCount{ 0 };
		void*         produceFence{ nullptr };
		void*         consumeFence{ nullptr };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		// Adapter that created the resources, copied from the browser host's D3D11
		// device. Used only for diagnostics when D3D12 rejects a handle.
		std::uint32_t adapterLuidLow{ 0 };
		std::uint32_t adapterLuidHigh{ 0 };
		std::uint64_t generation{ 0 };
	};

	// Web renderer interface. Implementations know nothing about the game or its
	// D3D12 render-pass hooks; frames cross the boundary as shared ring slots.
	class IWebRenderer
	{
	public:
		virtual ~IWebRenderer() = default;

		virtual bool Initialize(const RendererConfig& a_config) = 0;

		// Tear down a terminal browser-host connection while preserving instantiated-view
		// records, callbacks, and reconstructible state for a fresh on-demand start.
		// Called on the game thread only, after the FailureHandler has returned.
		// Transient queued bridge traffic may be discarded; the runtime must replay
		// each new document's bootstrap state before the next Update().
		// False means this web renderer cannot recover without replacing the object.
		virtual bool RestartAfterFailure() { return false; }

		// Instantiate the browser object if absent, then start or restart its
		// document navigation. Existing instances remain hosted so several can
		// composite at once; the first instance becomes the input target by
		// default. Use SetInputTargetView to change that.
		virtual void CreateOrNavigateView(const ViewManifest& a_manifest) = 0;

        // Development-only loose-file support. This may run on the dev worker,
        // so implementations must synchronize any mutable filesystem state.
        // Direct-path and non-browser implementations need no work. False asks the
        // caller to retry after an editor or antivirus releases a file.
        virtual bool RefreshViewFiles(std::string_view /*a_viewId*/) { return true; }

		// Selects which instantiated view receives mouse, focus, and synthetic-key
		// input. No-op if the id is not instantiated.
		virtual void SetInputTargetView(std::string_view /*a_id*/) {}

		// Resizes every hosted view's output surface to the same dimensions so
		// their frames composite 1:1.
		virtual void Resize(std::uint32_t a_width, std::uint32_t a_height) = 0;
		virtual void Update(double a_deltaSeconds) = 0;

		// Returns the current frame, or std::nullopt if there is nothing to
		// present.
		virtual std::optional<FrameBufferView> Render() = 0;

		// Delivers a JSON message to one view (native -> web); a_viewId is the
		// target's qualified view id. Implementations without a JS engine may log and drop it;
		// single-view implementations may ignore the id.
		virtual void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) = 0;

		// Receives JSON messages (web -> native), tagged with the source view id
		// so responses route back. Invoked from Update() on the calling (game)
		// thread, never from a renderer-internal thread. Set before
		// CreateOrNavigateView.
		using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;
		virtual void SetWebMessageHandler(WebMessageHandler) {}

		// A main-frame load reaching a terminal state, on the game thread
		// (drained from Update()). `failed` false means success and is followed
		// by DOM-ready; true means failure, with description/errorDomain/
		// errorCode carrying the web renderer's details. A failed load does not
		// fire DOM-ready, so this is the only signal that a view did not come
		// up. Set once before CreateOrNavigateView.
		struct LoadEvent
		{
			std::string_view viewId;
			bool             failed{ false };
			std::string_view url;
			std::string_view description;   // failed only
			std::string_view errorDomain;   // failed only
			int              errorCode{ 0 };  // failed only
		};
		using LoadHandler = std::function<void(const LoadEvent& a_event)>;
		virtual void SetLoadHandler(LoadHandler) {}

		// A terminal web-renderer failure that leaves no drawable view.
		// The runtime must immediately release any modal menu policy; otherwise an
		// invisible overlay can keep gameplay input and pause state. A renderer
		// implementation that supports recovery may later receive
		// RestartAfterFailure, but only after this handler returns. Fired on the
		// game thread from Update(); set once before
		// CreateOrNavigateView.
		struct FailureEvent
		{
			std::string_view stage;
			std::string_view viewId;
			std::string_view description;
			std::uint32_t    errorCode{ 0 };
		};
		using FailureHandler = std::function<void(const FailureEvent& a_event)>;
		virtual void SetFailureHandler(FailureHandler) {}

		// Browser-host health worth reporting in Mod Settings' System Health
		// destination. Only DEGRADED-BUT-ALIVE conditions belong here — a reduced
		// shared-texture ring or focus stranded in the browser host's child window.
		// A web renderer that cannot render at all reports through the log and the
		// launch dialog instead, because there is no view left to draw System Health.
		// `code` is a stable machine string the built-in
		// frontend maps to player-facing copy; `detail` is short technical text
		// shown only under the issue's disclosure and must carry no absolute
		// paths. Fired on the game thread, drained from Update(), and both edges
		// are reported: `active` false means the condition cleared. Set once
		// before CreateOrNavigateView.
		struct HealthEvent
		{
			std::string_view code;
			bool             active{ true };
			std::string_view detail;
		};
		using HealthHandler = std::function<void(const HealthEvent& a_event)>;
		virtual void SetHealthHandler(HealthHandler) {}

		// Fires when the input-target view's requested cursor changes, so the
		// OSF UI runtime can switch the real OS pointer (hover feedback, text I-beam).
		// WARNING: unlike the other handlers, this may be invoked from a
		// renderer-internal thread — the handler must be cheap and thread-safe.
		// The in-tree consumer is a single atomic store; the window hook applies
		// the shape on the next mouse message. Set once before CreateOrNavigateView.
		using CursorChangeHandler = std::function<void(CursorShape a_shape)>;
		virtual void SetCursorChangeHandler(CursorChangeHandler) {}

		// Web renderers such as WebView2 receive keyboard/IME through a real focused
		// native child window. The runtime uses this
		// seam for the whole active-menu input session; HUD-only and closed states
		// revoke it so the game remains the foreground owner. While the WebView
		// holds focus, the web renderer's
		// AcceleratorKeyPressed hook delegates framework-owned keys (toggle, Esc,
		// key capture) back to the runtime; the callback returns true when
		// Chromium must mark that accelerator handled.
		// a_scanCode is the composed physical code (DIK convention, see
		// Input/ScanCode.h) — framework-owned matching keys on it; a_vkCode
		// stays alongside for the fixed-VK checks (Esc, F12 devtools).
		using NativeAcceleratorHandler =
			std::function<bool(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down)>;
		virtual void SetNativeAcceleratorHandler(NativeAcceleratorHandler) {}
		virtual void SetNativeFocus(bool /*a_focused*/) {}
		[[nodiscard]] virtual bool UsesNativeKeyboardFocus() const { return false; }

		// Out-of-process web renderers decide synchronously, in the browser host process,
		// whether a key is framework-owned and must be withheld from the page,
		// so the browser host needs a mirror of the OSF UI runtime's accelerator state. The
		// OSF UI runtime pushes it every tick; web renderers diff and forward on change.
		// Toggle and capture-up are physical SCAN codes (DIK convention), the
		// binding identity space. No-op for in-process implementations, which call
		// the accelerator handler directly instead.
		virtual void SetAcceleratorKeys(std::uint32_t /*a_toggleScan*/,
			bool /*a_captured*/,
			bool /*a_captureArmed*/, std::uint32_t /*a_captureUpScan*/) {}

		// Announces (or replaces) the renderer's GPU shared-texture ring, on the
		// game thread (drained from Update()). The runtime forwards it to the
		// compositor, which owns the handles from then on — see SharedRingDesc.
		// Only fired by web renderers that produce sharedSlot frames.
		using SharedRingHandler = std::function<void(const SharedRingDesc&)>;
		virtual void SetSharedRingHandler(SharedRingHandler) {}

		// Delivers one keyboard transition into the web view. a_vkCode is a
		// Windows virtual-key code (the space Starfield ButtonEvents carry).
		// Thread-safe to call from the input thread; web renderers dispatch onto
		// their own thread.
		virtual void InjectKeyEvent(std::uint32_t /*a_vkCode*/, bool /*a_down*/) {}

		// Mouse input in view pixel coordinates (0..width, 0..height). Move
		// reports an absolute position — the caller maintains a virtual cursor,
		// since the OS cursor is hidden in gameplay. Button uses MouseButton
		// order (0=left, 1=right, 2=middle). Thread-safe.
		virtual void InjectMouseMove(int /*a_x*/, int /*a_y*/) {}
		virtual void InjectMouseButton(int /*a_x*/, int /*a_y*/, int /*a_button*/, bool /*a_down*/) {}
		// Mouse wheel at a view-space position. a_wheelDelta is a signed multiple
		// of WHEEL_DELTA (120); positive scrolls up. The web renderer forwards the raw
		// delta to the browser host's WebView2 WHEEL input, which performs the scroll.
		virtual void InjectMouseWheel(int /*a_x*/, int /*a_y*/, int /*a_wheelDelta*/) {}
		// Physical mouse-wheel fallback from the game window. The out-of-process
		// WebView2 web renderer normally captures this directly once it owns menu focus;
		// keeping a distinct path lets it ignore the duplicate game-side raw packet
		// while still using it if browser-host raw-input registration failed.
		virtual void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta)
		{
			InjectMouseWheel(a_x, a_y, a_wheelDelta);
		}

		// Open the browser's native developer tools for one view. Production
		// implementations may ignore this; the OSF UI runtime exposes it only in developer mode.
		virtual void OpenDevTools(std::string_view /*a_viewId*/) {}

		// NOTE: there is deliberately no "evaluate this script text" primitive.
		// EvaluateScript / CallJsFunction / RegisterJsFunction existed here and
		// were never called by anything; they were removed rather than left as a
		// standing way to run caller-supplied script inside a privileged view.
		// Native->page communication goes through the typed bridge
		// (SetWebMessageHandler / PostWebMessage), whose payloads are JSON-encoded
		// rather than concatenated into source text.

		// Receive console.* from a view on the game thread; a_level is
		// 0=log,1=warning,2=error,3=debug,4=info. nullptr unsubscribes.
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		virtual void SetConsoleHandler(std::string_view /*a_viewId*/, ConsoleHandler /*a_handler*/) {}

		// Per-view state mutated at runtime. Honored by multi-view implementations in
		// the compositing/scroll path; others ignore.
		virtual void SetViewHidden(std::string_view /*a_viewId*/, bool /*a_hidden*/) {}
		virtual void SetViewOrder(std::string_view /*a_viewId*/, int /*a_order*/) {}

		virtual void DestroyView(std::string_view /*a_viewId*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
