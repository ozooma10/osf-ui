#pragma once

#include "render/IWebRenderer.h"

#include <span>

struct ID3D12Device;

namespace OSFUI::WorldSurface
{
	// One host process + shared ring per surface is the cost model; keep the
	// cap in lockstep with Config::kMaxWorldSurfaces.
	inline constexpr std::uint32_t kMaxSurfaces = 4;

	struct SurfaceDesc
	{
		// The placeholder dimensions form a deliberately unique
		// material-resource signature: normal textures must never share one,
		// and no two surfaces may share a size (config validation enforces
		// both, this layer re-checks the basics).
		std::uint32_t placeholderWidth{ 0 };
		std::uint32_t placeholderHeight{ 0 };
		std::string   label;  // the surface's view id; log identity only
	};

	// Registers the surface set once, before TryInstall. Each entry's position
	// is the surface key that every later per-surface call takes. Returns the
	// accepted count; entries with a zero dimension or beyond kMaxSurfaces are
	// dropped so indices stay dense.
	std::uint32_t Configure(std::span<const SurfaceDesc> a_surfaces);
	[[nodiscard]] bool IsEnabled();

	// Installs the material SRV observer once the engine device is available.
	bool TryInstall(ID3D12Device* a_device);

	// Owns the duplicated handles in a_desc and adopts them on the game thread.
	void SetSharedRing(std::uint32_t a_surface, const SharedRingDesc& a_desc);

	// Selects the newest completed ring slot for the surface's captured
	// material, and releases the previously displayed slot back to that
	// surface's browser host through its consume fence (CPU-side pacing, one
	// engine frame after display).
	void Submit(std::uint32_t a_surface, const FrameBufferView& a_frame);

	// Rewrites every captured material descriptor of every surface to its
	// currently displayed ring slot. Called every tick so the bindings survive
	// a static browser page and heal themselves if the engine re-creates a
	// descriptor. No-op for surfaces missing either a capture or a ring.
	void Refresh();

	void Shutdown();
}
