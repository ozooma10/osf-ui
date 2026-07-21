#include "composite/UiPassSeam.h"

#include "composite/EngineD3D12.h"
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

		// The blobs being scanned are raw worker-stack memory, so most qwords
		// are not pointers at all (counters, -1 sentinels, flag words). Filter
		// to the user-mode canonical range before spending a VirtualQuery on
		// them — and before arithmetic that could wrap (a -1 "pointer" plus a
		// size overflowed the pre-fix IsReadableRange into a false positive:
		// the probe's one field crash).
		[[nodiscard]] bool IsPlausiblePointer(const std::uintptr_t a_candidate)
		{
			return a_candidate >= 0x10000 && a_candidate < 0x0000'8000'0000'0000;
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
				if (!Platform::SafeReadPointer(a_base + offset, candidate) || !IsPlausiblePointer(candidate)) {
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

		// ------------------------------------------------- D3D12 call capture
		// The blind scans found no D3D12 interfaces reachable from the engine
		// command context (first run: zero hits at 0x200/1-hop), so instead of
		// guessing offsets, catch the engine in the act: hook the process-wide
		// ID3D12GraphicsCommandList vtable (obtained from a throwaway list
		// created on the game's own device — same trick as the Present hook's
		// throwaway swapchain) and log, ONLY while ScaleformComposite's
		// original Execute runs on the bracket thread, which list sets render
		// targets and which resources get barrier-transitioned. That yields
		// the live list pointer (searched back through the ctx object to
		// derive its offset) and the composite target's format via GetDesc.
		//
		// Slot indices are fixed COM ABI, straight from the d3d12.h C vtable
		// (ID3D12GraphicsCommandListVtbl): 26 = ResourceBarrier,
		// 46 = OMSetRenderTargets. Trust but verify: after hooking, each slot
		// is self-tested by calling it on our own throwaway list and checking
		// the thunk observed the sentinel — a wrong index unhooks itself.
		constexpr std::size_t kSlotResourceBarrier = 26;
		constexpr std::size_t kSlotOMSetRenderTargets = 46;
		constexpr std::uint32_t kMaxCaptureBrackets = 6;  // composite brackets to log

		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
			const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

		std::atomic<OMSetRenderTargetsFn> g_origOMSetRenderTargets{ nullptr };
		std::atomic<ResourceBarrierFn> g_origResourceBarrier{ nullptr };
		std::atomic<std::uint32_t> g_captureTid{ 0 };       // bracket: composite worker tid, else 0
		std::atomic<std::uintptr_t> g_capturedList{ 0 };    // last list seen inside a bracket
		std::atomic<std::uint32_t> g_captureBrackets{ 0 };  // brackets logged so far
		std::atomic<int> g_captureState{ 0 };               // 0 untried, 1 ready, -1 failed
		ID3D12GraphicsCommandList* g_selfTestList = nullptr;  // non-null only during self-test
		std::atomic<bool> g_selfTestOMSeen{ false };
		std::atomic<bool> g_selfTestBarrierSeen{ false };

		void STDMETHODCALLTYPE OMSetRenderTargetsThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_numRTs,
			const D3D12_CPU_DESCRIPTOR_HANDLE* a_rts,
			const BOOL a_singleRange,
			const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
		{
			if (a_self == g_selfTestList) {
				g_selfTestOMSeen.store(true, std::memory_order_relaxed);
			} else if (g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId()) {
				g_capturedList.store(reinterpret_cast<std::uintptr_t>(a_self), std::memory_order_relaxed);
				REX::INFO("[UiPassSeam] capture OMSetRenderTargets list=0x{:X} numRTs={} rtv0=0x{:X} dsv={}",
					reinterpret_cast<std::uintptr_t>(a_self), a_numRTs,
					(a_rts && a_numRTs > 0) ? a_rts[0].ptr : 0,
					a_dsv ? "yes" : "no");
			}
			if (const auto original = g_origOMSetRenderTargets.load(std::memory_order_relaxed)) {
				original(a_self, a_numRTs, a_rts, a_singleRange, a_dsv);
			}
		}

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_numBarriers,
			const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			if (a_self == g_selfTestList) {
				g_selfTestBarrierSeen.store(true, std::memory_order_relaxed);
			} else if (g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId()) {
				g_capturedList.store(reinterpret_cast<std::uintptr_t>(a_self), std::memory_order_relaxed);
				const auto count = a_barriers ? (a_numBarriers < 8u ? a_numBarriers : 8u) : 0u;
				for (UINT i = 0; i < count; ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !barrier.Transition.pResource) {
						continue;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					REX::INFO("[UiPassSeam] capture barrier list=0x{:X} res=0x{:X} {}x{} fmt={} ({}) states 0x{:X}->0x{:X}",
						reinterpret_cast<std::uintptr_t>(a_self),
						reinterpret_cast<std::uintptr_t>(barrier.Transition.pResource),
						static_cast<std::uint64_t>(desc.Width), desc.Height,
						static_cast<int>(desc.Format), FormatName(desc.Format),
						static_cast<std::uint32_t>(barrier.Transition.StateBefore),
						static_cast<std::uint32_t>(barrier.Transition.StateAfter));
				}
			}
			if (const auto original = g_origResourceBarrier.load(std::memory_order_relaxed)) {
				original(a_self, a_numBarriers, a_barriers);
			}
		}

		[[nodiscard]] void* PatchSlot(void** a_vtbl, const std::size_t a_slot, void* a_thunk)
		{
			DWORD oldProtect = 0;
			if (!::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return nullptr;
			}
			void* original = a_vtbl[a_slot];
			a_vtbl[a_slot] = a_thunk;
			::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), oldProtect, &oldProtect);
			return original;
		}

		// Lazy, once. Creates a throwaway DIRECT list on the game's device to
		// reach the shared d3d12.dll vtable, hooks the two slots, and proves
		// the slot indices by calling both methods on the throwaway list
		// (OMSetRenderTargets with zero targets and a global-UAV barrier are
		// both legal no-ops). A failed self-test restores the slots.
		void EnsureCaptureInstalled()
		{
			int expected = 0;
			if (!g_captureState.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
				return;  // already tried (state is 1 or -1); never retry
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				REX::WARN("[UiPassSeam] capture: engine D3D12 device not reachable; skipping call capture");
				return;
			}

			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			auto* device = reinterpret_cast<ID3D12Device*>(engine.device);
			const bool created =
				SUCCEEDED(device->CreateCommandAllocator(
					D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
					reinterpret_cast<void**>(&allocator))) &&
				SUCCEEDED(device->CreateCommandList(
					0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
					__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list)));

			if (created) {
				auto** vtbl = *reinterpret_cast<void***>(list);
				g_selfTestList = list;
				const auto origOM = PatchSlot(vtbl, kSlotOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRenderTargetsThunk));
				const auto origBarrier = PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(&ResourceBarrierThunk));
				g_origOMSetRenderTargets.store(reinterpret_cast<OMSetRenderTargetsFn>(origOM), std::memory_order_relaxed);
				g_origResourceBarrier.store(reinterpret_cast<ResourceBarrierFn>(origBarrier), std::memory_order_relaxed);

				if (origOM && origBarrier) {
					list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
					D3D12_RESOURCE_BARRIER uav{};
					uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uav.UAV.pResource = nullptr;  // global UAV barrier: legal on any list
					list->ResourceBarrier(1, &uav);
				}

				const bool omOk = g_selfTestOMSeen.load(std::memory_order_relaxed);
				const bool barrierOk = g_selfTestBarrierSeen.load(std::memory_order_relaxed);
				if (origOM && origBarrier && omOk && barrierOk) {
					g_selfTestList = nullptr;
					g_captureState.store(1, std::memory_order_release);
					REX::INFO("[UiPassSeam] capture armed: ID3D12GraphicsCommandList vtable slots {} (barrier) / {} (OMSetRenderTargets) hooked and self-tested",
						kSlotResourceBarrier, kSlotOMSetRenderTargets);
				} else {
					// Wrong slot layout or protect failure: undo what we did.
					if (origOM) {
						(void)PatchSlot(vtbl, kSlotOMSetRenderTargets, origOM);
					}
					if (origBarrier) {
						(void)PatchSlot(vtbl, kSlotResourceBarrier, origBarrier);
					}
					g_origOMSetRenderTargets.store(nullptr, std::memory_order_relaxed);
					g_origResourceBarrier.store(nullptr, std::memory_order_relaxed);
					g_selfTestList = nullptr;
					REX::WARN("[UiPassSeam] capture self-test FAILED (patch={}/{} seen={}/{}); vtable restored, call capture disabled",
						origOM != nullptr, origBarrier != nullptr, omOk, barrierOk);
				}
				list->Close();
			} else {
				REX::WARN("[UiPassSeam] capture: throwaway command list creation failed; skipping call capture");
			}

			if (list) {
				list->Release();
			}
			if (allocator) {
				allocator->Release();
			}
			engine.directQueue->Release();
			engine.device->Release();
		}

		// After a captured bracket: where does the engine ctx keep that list?
		void ReportListOffset(const char* a_tag, const std::uintptr_t a_object, const std::uintptr_t a_list)
		{
			if (!a_object) {
				return;
			}
			bool found = false;
			for (std::size_t offset = 0; offset < 0x1000; offset += sizeof(std::uintptr_t)) {
				std::uintptr_t value = 0;
				if (Platform::SafeReadPointer(a_object + offset, value) && value == a_list) {
					REX::INFO("[UiPassSeam] capture: {}+0x{:X} holds the recording ID3D12GraphicsCommandList", a_tag, offset);
					found = true;
				}
			}
			if (!found) {
				REX::INFO("[UiPassSeam] capture: list 0x{:X} not stored in {}'s first 0x1000 bytes", a_list, a_tag);
			}
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
			std::uintptr_t cmdCtx = 0;
			if (count <= kMaxDetailLogs) {
				cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
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

			// Bracket the original Execute with D3D12 call capture on this
			// thread: the engine records the composite via this very ctx, so
			// whatever list its calls land on IS the one the draw path needs.
			EnsureCaptureInstalled();
			const bool capture =
				g_captureState.load(std::memory_order_acquire) == 1 &&
				g_captureBrackets.load(std::memory_order_relaxed) < kMaxCaptureBrackets;
			if (capture) {
				g_capturedList.store(0, std::memory_order_relaxed);
				g_captureTid.store(::GetCurrentThreadId(), std::memory_order_release);
			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origComposite.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			if (capture) {
				g_captureTid.store(0, std::memory_order_release);
				const auto bracket = g_captureBrackets.fetch_add(1, std::memory_order_relaxed) + 1;
				const auto list = g_capturedList.load(std::memory_order_relaxed);
				if (list) {
					if (!cmdCtx) {
						cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
					}
					ReportListOffset("cmdCtx", cmdCtx, list);
					ReportListOffset("graphCtx", reinterpret_cast<std::uintptr_t>(a_ctx), list);
				} else {
					// No D3D12 calls seen: the early-skip path (UI buffer
					// untouched) — expected on frames with no live movies.
					REX::INFO("[UiPassSeam] capture bracket {}: no OMSetRenderTargets/barrier calls (composite skipped?)", bracket);
				}
			}
			return result;
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
