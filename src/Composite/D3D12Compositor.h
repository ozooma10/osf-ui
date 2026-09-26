#pragma once

#include "Render/SharedTextureTransport.h"

#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	class SharedFrameConsumer;
	// Records the webview2 overlay quad into the UI pass target selected by UiPass.
	bool RecordOverlayIntoRenderTarget(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer);

	class D3D12Compositor final
	{
	public:
		D3D12Compositor();
		~D3D12Compositor();

		bool Initialize(std::shared_ptr<SharedFrameConsumer> a_frames);
		void Update();
		void SetVisible(bool a_visible);

	private:
		friend bool RecordOverlayIntoRenderTarget(ID3D12GraphicsCommandList*, ID3D12Resource*);
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
