#pragma once

#include "render/IWebRenderer.h"

struct ID3D12Device;

namespace OSFUI::WorldSurface
{
	// Enables one in-world browser surface. The placeholder dimensions form a
	// deliberately unique material-resource signature; normal textures must
	// never share it.
	void Configure(std::uint32_t a_targetWidth, std::uint32_t a_targetHeight);
	[[nodiscard]] bool IsEnabled();

	// Installs the material SRV observer once the engine device is available.
	bool TryInstall(ID3D12Device* a_device);

	// Owns the duplicated handles in a_desc and adopts them on the game thread.
	void SetSharedRing(const SharedRingDesc& a_desc);

	// Selects the newest completed ring slot for the captured material, and
	// releases the previously displayed slot back to the browser host through
	// the consume fence (CPU-side pacing, one engine frame after display).
	void Submit(const FrameBufferView& a_frame);

	// Rewrites the captured material descriptor to the currently displayed ring
	// slot. Called every tick so the binding survives a static browser page and
	// heals itself if the engine re-creates that descriptor. No-op until both a
	// placeholder and a ring exist.
	void Refresh();

	void Shutdown();
}
