#pragma once

#include "composite/ICompositor.h"

namespace SWUI
{
	// STUB. Future D3D12 overlay compositor. Performs NO hooking, owns NO
	// device objects, and submits nothing to the GPU.
	//
	// This class cannot be implemented until the following are known (tracked
	// in docs/reverse-engineering-notes.md — none of it may be guessed):
	//   - how to obtain Starfield's ID3D12Device (CreateDevice hook? export
	//     scan? Renderer singleton?)
	//   - how to obtain the game's direct command queue for our copy/draw work
	//   - swapchain access and present timing (where in the frame we may
	//     record an overlay draw; IDXGISwapChain::Present hook vs. an engine-
	//     level present callback)
	//   - descriptor heap strategy (own heaps vs. reserving slots in the
	//     game's shader-visible heap)
	//   - resource state transitions expected by the engine around our draw
	//   - HDR pipelines, dynamic resolution scaling, windowed vs. fullscreen
	//     swapchain differences
	//   - coexistence with Steam overlay, ReShade, RTSS and other present-
	//     chain residents (hook ordering, double-hook detection)
	class D3D12Compositor final : public ICompositor
	{
	public:
		bool Initialize() override;
		void Shutdown() override;
		void Submit(const FrameBufferView& a_frame) override;

		[[nodiscard]] std::string_view Name() const override { return "d3d12"; }
	};
}
