#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace SWUI
{
	// The game's live D3D12 objects, located via the route runtime-proven in
	// OSF RE (Investigations/Requests/2026-06-12-d3d12-device-route.md,
	// context module rendering.graphics_core, game 1.16.244):
	//
	//   root   = *(void**)REL::ID(944397)                       // g_RendererRoot
	//   device = [[root+0x30] + 0x418]                          // arDeviceProperties.pDxDevice
	//   queue  = [[[root+0x28] + 0x08] + 0x60]                  // the swap chain's DIRECT queue
	//
	// Locate() does NOT trust those offsets blindly: every hop is a guarded
	// read and the results only count if QueryInterface succeeds, the queue
	// reports D3D12_COMMAND_LIST_TYPE_DIRECT, and queue->GetDevice is
	// COM-identical to the device. On any failure both pointers stay null.
	//
	// The returned interfaces are AddRef'd (by QI); the caller owns one
	// reference to each and must Release them.
	struct EngineD3D12
	{
		ID3D12Device*       device{ nullptr };
		ID3D12CommandQueue* directQueue{ nullptr };

		[[nodiscard]] explicit operator bool() const { return device && directQueue; }
	};

	[[nodiscard]] EngineD3D12 LocateEngineD3D12();
}
