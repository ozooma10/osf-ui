#pragma once

#include "composite/ICompositor.h"

namespace SWUI
{
	// Phase 2 compositor: uploads the renderer's CPU frames into a GPU
	// texture using the game's own ID3D12Device + DIRECT queue (located via
	// composite/EngineD3D12.h — runtime-proven route, no hooks, no device
	// creation of our own). It does NOT draw anything yet: the texture stays
	// in COPY_DEST and present-time composition is Phase 3
	// (docs/renderer-plan.md).
	//
	// Design notes:
	//  - The engine objects are located LAZILY on the first Submit (the
	//    renderer root global is empty during SFSE plugin load; by the time
	//    frames flow the game is fully alive). Initialize() only resets
	//    state. Lookup failures retry on a budget, then give up loudly.
	//  - Submit copies the frame into one slot of a small upload ring and
	//    records+executes a CopyTextureRegion on the game's direct queue.
	//    Queues are free-threaded, so submitting from the SFSE tick thread
	//    is legal; slot reuse is fence-guarded and a busy ring SKIPS frames
	//    instead of ever blocking the game thread.
	//  - In devMode the first successful upload is verified end-to-end:
	//    texture -> readback buffer -> byte compare against the submitted
	//    pixels. That is the in-log substitute for a PIX capture.
	class D3D12Compositor final : public ICompositor
	{
	public:
		D3D12Compositor();
		~D3D12Compositor() override;

		bool Initialize() override;
		void Shutdown() override;
		void Submit(const FrameBufferView& a_frame) override;

		[[nodiscard]] std::string_view Name() const override { return "d3d12"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
