#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
	// Small, local-only status snapshot used by System Health.
	struct CompositorStatus
	{
		bool scaleformOverlayActive{ false };
		bool frameGeneration{ false };
	};

	// Consumes shared-texture frames and composites them over the game image.
	class ICompositor
	{
	public:
		virtual ~ICompositor() = default;

		virtual bool Initialize() = 0;
		virtual void Submit(const FrameBufferView& a_frame) = 0;

		// Overlay visibility. The Scaleform hook redraws the last frame independently of
		// Submit(), so it needs an explicit hide signal. Hiding only stops
		// new frames; the render hook can still reuse the previous one.
		// Default no-op for compositors that draw nothing.
		virtual void SetVisible(bool /*a_visible*/) {}

		// Callback invoked on the present/render thread when the output surface
		// size becomes known or changes. The runtime resizes the web view to
		// match, so the page renders aspect-correct instead of stretched.
		// Default no-op for implementations that do not report output-size changes asynchronously.
		using OutputResizeCallback = std::function<void(std::uint32_t a_width, std::uint32_t a_height)>;
		virtual void SetOutputResizeCallback(OutputResizeCallback /*a_callback*/) {}

		// Default true: most compositors need no asynchronously discovered output
		// size. One that does returns false until the Scaleform hook has observed
		// the real target, holding a deferred reveal off a manifest-sized frame.
		[[nodiscard]] virtual bool IsOutputSizeKnown() const { return true; }

		// GPU transport (out-of-process browser host): adopt a shared-texture
		// ring; later Submit() calls may carry sharedSlot frames living in it.
		// The compositor takes ownership of the handles (see SharedRingDesc).
		// Default no-op for compositors that draw nothing.
		virtual void SetSharedRing(const SharedRingDesc& /*a_desc*/) {}

		// Scaleform-overlay mode records into the engine's own UI render pass, which
		// makes it ride Frame Generation's UI handling.
		// Default no-op for compositors without a Scaleform-overlay path.
		virtual void SetScaleformOverlayEnabled(bool /*a_enabled*/) {}

		[[nodiscard]] virtual CompositorStatus GetStatus() const { return {}; }

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
