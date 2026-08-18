#pragma once

#include <dxgiformat.h>

namespace OSFUI::UiTargetFormat
{
	//Starfields UI layer is typeless RGBA8. Luma mod upgrades to RGBA16F
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
}
