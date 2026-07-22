#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
	// Monotonic counters sampled on the game thread while render diagnostics are
	// enabled. Implementations may leave unsupported fields at zero.
	struct CompositorStats
	{
		std::uint64_t presents{ 0 };
		std::uint64_t draws{ 0 };
		std::uint64_t freshFrames{ 0 };
		std::uint64_t reusedDraws{ 0 };
		std::uint64_t submits{ 0 };
		std::uint64_t busyWaits{ 0 };
		std::uint64_t droppedBusy{ 0 };
		std::uint64_t skippedConcurrent{ 0 };
		std::uint64_t sourceToDrawMsTotal{ 0 };
		std::uint64_t sourceToDrawSamples{ 0 };
		std::uint64_t recordCpuUsTotal{ 0 };
		std::uint64_t recordCpuSamples{ 0 };
		bool seamMode{ false };
		bool frameGeneration{ false };
	};

	// Consumes CPU frames from a renderer and presents them over the game image.
	// Submit() must finish using the FrameBufferView before returning; the
	// pixels are valid only for the duration of the call.
	class ICompositor
	{
	public:
		virtual ~ICompositor() = default;

		virtual bool Initialize() = 0;
		virtual void Shutdown() = 0;
		virtual void Submit(const FrameBufferView& a_frame) = 0;

		// Overlay visibility. A present-time compositor redraws the last frame
		// every present, so it needs an explicit hide signal: hiding only stops
		// Submit() being called, which the present hook cannot observe.
		// Default no-op for compositors that draw nothing.
		virtual void SetVisible(bool /*a_visible*/) {}

		// Callback invoked on the present/render thread when the output surface
		// size becomes known or changes. The runtime resizes the web view to
		// match, so the page renders aspect-correct instead of stretched.
		// Default no-op (a null compositor has no output).
		using OutputResizeCallback = std::function<void(std::uint32_t a_width, std::uint32_t a_height)>;
		virtual void SetOutputResizeCallback(OutputResizeCallback /*a_callback*/) {}

		// Default true: most compositors need no asynchronously discovered output
		// size. One that does returns false until its present hook has observed
		// the real target, holding a deferred reveal off a manifest-sized frame.
		[[nodiscard]] virtual bool IsOutputSizeKnown() const { return true; }

		// GPU transport (out-of-process WebView2 host): adopt a shared-texture
		// ring; later Submit() calls may carry sharedSlot frames living in it.
		// The compositor takes ownership of the handles (see SharedRingDesc).
		// Default no-op; CPU-only compositors ignore sharedSlot frames.
		virtual void SetSharedRing(const SharedRingDesc& /*a_desc*/) {}

		// Seam-draw mode records into the engine's own UI render pass instead of
		// at present time, which makes it ride Frame Generation's UI handling.
		// Default no-op for compositors without a seam path.
		virtual void SetSeamDrawMode(bool /*a_enabled*/) {}

		// Low-overhead counters used by the global render-diagnostics setting.
		// Disabled by default so normal play does not pay atomic/timing costs.
		virtual void SetRenderStatsEnabled(bool /*a_enabled*/) {}
		[[nodiscard]] virtual CompositorStats GetRenderStats() const { return {}; }

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
