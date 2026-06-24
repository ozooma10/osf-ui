#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace PrismaSF
{
	// The game's live D3D12 objects. The version-anchored route to them now
	// lives in CommonLibSF as RE::CreationRendererPrivate::Renderer (REL::ID
	// 944397 -> g_RendererRoot); see that header for the proven offset chain.
	//
	// Locate() does NOT trust the engine accessor blindly: the device and queue
	// it returns only count if QueryInterface succeeds, the queue reports
	// D3D12_COMMAND_LIST_TYPE_DIRECT, and queue->GetDevice() is COM-identical to
	// the device. That consumer-side guard is what makes a stale layout (after a
	// game patch) fail closed instead of feeding the compositor a bad pointer.
	// On any failure both pointers stay null.
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
