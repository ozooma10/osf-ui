#pragma once

#include "Composite/OutputSizeObservation.h"
#include "Render/SharedTextureTransport.h"

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	// Records the webview2 overlay quad into the engine's UI buffer. Returns true if a quad was recorded.
	bool RecordOverlayIntoUIBuffer(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer, bool a_firstDrawInRegion);

	class D3D12Compositor final
	{
	public:
		D3D12Compositor();
		~D3D12Compositor();

		bool Initialize();
		void Submit(const FrameBufferView& a_frame);
		void SetVisible(bool a_visible);
		std::optional<OutputSize> GetObservedOutputSize() const;
		void SetSharedRing(const SharedRingDesc& a_desc);

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
