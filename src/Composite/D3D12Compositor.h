#pragma once

#include "Render/IWebRenderer.h"

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	struct CompositorStatus
	{
		bool seamActive{ false };
		bool frameGeneration{ false };
	};

	using OutputResizeCallback = std::function<void(std::uint32_t a_width, std::uint32_t a_height)>;

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
	// layer, on the game's own ID3D12Device (located via Composite/EngineD3D12.h;
	// we create no device). See docs/seam-draw-design.md.
	//
	// Threading:
	//  - Submit() (SFSE tick thread) adopts newly announced shared rings and
	//    records which slot the browser host published. No GPU work is submitted there.
	//  - RecordSeamOverlayDraw (above) runs on an engine render worker and does
	//    all GPU work onto the engine's own command list. It also observes the
	//    UI target size and whether the frame graph exposes the Frame Generation
	//    COPY_SOURCE hand-off.
	//  - The compositor never hooks IDXGISwapChain::Present.
	//
	// Owns nothing of the game's: own root signature, PSO, descriptor heaps and
	// fence. Setup is lazy on the first Submit — the
	// renderer root global is empty during SFSE plugin load.
	class D3D12Compositor final
	{
	public:
		D3D12Compositor();
		~D3D12Compositor();

		bool Initialize();
		void Submit(const FrameBufferView& a_frame);
		void SetVisible(bool a_visible);
		void SetOutputResizeCallback(OutputResizeCallback a_callback);
		[[nodiscard]] bool IsOutputSizeKnown() const;
		// GPU transport (out-of-process browser host): adopt the shared ring;
		// sharedSlot frames submitted afterwards are sampled directly at the
		// engine seam (produce/consume fence synchronized, no CPU upload).
		void SetSharedRing(const SharedRingDesc& a_desc);
		void SetSeamDrawMode(bool a_enabled);
		[[nodiscard]] CompositorStatus GetStatus() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
