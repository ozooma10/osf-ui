#pragma once

#include <cstdint>
#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace OSFUI
{
	class SharedFrameConsumer;
	class SurfaceFrames;
	// Records the webview2 overlay quad into the UI pass target selected by UiPass.
	bool RecordOverlayIntoRenderTarget(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer);

	class D3D12Compositor final
	{
	public:
		explicit D3D12Compositor(std::shared_ptr<SharedFrameConsumer> a_frames);
		~D3D12Compositor();

		void Update();
		void SetVisible(bool a_visible);
		// Tick thread. While set, the UI pass draws this surface letterboxed over black instead of the overlay.
		void SetFullscreenSource(std::shared_ptr<SurfaceFrames> a_source);
		bool PreparedFrameReady(std::uint32_t a_width, std::uint32_t a_height) const;

	private:
		friend bool RecordOverlayIntoRenderTarget(ID3D12GraphicsCommandList*, ID3D12Resource*);
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
