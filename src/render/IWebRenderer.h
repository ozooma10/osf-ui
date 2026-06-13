#pragma once

#include "runtime/ViewManifest.h"

namespace SWUI
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
		virtual void LoadView(const ViewManifest& a_manifest) = 0;
		virtual void Resize(std::uint32_t a_width, std::uint32_t a_height) = 0;
		virtual void Update(double a_deltaSeconds) = 0;

		// Returns the current frame, or std::nullopt if there is nothing to
		// present. See FrameBufferView for the lifetime contract.
		virtual std::optional<FrameBufferView> Render() = 0;

		// Delivers a JSON message (native -> web). Backends without a JS
		// engine may log and drop it.
		virtual void SendMessageToWeb(std::string_view a_json) = 0;

		// Receives JSON messages (web -> native). Backends with a JS engine
		// invoke the handler from Update() on the calling (game) thread, never
		// from a renderer-internal thread. Backends without a JS engine ignore
		// this. Set before LoadView.
		using WebMessageHandler = std::function<void(std::string_view)>;
		virtual void SetWebMessageHandler(WebMessageHandler) {}

		// Delivers one keyboard transition into the web view. a_vkCode is a
		// Windows virtual-key code (the space Starfield ButtonEvents carry).
		// Thread-safe to call from the input thread; backends with a JS engine
		// dispatch it onto their own thread. No-op for backends without one.
		virtual void InjectKeyEvent(std::uint32_t /*a_vkCode*/, bool /*a_down*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
