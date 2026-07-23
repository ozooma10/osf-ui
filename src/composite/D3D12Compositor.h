#pragma once

#include "composite/ICompositor.h"

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	// Seam-draw hook, defined in D3D12Compositor.cpp and called by UiPassSeam
	// from a render worker inside the engine's UI-buffer hand-off: records the
	// overlay quad onto the ENGINE's own command list, into the engine's UI
	// buffer. a_fgTarget identifies the FG UI-input hand-off. Returns
	// true when a quad was recorded (the caller then restores the engine's
	// descriptor-heap binding). False when the compositor is not set up,
	// hidden, or has no ready GPU frame — the seam simply skips.
	// a_regionFirst: first seam draw of this frame's End region — the ring
	// serial is promoted only then, so both targets sample the same frame.
	[[nodiscard]] bool RecordSeamOverlayDraw(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
		bool a_fgTarget, bool a_regionFirst);

	// Draws the renderer's frames into Starfield's own transparent Scaleform UI
	// layer, on the game's own ID3D12Device (located via composite/EngineD3D12.h;
	// we create no device). See docs/seam-draw-design.md.
	//
	// Threading:
	//  - Submit() (SFSE tick thread) only records which shared-ring slot the
	//    host published. No GPU work there.
	//  - RecordSeamOverlayDraw (above) runs on an engine render worker and does
	//    all GPU work, onto the engine's own command list.
	//  - A hook on IDXGISwapChain::Present (vtable slot 8, captured from a
	//    throwaway swapchain so no engine offsets are touched; the vtable is
	//    shared by every swapchain in the process) draws nothing. It exists for
	//    what only the present stream can report: output size, Frame Generation
	//    pacer classification, shared-ring adoption, and hook liveness.
	//
	// Owns nothing of the game's: own root signature, PSO, descriptor heaps and
	// fence. Setup (locate + hook install) is lazy on the first Submit — the
	// renderer root global is empty during SFSE plugin load.
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
		// Records that the engine seam is hooked and drawing. Reported in the
		// render diagnostics; the present hook never draws either way.
		void SetSeamDrawMode(bool a_enabled) override;
		void SetRenderStatsEnabled(bool a_enabled) override;
		[[nodiscard]] CompositorStats GetRenderStats() const override;

		[[nodiscard]] std::string_view Name() const override { return "d3d12"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
