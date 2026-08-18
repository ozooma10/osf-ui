#pragma once

#include "Render/SharedTextureTransport.h"

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

	// Records the webview2 overlay quad into the engine's UI buffer. Returns true if a quad was recorded.
	bool RecordOverlayIntoUIBuffer(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer, bool a_fgTarget, bool a_regionFirst);

	class D3D12Compositor final
	{
	public:
		D3D12Compositor();
		~D3D12Compositor();

		bool Initialize();
		void Submit(const FrameBufferView& a_frame);
		void SetVisible(bool a_visible);
		void SetOutputResizeCallback(OutputResizeCallback a_callback);
		bool IsOutputSizeKnown() const;
		void SetSharedRing(const SharedRingDesc& a_desc);
		void SetSeamDrawMode(bool a_enabled);
		CompositorStatus GetStatus() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
