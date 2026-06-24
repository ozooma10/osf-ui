#pragma once

#include "runtime/ViewManifest.h"

namespace PrismaSF
{
	struct RendererConfig
	{
		std::uint32_t width{ 1280 };
		std::uint32_t height{ 720 };
		bool          devMode{ false };

		// Plugin data root (Paths::DataDir()). Backends that need runtime
		// assets resolve them under here (e.g. ultralight/bin, ultralight/
		// resources) so render/ stays decoupled from core/Paths.
		std::filesystem::path dataDir;
	};

	enum class PixelFormat
	{
		kRGBA8,
		kBGRA8,
	};

	// Non-owning view of one CPU-side frame produced by a renderer.
	//
	// Ownership/lifetime contract: the pixel data is owned by the renderer that
	// returned it and is valid ONLY until the next call to Render(), Resize(),
	// or Shutdown() on that renderer. Consumers (compositors) must copy or
	// upload the data before returning control. Never store a FrameBufferView.
	struct FrameBufferView
	{
		std::span<const std::uint8_t> pixels;  // tightly packed rows unless strideBytes says otherwise
		std::uint32_t                 width{ 0 };
		std::uint32_t                 height{ 0 };
		std::uint32_t                 strideBytes{ 0 };  // bytes per row
		PixelFormat                   format{ PixelFormat::kRGBA8 };
		std::uint64_t                 frameIndex{ 0 };
	};

	// Renderer backend interface. Backends render web (or fake) content into a
	// CPU buffer; they know nothing about the game, D3D12, or hooks.
	class IWebRenderer
	{
	public:
		virtual ~IWebRenderer() = default;

		virtual bool Initialize(const RendererConfig& a_config) = 0;
		virtual void Shutdown() = 0;

		// Loads (or replaces) a view by its manifest id. Backends that support
		// it keep previously-loaded views so several can be hosted and composited
		// at once; the first loaded view becomes active by default. Call
		// SetActiveView to choose which one receives input.
		virtual void LoadView(const ViewManifest& a_manifest) = 0;

		// Selects which loaded view receives input (and, today, the bridge).
		// Multi-view backends honor it; single-view backends ignore it. No-op if
		// the id is not loaded. Default no-op for backends without views.
		virtual void SetActiveView(std::string_view /*a_id*/) {}

		// Resizes the view surface(s). Multi-view backends resize every hosted
		// view to the same output size so their frames composite 1:1.
		virtual void Resize(std::uint32_t a_width, std::uint32_t a_height) = 0;
		virtual void Update(double a_deltaSeconds) = 0;

		// Returns the current frame, or std::nullopt if there is nothing to
		// present. See FrameBufferView for the lifetime contract.
		virtual std::optional<FrameBufferView> Render() = 0;

		// Delivers a JSON message to ONE view (native -> web). a_viewId is the
		// manifest id of the target view. Backends without a JS engine may log
		// and drop it; single-view backends may ignore the id.
		virtual void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) = 0;

		// Receives JSON messages (web -> native), tagged with the SOURCE view id
		// so responses route back to the right view. Backends with a JS engine
		// invoke the handler from Update() on the calling (game) thread, never
		// from a renderer-internal thread. Backends without a JS engine ignore
		// this. Set before LoadView.
		using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;
		virtual void SetWebMessageHandler(WebMessageHandler) {}

		// Invoked once per view when its DOM becomes ready, on the game thread
		// (drained from Update()), tagged with the view id. Backs the consumer
		// API's CreateView DOM-ready callback. Set once before LoadView; no-op
		// for backends without a JS engine.
		using DomReadyHandler = std::function<void(std::string_view a_viewId)>;
		virtual void SetDomReadyHandler(DomReadyHandler) {}

		// Delivers one keyboard transition into the web view. a_vkCode is a
		// Windows virtual-key code (the space Starfield ButtonEvents carry).
		// Thread-safe to call from the input thread; backends with a JS engine
		// dispatch it onto their own thread. No-op for backends without one.
		virtual void InjectKeyEvent(std::uint32_t /*a_vkCode*/, bool /*a_down*/) {}

		// Mouse input in VIEW pixel coordinates (0..width, 0..height). Move
		// reports an absolute position (the caller maintains a virtual cursor,
		// since the OS cursor is hidden in gameplay); button uses the
		// MouseButton order (0=left, 1=right, 2=middle). Thread-safe; no-op
		// for backends without a JS engine.
		virtual void InjectMouseMove(int /*a_x*/, int /*a_y*/) {}
		virtual void InjectMouseButton(int /*a_x*/, int /*a_y*/, int /*a_button*/, bool /*a_down*/) {}

		// --- Programmatic consumer-API support (PrismaUI SF native API) -------
		// These back the public PRISMA_UI_API surface (src/api/). Backends with a
		// JS engine marshal the work onto their own worker thread and deliver any
		// callback back on the game thread (drained from Update()), exactly like
		// SetWebMessageHandler. Default no-ops so non-JS backends (null/mock)
		// build and link unchanged. a_viewId is the manifest/internal view id.

		// Run arbitrary JS in a view; if a_onResult is set it receives the
		// expression result as a string, on the game thread. (PRISMA: Invoke)
		using ScriptResultHandler = std::function<void(std::string a_result)>;
		virtual void EvaluateScript(std::string_view /*a_viewId*/, std::string_view /*a_js*/,
			ScriptResultHandler /*a_onResult*/ = nullptr) {}

		// Call window.<a_fnName>(a_arg) directly, no eval. (PRISMA: InteropCall)
		virtual void CallJsFunction(std::string_view /*a_viewId*/, std::string_view /*a_fnName*/,
			std::string_view /*a_arg*/) {}

		// Bind window.<a_name>(str) in the view; a_callback fires on the game
		// thread with the single string argument. (PRISMA: RegisterJSListener)
		using JsListenerHandler = std::function<void(std::string a_argument)>;
		virtual void RegisterJsFunction(std::string_view /*a_viewId*/, std::string_view /*a_name*/,
			JsListenerHandler /*a_callback*/) {}

		// Receive console.* from a view on the game thread; a_level is
		// 0=log,1=warning,2=error,3=debug,4=info. nullptr unsubscribes.
		// (PRISMA: RegisterConsoleCallback)
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		virtual void SetConsoleHandler(std::string_view /*a_viewId*/, ConsoleHandler /*a_handler*/) {}

		// Per-view state the consumer API mutates at runtime. Multi-view backends
		// honor them in the compositing/scroll path; others ignore.
		virtual void SetViewHidden(std::string_view /*a_viewId*/, bool /*a_hidden*/) {}
		virtual void SetViewOrder(std::string_view /*a_viewId*/, int /*a_order*/) {}
		virtual void SetScrollPixelSize(std::string_view /*a_viewId*/, int /*a_pixels*/) {}

		// Tear down a single view and free its resources. (PRISMA: Destroy)
		virtual void DestroyView(std::string_view /*a_viewId*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
