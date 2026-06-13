#include "composite/EngineD3D12.h"

#include "platform/WindowsPlatform.h"

// Keep the Win32/D3D12 headers GDI-free so the ERROR macro never collides
// with REX::ERROR in this translation unit.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>

namespace SWUI
{
	namespace
	{
		// g_RendererRoot — the ONLY anchor. Proven in OSF RE 2026-06-12 on
		// game 1.16.244 (resolves to RVA 0x61FAC30 there); the ID is the
		// version-durable handle, never a raw address.
		constexpr std::uint64_t kRendererRootId = 944397;

		constexpr std::ptrdiff_t kRoot_DeviceProperties = 0x30;
		constexpr std::ptrdiff_t kDeviceProps_DxDevice = 0x418;
		constexpr std::ptrdiff_t kRoot_QueueOwnerA = 0x28;
		constexpr std::ptrdiff_t kQueueOwnerA_QueueOwnerB = 0x08;
		constexpr std::ptrdiff_t kQueueOwnerB_CommandQueue = 0x60;

		[[nodiscard]] bool ReadHop(const char* a_label, const std::uintptr_t a_base, const std::ptrdiff_t a_offset, std::uintptr_t& a_out)
		{
			if (!Platform::SafeReadPointer(a_base + a_offset, a_out) || a_out == 0 ||
				!Platform::IsReadableRange(a_out, sizeof(std::uintptr_t))) {
				REX::WARN("EngineD3D12: hop '{}' failed ([0x{:X}+0x{:X}] -> 0x{:X})", a_label, a_base, a_offset, a_out);
				return false;
			}
			return true;
		}

		template <class T>
		[[nodiscard]] T* QueryCandidate(const char* a_label, const std::uintptr_t a_candidate)
		{
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

		const auto rootGlobal = REL::ID(kRendererRootId).address();
		std::uintptr_t root = 0;
		if (!Platform::SafeReadPointer(rootGlobal, root) || root == 0) {
			REX::WARN("EngineD3D12: g_RendererRoot (ID {}) is empty — renderer not initialized yet?", kRendererRootId);
			return result;
		}

		// Device: root -> DeviceProperties -> pDxDevice, proven by QI.
		std::uintptr_t deviceProps = 0;
		std::uintptr_t deviceRaw = 0;
		if (!ReadHop("DeviceProperties", root, kRoot_DeviceProperties, deviceProps) ||
			!ReadHop("pDxDevice", deviceProps, kDeviceProps_DxDevice, deviceRaw)) {
			return result;
		}
		auto* device = QueryCandidate<ID3D12Device>("device", deviceRaw);
		if (!device) {
			return result;
		}

		// Queue: root -> QueueOwnerA -> QueueOwnerB -> commandQueue, proven by
		// QI + DIRECT type + COM identity of its parent device.
		std::uintptr_t queueOwnerA = 0;
		std::uintptr_t queueOwnerB = 0;
		std::uintptr_t queueRaw = 0;
		if (!ReadHop("QueueOwnerA", root, kRoot_QueueOwnerA, queueOwnerA) ||
			!ReadHop("QueueOwnerB", queueOwnerA, kQueueOwnerA_QueueOwnerB, queueOwnerB) ||
			!ReadHop("commandQueue", queueOwnerB, kQueueOwnerB_CommandQueue, queueRaw)) {
			device->Release();
			return result;
		}
		auto* queue = QueryCandidate<ID3D12CommandQueue>("queue", queueRaw);
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
			"(root ID {} -> 0x{:X}; all hops QI-verified)",
			deviceRaw, queueRaw, kRendererRootId, root);

		result.device = device;
		result.directQueue = queue;
		return result;
	}
}
