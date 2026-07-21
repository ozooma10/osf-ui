#include "composite/UiPassSeam.h"

#include "core/Log.h"
#include "platform/WindowsPlatform.h"

// GDI-free Win32/D3D12 so the ERROR macro never collides with REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OSFUI::UiPassSeam
{
	namespace
	{
		// AddrLib IDs, proven on 1.16.244. Canonical record with disassembly
		// evidence: OSF RE context module `rendering.ui_pass` (2026-07-21).
		constexpr std::uint64_t kVtblScaleformBegin = 497423;      // anon-ns ScaleformBegin pass vtable
		constexpr std::uint64_t kVtblScaleformComposite = 497272;  // ScaleformCompositeRenderPass vtable
		constexpr std::uint64_t kIdBeginExecute = 145955;          // ScaleformBegin::ExecuteRenderPass
		constexpr std::uint64_t kIdCompositeExecute = 145827;      // ScaleformCompositeRenderPass::ExecuteRenderPass
		constexpr std::uint64_t kIdGraphCtxGetCmdCtx = 144161;     // GraphContext -> engine command context getter
		constexpr std::uint64_t kIdScaleformHAL = 937259;          // Scaleform::Render::CreationRenderer::HAL*

		// RenderPass vtable slot 7 = ExecuteRenderPass(this, GraphContext*, IOHandles*).
		constexpr std::size_t kExecuteSlot = 7;

		// Valid only inside a Scaleform pass Execute (bound by ScaleformBegin,
		// cleared before the bracket returns) — read there, never cached.
		constexpr std::uint64_t kHalRTOffset = 0x1410;   // UI render-target info (graph-transient stack ptr)
		constexpr std::uint64_t kHalCtxOffset = 0x1418;  // engine command context

		constexpr std::uint32_t kMaxDetailLogs = 8;  // per site: plain call logs
		constexpr std::uint32_t kMaxScans = 2;       // per site: deep pointer scans (verbose)

		using ExecuteFn = void* (*)(void*, void*, void*, void*);
		using GetCmdCtxFn = void* (*)(void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };

		struct SiteState
		{
			std::atomic<std::uint64_t> count{ 0 };
			std::atomic<std::uint32_t> scans{ 0 };
		};
		SiteState g_begin;
		SiteState g_composite;

		// ---------------------------------------------------------------- SEH
		// The scans below make virtual calls on pointers we only BELIEVE are
		// live COM objects (gated on their vtable living inside d3d12.dll).
		// Each such call sits behind SEH so a wrong guess logs nothing instead
		// of crashing a render worker. These helpers hold no C++ objects
		// (C2712: __try cannot share a frame with unwinding).

		[[nodiscard]] bool TryQueryInterface(void* a_object, const IID& a_iid, void** a_out) noexcept
		{
			__try {
				return SUCCEEDED(static_cast<IUnknown*>(a_object)->QueryInterface(a_iid, a_out)) && *a_out;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] void* CallGetterSeh(GetCmdCtxFn a_fn, void* a_arg) noexcept
		{
			__try {
				return a_fn(a_arg);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// ------------------------------------------------------------- module
		// A virtual call is only attempted when the candidate's vtable lies in
		// d3d12.dll — engine objects put a destructor at slot 0, so an
		// unguarded "QueryInterface" on one would destroy it.
		struct ModuleRange
		{
			std::uintptr_t base{ 0 };
			std::size_t size{ 0 };

			[[nodiscard]] bool Contains(const std::uintptr_t a_address) const
			{
				return base && a_address >= base && a_address < base + size;
			}
		};

		[[nodiscard]] ModuleRange LocateD3D12Module()
		{
			const auto module = ::GetModuleHandleW(L"d3d12.dll");
			if (!module) {
				return {};
			}
			const auto base = reinterpret_cast<std::uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			return { base, nt->OptionalHeader.SizeOfImage };
		}

		[[nodiscard]] const char* FormatName(const DXGI_FORMAT a_format)
		{
			switch (a_format) {
				case DXGI_FORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return "R8G8B8A8_UNORM_SRGB";
				case DXGI_FORMAT_B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM";
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return "B8G8R8A8_UNORM_SRGB";
				case DXGI_FORMAT_R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM";
				case DXGI_FORMAT_R11G11B10_FLOAT:      return "R11G11B10_FLOAT";
				case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
				default:                               return "other";
			}
		}

		// ------------------------------------------------------------- scanner
		// Walks an object's leading qwords looking for D3D12 interfaces, one
		// nesting level deep. Everything is SafeReadPointer'd (the blobs include
		// worker-stack memory) and log output is budgeted per scan.
		struct ScanBudget
		{
			std::uint32_t logs{ 40 };
			std::array<std::uintptr_t, 32> visited{};
			std::size_t visitedCount{ 0 };

			[[nodiscard]] bool MarkVisited(const std::uintptr_t a_ptr)
			{
				for (std::size_t i = 0; i < visitedCount; ++i) {
					if (visited[i] == a_ptr) {
						return false;
					}
				}
				if (visitedCount < visited.size()) {
					visited[visitedCount++] = a_ptr;
				}
				return true;
			}
		};

		// True when the candidate was a d3d12.dll COM object (logged either way
		// it classifies); false means "not COM — maybe recurse into it".
		bool ClassifyCandidate(
			const char* a_tag,
			const std::size_t a_offset,
			const std::uintptr_t a_candidate,
			const ModuleRange& a_d3d12,
			ScanBudget& a_budget)
		{
			std::uintptr_t vtbl = 0;
			if (!Platform::SafeReadPointer(a_candidate, vtbl) || !a_d3d12.Contains(vtbl)) {
				return false;
			}
			if (!a_budget.MarkVisited(a_candidate) || a_budget.logs == 0) {
				return true;
			}

			auto* object = reinterpret_cast<void*>(a_candidate);
			void* iface = nullptr;
			if (TryQueryInterface(object, __uuidof(ID3D12GraphicsCommandList), &iface)) {
				auto* list = static_cast<ID3D12GraphicsCommandList*>(iface);
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12GraphicsCommandList 0x{:X} (type={})",
					a_tag, a_offset, a_candidate, static_cast<int>(list->GetType()));
				list->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12Resource), &iface)) {
				auto* resource = static_cast<ID3D12Resource*>(iface);
				const auto desc = resource->GetDesc();
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12Resource 0x{:X} dim={} {}x{} fmt={} ({}) flags=0x{:X}",
					a_tag, a_offset, a_candidate, static_cast<int>(desc.Dimension),
					static_cast<std::uint64_t>(desc.Width), desc.Height,
					static_cast<int>(desc.Format), FormatName(desc.Format),
					static_cast<std::uint32_t>(desc.Flags));
				resource->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12CommandQueue), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12CommandQueue 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12CommandQueue*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12CommandAllocator), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12CommandAllocator 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12CommandAllocator*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12DescriptorHeap), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12DescriptorHeap 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12DescriptorHeap*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12Object), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12Object 0x{:X} (unclassified)", a_tag, a_offset, a_candidate);
				static_cast<ID3D12Object*>(iface)->Release();
			}
			return true;
		}

		void ScanObject(
			const char* a_tag,
			const std::uintptr_t a_base,
			const std::size_t a_bytes,
			const int a_depth,
			const ModuleRange& a_d3d12,
			ScanBudget& a_budget)
		{
			char nestedTag[64];
			for (std::size_t offset = 0; offset < a_bytes; offset += sizeof(std::uintptr_t)) {
				if (a_budget.logs == 0) {
					return;
				}
				std::uintptr_t candidate = 0;
				if (!Platform::SafeReadPointer(a_base + offset, candidate) || candidate == 0) {
					continue;
				}
				if (ClassifyCandidate(a_tag, offset, candidate, a_d3d12, a_budget)) {
					continue;
				}
				// Not COM: one level of nesting covers engine wrapper structs
				// (e.g. an engine texture object holding the ID3D12Resource).
				if (a_depth > 0 && a_budget.MarkVisited(candidate) &&
					Platform::IsReadableRange(candidate, 0x100)) {
					std::snprintf(nestedTag, sizeof(nestedTag), "%s+0x%zX->", a_tag, offset);
					ScanObject(nestedTag, candidate, 0x100, a_depth - 1, a_d3d12, a_budget);
				}
			}
		}

		// ------------------------------------------------------------- engine
		[[nodiscard]] std::uintptr_t ReadHalField(const std::uint64_t a_offset)
		{
			static const REL::Relocation<std::uintptr_t*> halGlobal{ REL::ID(kIdScaleformHAL) };
			std::uintptr_t hal = 0;
			if (!Platform::SafeReadPointer(reinterpret_cast<std::uintptr_t>(halGlobal.get()), hal) || !hal) {
				return 0;
			}
			std::uintptr_t value = 0;
			(void)Platform::SafeReadPointer(hal + a_offset, value);
			return value;
		}

		[[nodiscard]] void* GetEngineCmdCtx(void* a_graphCtx)
		{
			if (!a_graphCtx) {
				return nullptr;
			}
			static const REL::Relocation<GetCmdCtxFn> getter{ REL::ID(kIdGraphCtxGetCmdCtx) };
			return CallGetterSeh(getter.get(), a_graphCtx);
		}

		// ------------------------------------------------------------- thunks
		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto count = g_begin.count.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto original = reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			// Post-original: ScaleformBegin has just bound the HAL fields.
			if (count <= kMaxDetailLogs) {
				const auto halRT = ReadHalField(kHalRTOffset);
				const auto halCtx = ReadHalField(kHalCtxOffset);
				const auto getterCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
				// getterCtx == halCtx is the runtime proof that the assumed
				// getter ABI (this-only, result in rax) is right.
				REX::INFO("[UiPassSeam] Begin[{}] tid={} ctx=0x{:X} io=0x{:X} hal+0x1410=0x{:X} hal+0x1418=0x{:X} getter(ctx)=0x{:X}{}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_ctx), reinterpret_cast<std::uintptr_t>(a_io),
					halRT, halCtx, getterCtx,
					(getterCtx && getterCtx == halCtx) ? " [getter ABI CONFIRMED]" : "");

				if (g_begin.scans.load(std::memory_order_relaxed) < kMaxScans && (halRT || halCtx)) {
					g_begin.scans.fetch_add(1, std::memory_order_relaxed);
					const auto d3d12 = LocateD3D12Module();
					if (d3d12.base) {
						ScanBudget budget;
						if (halRT && Platform::IsReadableRange(halRT, 0x100)) {
							ScanObject("halRT", halRT, 0x100, 1, d3d12, budget);
						}
						if (halCtx && Platform::IsReadableRange(halCtx, 0x200)) {
							ScanObject("halCtx", halCtx, 0x200, 1, d3d12, budget);
						}
						REX::INFO("[UiPassSeam] Begin[{}] scan done ({} log slots left)", count, budget.logs);
					}
				}
			}
			return result;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto count = g_composite.count.fetch_add(1, std::memory_order_relaxed) + 1;

			// Pre-original: this is where the future under-UI draw records, so
			// probe the state the draw would actually see.
			if (count <= kMaxDetailLogs) {
				const auto cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
				REX::INFO("[UiPassSeam] Composite[{}] tid={} this=0x{:X} ctx=0x{:X} io=0x{:X} cmdCtx=0x{:X}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_this), reinterpret_cast<std::uintptr_t>(a_ctx),
					reinterpret_cast<std::uintptr_t>(a_io), cmdCtx);

				if (g_composite.scans.load(std::memory_order_relaxed) < kMaxScans && cmdCtx) {
					g_composite.scans.fetch_add(1, std::memory_order_relaxed);
					const auto d3d12 = LocateD3D12Module();
					if (d3d12.base && Platform::IsReadableRange(cmdCtx, 0x200)) {
						ScanBudget budget;
						ScanObject("cmdCtx", cmdCtx, 0x200, 1, d3d12, budget);
						REX::INFO("[UiPassSeam] Composite[{}] scan done ({} log slots left)", count, budget.logs);
					}
				}
			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origComposite.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		// -------------------------------------------------------------- hooks
		// Fail-closed slot swap: only hook when slot 7 still holds the engine's
		// own implementation (both sides resolved through AddrLib, so the guard
		// survives game patches that shift addresses). A foreign value means a
		// game-patch layout change or another mod hooked first — leave it alone
		// and say so, rather than chain onto an unknown ABI.
		[[nodiscard]] std::uintptr_t HookExecuteSlot(
			const char* a_label,
			const std::uint64_t a_vtblId,
			const std::uint64_t a_implId,
			ExecuteFn a_thunk)
		{
			const REL::Relocation<std::uintptr_t> vtbl{ REL::ID(a_vtblId) };
			const REL::Relocation<std::uintptr_t> expected{ REL::ID(a_implId) };
			const auto slotAddress = vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);

			std::uintptr_t current = 0;
			if (!Platform::SafeReadPointer(slotAddress, current)) {
				REX::WARN("[UiPassSeam] {}: vtable slot at 0x{:X} unreadable; not hooking", a_label, slotAddress);
				return 0;
			}
			if (current != expected.address()) {
				REX::WARN("[UiPassSeam] {}: slot 7 holds 0x{:X}, expected 0x{:X} (game patch or foreign hook); not hooking",
					a_label, current, expected.address());
				return 0;
			}

			auto** slot = reinterpret_cast<void**>(slotAddress);
			DWORD oldProtect = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				REX::WARN("[UiPassSeam] {}: VirtualProtect failed; not hooking", a_label);
				return 0;
			}
			*slot = reinterpret_cast<void*>(a_thunk);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			REX::INFO("[UiPassSeam] hooked {} slot 7 (vtbl 0x{:X}, original 0x{:X})", a_label, vtbl.address(), current);
			return current;
		}
	}

	bool Install()
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return true;
		}

		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin", kVtblScaleformBegin, kIdBeginExecute, &BeginThunk);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite", kVtblScaleformComposite, kIdCompositeExecute, &CompositeThunk);
		g_origBegin.store(origBegin, std::memory_order_relaxed);
		g_origComposite.store(origComposite, std::memory_order_relaxed);

		const bool ok = origBegin != 0 && origComposite != 0;
		if (!ok) {
			REX::WARN("[UiPassSeam] probe incomplete — see slot warnings above (unhooked passes run untouched)");
		}
		return ok;
	}
}
