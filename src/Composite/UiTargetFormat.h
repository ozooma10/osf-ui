#pragma once

#include <dxgiformat.h>

namespace OSFUI::UiTargetFormat
{
	// Starfield's stock UI layer is typeless RGBA8. Luma upgrades the same
	// Scaleform buffers to RGBA16F; both are rendered through the matching typed
	// RTV so the PSO and bound render target always agree.
	[[nodiscard]] constexpr DXGI_FORMAT ResolveRtv(const DXGI_FORMAT a_resourceFormat)
	{
		switch (a_resourceFormat) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	[[nodiscard]] constexpr const char* Name(const DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return "R8G8B8A8_UNORM";
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return "R16G16B16A16_FLOAT";
		default:
			return "UNKNOWN";
		}
	}

	static_assert(ResolveRtv(DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
		DXGI_FORMAT_R8G8B8A8_UNORM);
	static_assert(ResolveRtv(DXGI_FORMAT_R16G16B16A16_FLOAT) ==
		DXGI_FORMAT_R16G16B16A16_FLOAT);
}
