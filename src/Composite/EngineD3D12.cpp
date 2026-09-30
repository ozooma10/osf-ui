#include "Composite/EngineD3D12.h"

#include "RE/C/CreationRenderer.h"

#include "Composite/D3D12Prologue.h"  // GDI-free <Windows.h> + <d3d12.h>

namespace OSFUI
{
	namespace
	{
		template <class T>
		[[nodiscard]] T* QueryCandidate(const char* a_label, const std::uintptr_t a_candidate)
		{
			if (a_candidate == 0) {
				REX::WARN("EngineD3D12: {} candidate is null", a_label);
				return nullptr;
			}
			auto* unknown = reinterpret_cast<IUnknown*>(a_candidate);
			T* result = nullptr;
			const auto hr = unknown->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result));
			if (FAILED(hr) || !result) {
				REX::WARN("EngineD3D12: {} candidate 0x{:X} failed QueryInterface (hr=0x{:08X})", a_label, a_candidate, static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			return result;
		}
	}

	EngineD3D12 LocateEngineD3D12()
	{
		EngineD3D12 result{};

		auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
		if (!renderer) {
			REX::WARN("EngineD3D12: RE::CreationRendererPrivate::Renderer is null — renderer not initialized yet?");
			return result;
		}

		auto* device = QueryCandidate<ID3D12Device>("device", reinterpret_cast<std::uintptr_t>(renderer->GetDevice()));
		if (!device) {
			return result;
		}

		auto* queue = QueryCandidate<ID3D12CommandQueue>("queue", reinterpret_cast<std::uintptr_t>(renderer->GetGraphicsQueue()));
		if (!queue) {
			device->Release();
			return result;
		}

		REX::INFO("EngineD3D12: located ID3D12Device=0x{:X} + ID3D12CommandQueue=0x{:X}", reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(queue));

		result.device = device;
		result.directQueue = queue;
		return result;
	}
}
