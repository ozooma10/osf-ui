#pragma once

#include "composite/ICompositor.h"

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	// Seam-draw hook, defined in D3D12Compositor.cpp and called by UiPassSeam
	// from a render worker inside the engine's UI-buffer hand-off: records the
	// overlay quad onto the ENGINE's own command list, into the engine's UI
	// buffer. Returns true when a quad was recorded (the caller then restores
	// the engine's descriptor-heap binding). False when the compositor is not
	// set up, hidden, or has no ready GPU frame — the seam simply skips.
	[[nodiscard]] bool RecordSeamOverlayDraw(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer);

	// Draws the renderer's frames over the game image at present time, on the
	// game's own ID3D12Device + DIRECT queue (located via composite/EngineD3D12.h;
	// we create no device).
	//
	// Threading:
	//  - Submit() (SFSE tick thread) only copies the CPU frame into a
	//    lock-protected staging buffer. No GPU work there.
	//  - A hook on IDXGISwapChain::Present (vtable slot 8, captured from a
	//    throwaway swapchain so no engine offsets are touched; the vtable is
	//    shared by every swapchain in the process) runs on the render thread and
	//    does all GPU work: upload the cached frame to a texture, draw it as an
	//    alpha-blended fullscreen quad onto the current backbuffer, then call the
	//    original Present. So there is no cross-thread resource state.
	//
	// Owns nothing of the game's: own root signature, PSO, descriptor heaps,
	// fence, command allocators. Setup (locate + hook install) is lazy on the
	// first Submit — the renderer root global is empty during SFSE plugin load.
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
		[[nodiscard]] bool IsOutputSizeKnown() const override;
		// GPU transport (out-of-process WebView2 host): adopt the shared ring;
		// sharedSlot frames submitted afterwards are sampled directly on the
		// present thread (produce/consume fence synchronized, no CPU upload).
		void SetSharedRing(const SharedRingDesc& a_desc) override;
		// Seam-draw mode: the present hook keeps discovery/plumbing duties but
		// never draws; RecordSeamOverlayDraw (above) does, at the engine seam.
		void SetSeamDrawMode(bool a_enabled) override;

		[[nodiscard]] std::string_view Name() const override { return "d3d12"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
