#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace OSFUI
{
	struct EngineD3D12
	{
		ID3D12Device*       device{ nullptr };
		ID3D12CommandQueue* directQueue{ nullptr };

		explicit operator bool() const { return device && directQueue; }
	};

	EngineD3D12 LocateEngineD3D12();
}
