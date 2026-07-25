#include "composite/ScaleformToTextureProbe.h"

#include "platform/WindowsPlatform.h"

// Keep <Windows.h> confined to this file. NOGDI stops wingdi's ERROR macro from
// clobbering REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace OSFUI::ScaleformToTextureProbe
{
	namespace
	{
		// Starfield 1.16.244 / Address Library:
		//   497640 -> CreationRendererPrivate::ScaleformToTextureRenderPass vtable
		//   146151 -> shared Scaleform pass Execute implementation
		//
		// Static RE of UIRenderPass::BuildRenderGraph (ID 145957) shows that the
		// engine creates this concrete pass only when a Scaleform render item has
		// both a movie object and an attached render-target object. Items without
		// that target receive ScaleformRenderPass instead.
		constexpr std::uint64_t kVtblScaleformToTexture = 497640;
		constexpr std::uint64_t kIdExecute = 146151;
		constexpr std::size_t   kExecuteSlot = 7;

		// Same four-register forwarding precedent as UiPassSeam::ExecuteFn. The
		// 1.16.244 implementation consumes RCX/RDX and safely ignores R8/R9.
		using ExecuteFn = void* (*)(void*, void*, void*, void*);

		std::atomic<ExecuteFn>      g_original{ nullptr };
		std::atomic<std::uint64_t> g_calls{ 0 };
		std::atomic<bool>          g_installTried{ false };
		std::atomic<bool>          g_installed{ false };

		[[nodiscard]] bool ShouldLog(const std::uint64_t a_call)
		{
			// Dense startup evidence, then logarithmic sampling for a long
			// session. At 60 fps this emits fewer than twenty lines per pass.
			return a_call <= 8 || (a_call & (a_call - 1)) == 0;
		}

		void* ExecuteThunk(void* a_this, void* a_context, void* a_arg3, void* a_arg4)
		{
			const auto call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
			const bool sample = ShouldLog(call);

			std::uint32_t renderItem = 0;
			std::uint32_t colorBinding = 0;
			std::uint32_t depthBinding = 0;
			bool tailReadable = false;
			if (a_this) {
				const auto tail = reinterpret_cast<std::uintptr_t>(a_this) + 0xF0;
				tailReadable = Platform::IsReadableRange(tail, 3 * sizeof(std::uint32_t));
				if (tailReadable) {
					std::memcpy(&renderItem, reinterpret_cast<const void*>(tail + 0x0), sizeof(renderItem));
					std::memcpy(&colorBinding, reinterpret_cast<const void*>(tail + 0x4), sizeof(colorBinding));
					std::memcpy(&depthBinding, reinterpret_cast<const void*>(tail + 0x8), sizeof(depthBinding));
				}
			}

			if (sample) {
				REX::INFO("[ScaleformToTextureProbe] call={} enter tid={} this=0x{:X} "
						  "ctx=0x{:X} arg3=0x{:X} arg4=0x{:X} tailReadable={} "
						  "renderItem=0x{:08X} colorBinding=0x{:08X} depthBinding=0x{:08X}",
					call, static_cast<std::uint32_t>(::GetCurrentThreadId()),
					reinterpret_cast<std::uintptr_t>(a_this),
					reinterpret_cast<std::uintptr_t>(a_context),
					reinterpret_cast<std::uintptr_t>(a_arg3),
					reinterpret_cast<std::uintptr_t>(a_arg4),
					tailReadable, renderItem, colorBinding, depthBinding);
			}

			const auto started = std::chrono::steady_clock::now();
			const auto original = g_original.load(std::memory_order_acquire);
			void* result = original ? original(a_this, a_context, a_arg3, a_arg4) : nullptr;
			if (sample) {
				const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - started).count();
				REX::INFO("[ScaleformToTextureProbe] call={} exit result=0x{:X} cpu={} us",
					call, reinterpret_cast<std::uintptr_t>(result), elapsedUs);
			}
			return result;
		}
	}

	bool Install()
	{
		if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
			return g_installed.load(std::memory_order_acquire);
		}

		const REL::Relocation<std::uintptr_t> vtbl{ REL::ID(kVtblScaleformToTexture) };
		const REL::Relocation<std::uintptr_t> expected{ REL::ID(kIdExecute) };
		const auto slotAddress = vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);

		std::uintptr_t current = 0;
		if (!Platform::SafeReadPointer(slotAddress, current)) {
			REX::WARN("[ScaleformToTextureProbe] slot 7 at 0x{:X} is unreadable; probe not installed",
				slotAddress);
			return false;
		}
		if (current != expected.address()) {
			REX::WARN("[ScaleformToTextureProbe] slot 7 holds 0x{:X}, expected 0x{:X} "
					  "(game patch or foreign hook); probe not installed",
				current, expected.address());
			return false;
		}

		auto** slot = reinterpret_cast<void**>(slotAddress);
		DWORD oldProtect = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			REX::WARN("[ScaleformToTextureProbe] VirtualProtect failed; probe not installed");
			return false;
		}

		// Publish the original before the vtable slot can expose the thunk to a
		// render worker. This ordering closes the same startup race guarded in
		// UiPassSeam.
		g_original.store(reinterpret_cast<ExecuteFn>(current), std::memory_order_release);
		*slot = reinterpret_cast<void*>(&ExecuteThunk);
		::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

		std::uintptr_t installed = 0;
		if (!Platform::SafeReadPointer(slotAddress, installed) ||
			installed != reinterpret_cast<std::uintptr_t>(&ExecuteThunk)) {
			REX::WARN("[ScaleformToTextureProbe] slot read-back failed; probe state is unknown");
			return false;
		}

		g_installed.store(true, std::memory_order_release);
		REX::INFO("[ScaleformToTextureProbe] armed on vtable 0x{:X} slot 7 "
				  "(original 0x{:X}); bounded samples will log only in devMode",
			vtbl.address(), current);
		return true;
	}
}
