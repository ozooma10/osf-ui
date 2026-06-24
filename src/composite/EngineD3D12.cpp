#include "composite/EngineD3D12.h"

#include "platform/WindowsPlatform.h"

#include "RE/C/CreationRenderer.h"

// Keep the Win32/D3D12 headers GDI-free so the ERROR macro never collides
// with REX::ERROR in this translation unit.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>

namespace PrismaSF
{
	namespace
	{
		// QI a candidate pointer to T. The engine accessor hands back raw,
		// unvalidated pointers, so guard readability before touching the vtable:
		// a not-yet-initialized renderer then fails cleanly instead of crashing.
		// A successful QI also AddRefs, giving the caller the owned reference it
		// later Releases.
		template <class T>
		[[nodiscard]] T* QueryCandidate(const char* a_label, const std::uintptr_t a_candidate)
		{
			if (a_candidate == 0 || !Platform::IsReadableRange(a_candidate, sizeof(std::uintptr_t))) {
				REX::WARN("EngineD3D12: {} candidate 0x{:X} is null or unreadable", a_label, a_candidate);
				return nullptr;
			}
			auto* unknown = reinterpret_cast<IUnknown*>(a_candidate);
			T* result = nullptr;
			const auto hr = unknown->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result));
			if (FAILED(hr) || !result) {
				REX::WARN("EngineD3D12: {} candidate 0x{:X} failed QueryInterface (hr=0x{:08X})",
					a_label, a_candidate, static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			return result;
		}

		[[nodiscard]] bool IsSameComObject(IUnknown* a_lhs, IUnknown* a_rhs)
		{
			IUnknown* lhs = nullptr;
			IUnknown* rhs = nullptr;
			const bool ok =
				SUCCEEDED(a_lhs->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&lhs))) &&
				SUCCEEDED(a_rhs->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&rhs)));
			const bool same = ok && lhs == rhs;
			if (lhs) {
				lhs->Release();
			}
			if (rhs) {
				rhs->Release();
			}
			return same;
		}
	}

	EngineD3D12 LocateEngineD3D12()
	{
		EngineD3D12 result{};

		// The proven offset walk (g_RendererRoot -> device / DIRECT queue) lives
		// in CommonLibSF now; this returns raw, unverified engine pointers.
		auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
		if (!renderer) {
			REX::WARN("EngineD3D12: RE::CreationRendererPrivate::Renderer is null — renderer not initialized yet?");
			return result;
		}

		// Re-prove the pair on our side: QI confirms the pointers really are the
		// interfaces we expect (catching a layout that drifted under a patch),
		// the queue must be DIRECT, and it must belong to this exact device.
		auto* device = QueryCandidate<ID3D12Device>("device", reinterpret_cast<std::uintptr_t>(renderer->GetDevice()));
		if (!device) {
			return result;
		}

		auto* queue = QueryCandidate<ID3D12CommandQueue>("queue", reinterpret_cast<std::uintptr_t>(renderer->GetGraphicsQueue()));
		if (!queue) {
			device->Release();
			return result;
		}

		const auto desc = queue->GetDesc();
		if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
			REX::WARN("EngineD3D12: queue type is {} (expected DIRECT) — refusing it", static_cast<int>(desc.Type));
			queue->Release();
			device->Release();
			return result;
		}

		ID3D12Device* queueDevice = nullptr;
		const bool sameDevice =
			SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&queueDevice))) &&
			queueDevice && IsSameComObject(queueDevice, device);
		if (queueDevice) {
			queueDevice->Release();
		}
		if (!sameDevice) {
			REX::WARN("EngineD3D12: queue does not belong to the located device — refusing the pair");
			queue->Release();
			device->Release();
			return result;
		}

		REX::INFO(
			"EngineD3D12: located ID3D12Device=0x{:X} + DIRECT ID3D12CommandQueue=0x{:X} "
			"(via RE::CreationRendererPrivate::Renderer; all pointers QI-verified)",
			reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(queue));

		result.device = device;
		result.directQueue = queue;
		return result;
	}
}
