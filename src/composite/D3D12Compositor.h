#pragma once

#include "composite/ICompositor.h"

namespace PrismaSF
{
	// Phase 3 compositor: draws the renderer's frames over the game image at
	// present time, on the game's own ID3D12Device + DIRECT queue (located via
	// composite/EngineD3D12.h — runtime-proven route; we create no device).
	//
	// Threading model — the crux of this class:
	//  - Submit() (SFSE tick thread) ONLY copies the CPU frame into a
	//    lock-protected staging buffer. No GPU work there.
	//  - A hook on IDXGISwapChain::Present (vtable slot 8 — captured from a
	//    throwaway swapchain so no engine offsets are touched; the vtable is
	//    shared by every swapchain in the process) runs on the render thread.
	//    THAT is where all GPU work happens: upload the cached frame to a
	//    texture, then draw it as an alpha-blended fullscreen quad onto the
	//    current backbuffer, then call the original Present.
	//  - Because every GPU operation for the overlay lives on the render
	//    thread inside the present hook, there is no cross-thread resource
	//    state to coordinate.
	//
	// Still owns nothing of the game's: own root signature, PSO, descriptor
	// heaps, fence, command allocators. Setup (locate + hook install) is lazy
	// on the first Submit (the renderer root global is empty during SFSE
	// plugin load).
	class D3D12Compositor final : public ICompositor
	{
	public:
		D3D12Compositor();
		~D3D12Compositor() override;

		bool Initialize() override;
		void Shutdown() override;
		void Submit(const FrameBufferView& a_frame) override;
		void SetVisible(bool a_visible) override;
		void SetOutputResizeCallback(OutputResizeCallback a_callback) override;

		[[nodiscard]] std::string_view Name() const override { return "d3d12"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
