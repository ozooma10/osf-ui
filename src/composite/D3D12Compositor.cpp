#include "composite/D3D12Compositor.h"

#include "composite/EngineD3D12.h"
#include "core/Log.h"
#include "platform/WindowsPlatform.h"

// GDI-free Win32/D3D12 so the ERROR macro never collides with REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <intrin.h>  // _ReturnAddress: classify who invokes our Present thunk

#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

namespace OSFUI
{
	namespace
	{
		constexpr std::uint32_t kMaxSwapchains = 4;  // distinct swapchains we track for discovery

		template <class T>
		void SafeRelease(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		// Coexistence diagnostic: which module does a code address live in?
		// Logs who owns the Present slot before we hook it — if ReShade/RTSS/
		// Steam overlay hooked first, their module shows up instead of dxgi.dll.
		[[nodiscard]] std::string ModuleNameOfAddress(const void* a_address)
		{
			HMODULE mod = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &mod) ||
				!mod) {
				return "<unknown module>";
			}
			wchar_t widePath[MAX_PATH]{};
			const auto len = ::GetModuleFileNameW(mod, widePath, MAX_PATH);
			if (len == 0) {
				return "<unknown module>";
			}
			char utf8[MAX_PATH * 3]{};
			const auto written = ::WideCharToMultiByte(CP_UTF8, 0, widePath, static_cast<int>(len),
				utf8, sizeof(utf8) - 1, nullptr, nullptr);
			if (written <= 0) {
				return "<unknown module>";
			}
			return std::string(utf8, static_cast<std::size_t>(written));
		}

		// Bare lowercase filename of a module path, for prefix matching.
		[[nodiscard]] std::string ModuleFileNameLower(const std::string& a_path)
		{
			const auto sep = a_path.find_last_of("\\/");
			auto name = sep == std::string::npos ? a_path : a_path.substr(sep + 1);
			for (auto& c : name) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return name;
		}

		// Starfield statically links AMD FidelityFX FSR3 *and exports its C
		// api*, so the frame-interpolation swapchain implementation can be
		// bounded from the exe's own export table — no engine offsets. The
		// FrameInterpolationSwapChainDX12 methods themselves (including the
		// pacing thread that presents the real swapchain when Frame Generation
		// is on) are unexported, but they sit between these exports in the
		// image (proven from a 2026-07-21 trainwreck stack: internal frames at
		// +3512F07..+35156A5 on 1.16.244, inside the export span below). The
		// padding covers unexported neighbors from the same translation units;
		// the surrounding code on both sides is still FidelityFX/AGS, so the
		// padded range cannot swallow game render-loop call sites. Resolves to
		// {0,0} (detection off) if a future patch stops exporting these.
		struct FfxFrameInterpRegion
		{
			std::uintptr_t lo{ 0 };
			std::uintptr_t hi{ 0 };
		};
		[[nodiscard]] FfxFrameInterpRegion ResolveFfxFrameInterpRegion()
		{
			static constexpr const char* kExports[] = {
				"ffxFsr3SkipPresent",
				"ffxFsr3DispatchFrameGeneration",
				"ffxCreateFrameinterpolationSwapchainDX12",
				"ffxCreateFrameinterpolationSwapchainForHwndDX12",
				"ffxGetFrameinterpolationCommandlistDX12",
				"ffxGetFrameinterpolationTextureDX12",
				"ffxRegisterFrameinterpolationUiResourceDX12",
				"ffxReplaceSwapchainForFrameinterpolationDX12",
				"ffxSetFrameGenerationConfigToSwapchainDX12",
				"ffxWaitForPresents",
			};
			constexpr std::uintptr_t kPad = 0x10000;

			const HMODULE exe = ::GetModuleHandleW(nullptr);
			std::uintptr_t lo = 0;
			std::uintptr_t hi = 0;
			int found = 0;
			for (const auto* name : kExports) {
				const auto addr = reinterpret_cast<std::uintptr_t>(::GetProcAddress(exe, name));
				if (!addr) {
					continue;
				}
				++found;
				lo = lo == 0 ? addr : (std::min)(lo, addr);
				hi = (std::max)(hi, addr);
			}
			if (found < 4) {  // export surface changed; don't guess from a sliver
				return {};
			}
			return { lo - kPad, hi + kPad };
		}

		// A vtable-slot read cannot see tools that hook Present by patching the
		// function's first bytes (MinHook-style — BetterConsole does this): the
		// slot still points into dxgi.dll while dxgi!Present's first instruction
		// jumps into the tool. Follow rel32 and [rip+disp32] JMPs (bounded,
		// readability-guarded) to where the code actually lands.
		[[nodiscard]] const void* FollowInlineJmps(const void* a_code)
		{
			auto* code = static_cast<const std::uint8_t*>(a_code);
			for (int depth = 0; depth < 8; ++depth) {
				if (!Platform::IsReadableRange(reinterpret_cast<std::uintptr_t>(code), 6)) {
					break;
				}
				if (code[0] == 0xE9) {  // jmp rel32
					std::int32_t disp = 0;
					std::memcpy(&disp, code + 1, sizeof(disp));
					code += 5 + static_cast<std::ptrdiff_t>(disp);
					continue;
				}
				if (code[0] == 0xFF && code[1] == 0x25) {  // jmp [rip+disp32]
					std::int32_t disp = 0;
					std::memcpy(&disp, code + 2, sizeof(disp));
					std::uintptr_t target = 0;
					if (!Platform::SafeReadPointer(reinterpret_cast<std::uintptr_t>(code) + 6 + disp, target) || target == 0) {
						break;
					}
					code = reinterpret_cast<const std::uint8_t*>(target);
					continue;
				}
				break;
			}
			return code;
		}

		// Fullscreen triangle from SV_VertexID (no vertex buffer). UV (0,0) is
		// the top-left so the texture's row 0 lands at the top of the screen.
		constexpr const char* kVertexShader = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

		// The overlay texture is BGRA8 premultiplied alpha. Sample straight
		// through; the premultiplied-over blend is configured in the PSO.
		constexpr const char* kPixelShader = R"(
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return gTex.Sample(gSmp, uv);
}
)";


		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		// Process-static so the thunk keeps working even if the compositor
		// Impl is torn down (the vtable hook is one-way; we never unhook).
		std::atomic<void*>     g_overlay{ nullptr };  // D3D12Compositor::Impl*
		std::atomic<PresentFn> g_originalPresent{ nullptr };

		// Bridge for the seam-draw hook: Impl is private to the class, so the
		// free function RecordSeamOverlayDraw goes through a pointer that only
		// Impl (which can name itself) installs at setup.
		using SeamDrawFn = bool (*)(ID3D12GraphicsCommandList*, ID3D12Resource*, bool, bool);
		std::atomic<SeamDrawFn> g_seamDrawFn{ nullptr };

		[[nodiscard]] ID3DBlob* CompileShader(const char* a_src, const char* a_entry, const char* a_target)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			const auto hr = ::D3DCompile(a_src, std::strlen(a_src), nullptr, nullptr, nullptr,
				a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				REX::ERROR("D3D12Compositor: shader '{}' compile failed (hr=0x{:08X}): {}",
					a_target, static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			}
			SafeRelease(errors);
			return code;
		}
	}

	struct D3D12Compositor::Impl
	{
		EngineD3D12   engine{};
		std::atomic_bool visible{ false };

		// Output-size signal -> runtime (resize the view to match the screen).
		OutputResizeCallback onOutputResize;
		std::uint32_t        notifiedOutputW{ 0 };
		std::uint32_t        notifiedOutputH{ 0 };
		std::atomic_bool     outputSizeKnown{ false };

		bool setupAttempted{ false };
		bool setupOk{ false };

		// GPU shared-ring transport (out-of-process WebView2 host). SetSharedRing
		// (game thread) parks the announced ring here; the present thread adopts
		// it lazily (EnsureSharedRing) because opening the handles needs the
		// located engine device. The compositor owns the handles from
		// SetSharedRing on.
		//
		// All drawing happens at the engine seam: UiPassSeam records the overlay
		// quad into Starfield's transparent UI layer via RecordSeamOverlay below.
		// The Present hook draws nothing (see OnPresent). ringMutex guards the
		// opened ring (sharedSlots/fences/SRVs) between the present thread
		// (adoption) and the seam's render workers (sampling).
		std::atomic<bool> seamMode{ false };
		std::mutex        ringMutex;
		static constexpr std::uint32_t kSeamRtvSlots = 8;
		ID3D12DescriptorHeap*      seamRtvHeap{ nullptr };  // typed RTVs onto the engine's (typeless) UI buffers
		std::atomic<std::uint32_t> seamRtvNext{ 0 };
		std::uint64_t seamLastConsumeSignaled{ 0 };  // ringMutex
		std::atomic<std::uint64_t> seamDraws{ 0 };
		std::atomic<std::uint64_t> seamDrawsFgTarget{ 0 };  // diagnostics
		bool          noSharedFrameLogged{ false };  // ringMutex
		// Newest slot whose produce fence is CPU-verified complete. The seam
		// cannot queue-wait on the host's fence (not our queue), and skipping
		// incomplete frames flickers under rapid production (mouse-move
		// repaints, 2026-07-21) — so an incomplete newest frame falls back to
		// this one instead: one frame stale, never absent.
		std::uint32_t seamReadySlot{ 0 };    // ringMutex
		std::uint64_t seamReadySerial{ 0 };  // ringMutex
		// The previous ring generation is retired, not released, on adoption:
		// seam draws live inside ENGINE command lists that our idle fence
		// cannot cover, so the old textures must outlive one more adoption.
		ID3D12Resource* retiredSlots[SharedRingDesc::kMaxSlots]{};
		ID3D12Fence*    retiredProduce{ nullptr };
		ID3D12Fence*    retiredConsume{ nullptr };

		std::mutex     sharedMutex;
		SharedRingDesc sharedPending{};
		bool           sharedDirty{ false };
		// present-thread-only, opened on the engine device:
		ID3D12Resource* sharedSlots[SharedRingDesc::kMaxSlots]{};
		std::uint32_t   sharedSlotCount{ 0 };
		ID3D12Fence*    sharedProduce{ nullptr };
		ID3D12Fence*    sharedConsume{ nullptr };
		std::uint64_t   sharedGeneration{ 0 };
		bool            sharedOpenFailed{ false };
		// Latest published ring frame (guarded by frameMutex). sharedFrameReady
		// stays false until the host has submitted its first shared-slot frame.
		std::mutex    frameMutex;
		std::uint64_t lastSubmittedIndex{ 0 };
		bool          sharedFrameReady{ false };
		std::uint32_t gpuSlot{ 0 };
		std::uint64_t gpuSerial{ 0 };
		std::uint64_t gpuSourceTimeMs{ 0 };
		bool          cpuFrameWarned{ false };  // Submit thread only

		// Shared GPU objects, created once.
		ID3D12Fence*          fence{ nullptr };
		HANDLE                fenceEvent{ nullptr };
		std::uint64_t         nextFenceValue{ 1 };
		ID3D12RootSignature*  rootSig{ nullptr };
		// One PSO. The seam always renders through a typed R8G8B8A8_UNORM view
		// onto the engine's UI buffer, so unlike the retired backbuffer draw
		// there is no per-swapchain format to match — and HDR or _SRGB
		// backbuffers no longer suppress the overlay.
		ID3D12PipelineState*  seamPso{ nullptr };
		bool                  seamPsoFailed{ false };  // ringMutex; don't retry+spam
		// shader-visible: slot 0 unused, 1..kMaxSlots = shared ring
		ID3D12DescriptorHeap* srvHeap{ nullptr };
		std::uint32_t         rtvStride{ 0 };
		std::uint32_t         srvStride{ 0 };
		ID3DBlob*             vsBlob{ nullptr };
		ID3DBlob*             psBlob{ nullptr };

		// Per-swapchain discovery state. The swapchain is not ref'd: a lasting
		// ref keeps a game-released swapchain alive, its HWND stays associated,
		// and the game's next CreateSwapChainForHwnd on that window fails —
		// Starfield's built-in FSR3 frame-interpolation swapchain creation
		// null-derefs on exactly that failure (external CTD, 2026-07-21, crash
		// inside ffxCreateFrameinterpolationSwapchainForHwndDX12). `swap` is
		// borrowed: refreshed by AcquireTarget on every present and only
		// dereferenced below that call in the same present, while the presenting
		// swapchain is guaranteed alive by its own Present call.
		struct Target
		{
			IDXGISwapChain3* swap{ nullptr };  // borrowed (NOT ref'd), key is the raw self ptr
			std::uintptr_t   key{ 0 };
			std::uint32_t    width{ 0 };
			std::uint32_t    height{ 0 };
			bool             seen{ false };  // dims known yet?
			// Who calls Present on this swapchain (immediate return address of
			// our thunk, re-resolved to a module when the call site changes).
			// Two Frame Generation pacers are recognized, and fgDriven targets
			// tell the seam that the frame graph currently interpolates — which
			// changes which UI hand-off is safe to decorate:
			//  - sl.dlss_g.dll (NVIDIA DLSS-FG via Streamline). sl.interposer
			//    alone is NOT a trigger: every vanilla install ships it.
			//  - the exe's own statically-linked FSR3 frame-interpolation
			//    swapchain, detected by return address inside the export-bounded
			//    ffx region — its module is Starfield.exe, so a module-name
			//    check can't see it.
			const void*      callerRet{ nullptr };
			bool             fgDriven{ false };
			bool             callerLogged{ false };
			// Last present tick, for evicting dead entries: swapchain
			// recreation (FG toggle, display-mode change) churns pointers, and
			// without eviction the fixed table fills with keys that never
			// present again.
			std::uint64_t    lastSeenMs{ 0 };
		};
		Target targets[kMaxSwapchains]{};

		// Address range of the exe's built-in FSR3 frame-interpolation code
		// (resolved once in EnsureSetup; {0,0} = undetectable). See
		// ResolveFfxFrameInterpRegion.
		FfxFrameInterpRegion ffxFrameInterp{};

		// Present threads publish FG liveness for seam render workers. With FG
		// active, the first RT->pixel-SRV match is an opaque scene buffer, not
		// the transparent Scaleform UI layer; only the RT->COPY_SOURCE hand-off
		// is safe to decorate (it later feeds both the real composite and FFX).
		std::atomic_bool frameGenActiveSignal{ false };
		std::atomic_bool seamFgLayerOnlyLogged{ false };

		// Single-flight gate for OnPresent. With Frame Generation active the
		// real swapchain presents from FG's pacing thread concurrently with the
		// game thread's presents, and the discovery state OnPresent touches
		// (targets, caller classification) is single-thread state. An
		// overlapped present skips discovery instead of corrupting it.
		std::atomic_bool           presentBusy{ false };
		std::atomic<std::uint64_t> skippedConcurrent{ 0 };
		std::atomic_bool           concurrentWarned{ false };
		bool          targetsFullLogged{ false };

		// Opt-in counters behind osfui.renderStats. These span the game tick,
		// present and UI-seam render threads, so every field read by Runtime is
		// atomic. Normal play leaves the gate false and pays only one relaxed load
		// at each candidate event.
		std::atomic_bool           renderStatsEnabled{ false };
		std::atomic<std::uint64_t> statsPresents{ 0 };
		std::atomic<std::uint64_t> statsDraws{ 0 };
		std::atomic<std::uint64_t> statsFreshFrames{ 0 };
		std::atomic<std::uint64_t> statsReusedDraws{ 0 };
		std::atomic<std::uint64_t> statsSubmits{ 0 };
		std::atomic<std::uint64_t> statsLastSerial{ 0 };
		std::atomic<std::uint64_t> statsSourceToDrawMsTotal{ 0 };
		std::atomic<std::uint64_t> statsSourceToDrawSamples{ 0 };
		std::atomic<std::uint64_t> statsRecordCpuUsTotal{ 0 };
		std::atomic<std::uint64_t> statsRecordCpuSamples{ 0 };

		// Hook-liveness watchdog. Another overlay that re-hooks Present after us
		// without chaining silently stops our thunk from running — output-size
		// discovery and FG classification then freeze. PresentThunk stamps this
		// every call (present thread); CheckPresentLiveness (tick thread) warns
		// when the game keeps ticking but no present has reached us. No false
		// positive on focus loss: the game pauses the tick loop too, so the
		// watchdog isn't polled then.
		std::atomic<std::uint64_t> lastPresentMs{ 0 };
		std::uint64_t              setupCompletedMs{ 0 };  // tick thread only
		bool                       bypassWarned{ false };  // tick thread only

		~Impl()
		{
			g_overlay.store(nullptr);
			g_seamDrawFn.store(nullptr, std::memory_order_release);
			WaitForGpuIdle();
			ReleaseSharedRing();
			{
				std::scoped_lock lk(sharedMutex);
				if (sharedDirty) {
					CloseRingHandles(sharedPending);
					sharedDirty = false;
				}
			}
			SafeRelease(seamPso);
			SafeRelease(rootSig);
			SafeRelease(srvHeap);
			SafeRelease(seamRtvHeap);
			SafeRelease(vsBlob);
			SafeRelease(psBlob);
			SafeRelease(fence);
			if (fenceEvent) {
				::CloseHandle(fenceEvent);
			}
			SafeRelease(engine.directQueue);
			SafeRelease(engine.device);
		}

		void CountRenderStatsDraw(const std::uint64_t a_serial,
			const std::uint64_t a_sourceTimeMs,
			const std::chrono::steady_clock::time_point a_started)
		{
			if (!renderStatsEnabled.load(std::memory_order_relaxed)) return;
			statsDraws.fetch_add(1, std::memory_order_relaxed);
			if (statsLastSerial.exchange(a_serial, std::memory_order_relaxed) != a_serial) {
				statsFreshFrames.fetch_add(1, std::memory_order_relaxed);
				const auto now = ::GetTickCount64();
				if (a_sourceTimeMs != 0 && now >= a_sourceTimeMs) {
					statsSourceToDrawMsTotal.fetch_add(now - a_sourceTimeMs, std::memory_order_relaxed);
					statsSourceToDrawSamples.fetch_add(1, std::memory_order_relaxed);
				}
			} else {
				statsReusedDraws.fetch_add(1, std::memory_order_relaxed);
			}
			const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - a_started).count();
			statsRecordCpuUsTotal.fetch_add(static_cast<std::uint64_t>((std::max)(us, 0ll)),
				std::memory_order_relaxed);
			statsRecordCpuSamples.fetch_add(1, std::memory_order_relaxed);
		}

		void SetRenderStatsEnabled(const bool a_enabled)
		{
			renderStatsEnabled.store(false, std::memory_order_relaxed);
			if (!a_enabled) return;
			statsPresents.store(0, std::memory_order_relaxed);
			statsDraws.store(0, std::memory_order_relaxed);
			statsFreshFrames.store(0, std::memory_order_relaxed);
			statsReusedDraws.store(0, std::memory_order_relaxed);
			statsSubmits.store(0, std::memory_order_relaxed);
			statsLastSerial.store(0, std::memory_order_relaxed);
			statsSourceToDrawMsTotal.store(0, std::memory_order_relaxed);
			statsSourceToDrawSamples.store(0, std::memory_order_relaxed);
			statsRecordCpuUsTotal.store(0, std::memory_order_relaxed);
			statsRecordCpuSamples.store(0, std::memory_order_relaxed);
			renderStatsEnabled.store(true, std::memory_order_release);
		}

		// busyWaits/droppedBusy belonged to the retired present-time draw ring
		// and stay at their zero defaults; the wire format keeps the fields so
		// the host's diagnostics page needs no version dance.
		[[nodiscard]] CompositorStats GetRenderStats() const
		{
			return {
				.presents = statsPresents.load(std::memory_order_relaxed),
				.draws = statsDraws.load(std::memory_order_relaxed),
				.freshFrames = statsFreshFrames.load(std::memory_order_relaxed),
				.reusedDraws = statsReusedDraws.load(std::memory_order_relaxed),
				.submits = statsSubmits.load(std::memory_order_relaxed),
				.skippedConcurrent = skippedConcurrent.load(std::memory_order_relaxed),
				.sourceToDrawMsTotal = statsSourceToDrawMsTotal.load(std::memory_order_relaxed),
				.sourceToDrawSamples = statsSourceToDrawSamples.load(std::memory_order_relaxed),
				.recordCpuUsTotal = statsRecordCpuUsTotal.load(std::memory_order_relaxed),
				.recordCpuSamples = statsRecordCpuSamples.load(std::memory_order_relaxed),
				.seamMode = seamMode.load(std::memory_order_relaxed),
				.frameGeneration = frameGenActiveSignal.load(std::memory_order_relaxed),
			};
		}

		// Drain the engine's DIRECT queue up to this point. We submit no work of
		// our own any more, but seam draws ride ENGINE command lists on this
		// queue, so this is what makes retiring a ring generation safe.
		void WaitForGpuIdle()
		{
			if (!fence || !fenceEvent || !engine.directQueue) {
				return;
			}
			const auto value = nextFenceValue++;
			if (SUCCEEDED(engine.directQueue->Signal(fence, value)) && fence->GetCompletedValue() < value) {
				fence->SetEventOnCompletion(value, fenceEvent);
				::WaitForSingleObject(fenceEvent, 2000);
			}
		}

		static void CloseRingHandles(SharedRingDesc& a_desc)
		{
			for (auto*& handle : a_desc.slotHandles) {
				if (handle) {
					::CloseHandle(handle);
					handle = nullptr;
				}
			}
			if (a_desc.produceFence) {
				::CloseHandle(a_desc.produceFence);
				a_desc.produceFence = nullptr;
			}
			if (a_desc.consumeFence) {
				::CloseHandle(a_desc.consumeFence);
				a_desc.consumeFence = nullptr;
			}
		}

		// Adoption path: previous generation moves to the retirement slots
		// (freeing whatever was retired before), so engine lists recorded just
		// before a re-announce still reference live textures.
		void RetireSharedRing()
		{
			for (auto*& slot : retiredSlots) {
				SafeRelease(slot);
			}
			SafeRelease(retiredProduce);
			SafeRelease(retiredConsume);
			for (std::size_t i = 0; i < SharedRingDesc::kMaxSlots; ++i) {
				retiredSlots[i] = sharedSlots[i];
				sharedSlots[i] = nullptr;
			}
			retiredProduce = sharedProduce;
			sharedProduce = nullptr;
			retiredConsume = sharedConsume;
			sharedConsume = nullptr;
			sharedSlotCount = 0;
			seamReadySlot = 0;  // slots of the old generation are gone
			seamReadySerial = 0;
		}

		void ReleaseSharedRing()
		{
			for (auto*& slot : retiredSlots) {
				SafeRelease(slot);
			}
			SafeRelease(retiredProduce);
			SafeRelease(retiredConsume);
			for (auto*& slot : sharedSlots) {
				SafeRelease(slot);
			}
			sharedSlotCount = 0;
			SafeRelease(sharedProduce);
			SafeRelease(sharedConsume);
		}

		// Game thread. The compositor owns the handles from here on.
		void SetSharedRing(const SharedRingDesc& a_desc)
		{
			std::scoped_lock lk(sharedMutex);
			if (sharedDirty) {
				CloseRingHandles(sharedPending);  // superseded before adoption
			}
			sharedPending = a_desc;
			sharedDirty = true;
			sharedOpenFailed = false;
		}

		// Present thread: adopt the latest announced ring. Returns true when a
		// usable ring is open.
		[[nodiscard]] bool EnsureSharedRing()
		{
			SharedRingDesc pending{};
			{
				std::scoped_lock lk(sharedMutex);
				if (!sharedDirty) {
					return sharedSlots[0] != nullptr && !sharedOpenFailed;
				}
				pending = sharedPending;
				sharedPending = {};
				sharedDirty = false;
			}
			// Old slots may still be referenced by in-flight ENGINE lists
			// carrying seam draws; the queue drain plus one generation of
			// retirement covers them. ringMutex: the seam worker must not
			// sample mid-swap.
			WaitForGpuIdle();
			std::scoped_lock ring(ringMutex);
			RetireSharedRing();

			auto* dev = engine.device;
			bool ok = pending.slotCount > 0 &&
			          pending.slotCount <= SharedRingDesc::kMaxSlots;
			HRESULT openHr = ok ? S_OK : E_INVALIDARG;
			const char* openObject = "ring metadata";
			int openSlot = -1;
			for (std::size_t i = 0; ok && i < pending.slotCount; ++i) {
				openObject = "texture";
				openSlot = static_cast<int>(i);
				if (!pending.slotHandles[i]) {
					openHr = E_HANDLE;
					ok = false;
				} else {
					openHr = dev->OpenSharedHandle(pending.slotHandles[i],
						__uuidof(ID3D12Resource), reinterpret_cast<void**>(&sharedSlots[i]));
					ok = SUCCEEDED(openHr);
				}
			}
			if (ok) {
				openObject = "produce fence";
				openSlot = -1;
				openHr = pending.produceFence ? dev->OpenSharedHandle(pending.produceFence,
					__uuidof(ID3D12Fence), reinterpret_cast<void**>(&sharedProduce)) : E_HANDLE;
				ok = SUCCEEDED(openHr);
			}
			if (ok) {
				openObject = "consume fence";
				openHr = pending.consumeFence ? dev->OpenSharedHandle(pending.consumeFence,
					__uuidof(ID3D12Fence), reinterpret_cast<void**>(&sharedConsume)) : E_HANDLE;
				ok = SUCCEEDED(openHr);
			}
			CloseRingHandles(pending);
			if (!ok) {
				const auto gameLuid = dev->GetAdapterLuid();
				REX::ERROR("D3D12Compositor: OpenSharedHandle failed for {} (slot {}, hr=0x{:08X}); "
					"game adapter LUID 0x{:08X}:0x{:08X}, host adapter LUID 0x{:08X}:0x{:08X} — "
					"GPU frames from the WebView2 host cannot be composited",
					openObject, openSlot, static_cast<std::uint32_t>(openHr),
					static_cast<std::uint32_t>(gameLuid.HighPart), gameLuid.LowPart,
					pending.adapterLuidHigh, pending.adapterLuidLow);
				ReleaseSharedRing();
				sharedOpenFailed = true;
				return false;
			}
			sharedGeneration = pending.generation;
			sharedSlotCount = pending.slotCount;
			for (std::uint32_t i = 0; i < sharedSlotCount; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				const D3D12_CPU_DESCRIPTOR_HANDLE handle{
					srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
					static_cast<SIZE_T>(1 + i) * srvStride
				};
				dev->CreateShaderResourceView(sharedSlots[i], &srv, handle);
			}
			REX::INFO("D3D12Compositor: shared ring adopted ({}x{}, {} slots, generation {})",
				pending.width, pending.height, sharedSlotCount, pending.generation);
			return true;
		}

		// Setup, on the Submit / tick thread.
		void EnsureSetup()
		{
			if (setupAttempted) {
				return;
			}
			setupAttempted = true;

			engine = LocateEngineD3D12();
			if (!engine) {
				REX::ERROR("D3D12Compositor: could not locate the engine device/queue; overlay disabled "
						   "(see reverse-engineering-notes.md §2)");
				return;
			}

			if (!CreateSharedObjects() || !InstallPresentHook()) {
				REX::ERROR("D3D12Compositor: setup failed; overlay disabled this session");
				return;
			}

			ffxFrameInterp = ResolveFfxFrameInterpRegion();
			if (ffxFrameInterp.lo != 0) {
				REX::INFO("D3D12Compositor: FSR3 frame-interpolation code bounded at [0x{:X}, 0x{:X}) — "
						  "presents issued from it will be recognized as Frame Generation pacing",
					ffxFrameInterp.lo, ffxFrameInterp.hi);
			} else {
				REX::WARN("D3D12Compositor: could not bound the exe's FSR3 frame-interpolation code from its "
						  "exports; FSR3 Frame Generation pacing will NOT be detected (DLSS-FG detection unaffected)");
			}

			g_overlay.store(this);
			g_seamDrawFn.store(&Impl::SeamDrawThunk, std::memory_order_release);
			setupOk = true;
			setupCompletedMs = ::GetTickCount64();
			REX::INFO("D3D12Compositor: seam overlay armed (Present slot-8 hook installed for discovery only)");
		}

		[[nodiscard]] bool CreateSharedObjects()
		{
			auto* dev = engine.device;

			if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)))) {
				REX::ERROR("D3D12Compositor: CreateFence failed");
				return false;
			}
			fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!fenceEvent) {
				REX::ERROR("D3D12Compositor: CreateEvent failed");
				return false;
			}

			// Root signature: 1 SRV table (t0) + 1 static linear-clamp sampler.
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &range;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			rsDesc.NumStaticSamplers = 1;
			rsDesc.pStaticSamplers = &sampler;
			rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ID3DBlob* rsBlob = nullptr;
			ID3DBlob* rsError = nullptr;
			if (FAILED(::D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError))) {
				REX::ERROR("D3D12Compositor: SerializeRootSignature failed: {}",
					rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "no message");
				SafeRelease(rsBlob);
				SafeRelease(rsError);
				return false;
			}
			const auto rsHr = dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
				__uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&rootSig));
			SafeRelease(rsBlob);
			SafeRelease(rsError);
			if (FAILED(rsHr)) {
				REX::ERROR("D3D12Compositor: CreateRootSignature failed");
				return false;
			}

			vsBlob = CompileShader(kVertexShader, "main", "vs_5_0");
			psBlob = CompileShader(kPixelShader, "main", "ps_5_0");
			if (!vsBlob || !psBlob) {
				return false;
			}

			// Descriptor heaps. SRV slot 0 is left unused (it held the retired
			// CPU-upload overlay texture); slots 1..kMaxSlots hold the
			// shared-ring textures.
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.NumDescriptors = 1 + SharedRingDesc::kMaxSlots;
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(dev->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&srvHeap)))) {
				REX::ERROR("D3D12Compositor: CreateDescriptorHeap(SRV) failed");
				return false;
			}
			srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			// Seam-draw RTV ring: typed views created per draw onto the
			// engine's (typeless, churning) UI buffers. Descriptors are copied
			// into the command list at OMSetRenderTargets time, so a small
			// ring outlives concurrent worker frames.
			{
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				heap.NumDescriptors = kSeamRtvSlots;
				if (FAILED(dev->CreateDescriptorHeap(&heap,
						__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&seamRtvHeap)))) {
					REX::ERROR("D3D12Compositor: seam RTV heap creation failed");
					return false;
				}
			}

			return true;
		}

		// Capture the swapchain Present vtable from a throwaway probe swapchain
		// (the "Kiero" technique): dxgi implements one swapchain class, so the
		// vtable is shared by every swapchain in the process and one slot-8 swap
		// sees the game's real presents too. No engine offsets — pure DXGI.
		//
		// The probe's shape is load-bearing for coexistence. Root-caused
		// 2026-07-19: BetterConsole inline-hooks CreateSwapChainForHwnd and
		// treats every call as a real game presenter. The old HWND probe on
		// engine.directQueue (a) evicted the game's entry from its queue-keyed
		// swapchain table, leaving a dangling probe pointer there → null-deref
		// CTD inside its Present hook one frame after F10, and (b) got its dummy
		// window re-subclassed, clobbering the single global that tool chains the
		// game window's WndProc through → DefWindowProcW severed the game's
		// input handling. Hence:
		//   * windowless probe: CreateSwapChainForComposition never enters
		//     CreateSwapChainForHwnd, so hooks there never fire and there is no
		//     window to re-subclass;
		//   * private throwaway DIRECT queue, never engine.directQueue, so tools
		//     that key state by command-queue pointer cannot mistake the probe
		//     for the game's presenter.
		// The dummy-window HWND probe survives only as a fallback (also on the
		// private queue) for systems where composition creation fails.
		[[nodiscard]] bool InstallPresentHook()
		{
			IDXGIFactory2* factory = nullptr;
			if (FAILED(::CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
				REX::ERROR("D3D12Compositor: CreateDXGIFactory2 failed");
				return false;
			}

			ID3D12CommandQueue* probeQueue = nullptr;
			{
				D3D12_COMMAND_QUEUE_DESC qDesc{};
				qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				if (FAILED(engine.device->CreateCommandQueue(&qDesc,
						__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&probeQueue)))) {
					REX::ERROR("D3D12Compositor: probe CreateCommandQueue failed");
					SafeRelease(factory);
					return false;
				}
			}

			DXGI_SWAP_CHAIN_DESC1 scDesc{};
			scDesc.Width = 1;
			scDesc.Height = 1;
			scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			scDesc.SampleDesc.Count = 1;
			scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			scDesc.BufferCount = 2;
			scDesc.Scaling = DXGI_SCALING_STRETCH;

			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = ::DefWindowProcW;
			wc.hInstance = ::GetModuleHandleW(nullptr);
			wc.lpszClassName = L"OSFUIOverlayDummyWnd";
			HWND dummyWnd = nullptr;
			bool dummyClassRegistered = false;

			IDXGISwapChain1* probeSwap = nullptr;
			{
				// Composition swapchains require flip model + a definite alpha mode.
				auto compDesc = scDesc;
				compDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
				compDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
				const auto hr = factory->CreateSwapChainForComposition(probeQueue, &compDesc, nullptr, &probeSwap);
				if (SUCCEEDED(hr) && probeSwap) {
					REX::DEBUG("D3D12Compositor: probe = windowless composition swapchain on a private queue");
				} else {
					probeSwap = nullptr;
					REX::WARN("D3D12Compositor: CreateSwapChainForComposition failed (hr=0x{:08X}) — falling back to "
							  "an HWND probe swapchain. Tools that hook CreateSwapChainForHwnd (e.g. BetterConsole) "
							  "may misinterpret the probe; include this line in coexistence reports.",
						static_cast<std::uint32_t>(hr));
				}
			}

			if (!probeSwap) {
				dummyClassRegistered = ::RegisterClassExW(&wc) != 0;
				dummyWnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
					0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
				if (dummyWnd) {
					auto hwndDesc = scDesc;
					hwndDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
					hwndDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
					const auto hr = factory->CreateSwapChainForHwnd(probeQueue, dummyWnd, &hwndDesc, nullptr, nullptr, &probeSwap);
					if (FAILED(hr) || !probeSwap) {
						probeSwap = nullptr;
						REX::ERROR("D3D12Compositor: probe CreateSwapChainForHwnd failed (hr=0x{:08X})", static_cast<std::uint32_t>(hr));
					}
				} else {
					REX::ERROR("D3D12Compositor: dummy window creation failed");
				}
			}

			bool ok = false;
			if (probeSwap) {
				auto** vtbl = *reinterpret_cast<void***>(probeSwap);
				auto& slot8 = vtbl[8];  // IDXGISwapChain::Present
				g_originalPresent.store(reinterpret_cast<PresentFn>(slot8));

				// Who owns Present before we hook it? Clean stack -> dxgi.dll;
				// ReShade/RTSS/Steam-overlay/frame-gen hooked first -> their
				// module. Either way we chain (calling whatever was there as
				// "original"), so hooking after others is the safe order.
				const auto owner = ModuleNameOfAddress(reinterpret_cast<const void*>(g_originalPresent.load()));
				if (owner.find("dxgi.dll") == std::string::npos && owner.find("<unknown") == std::string::npos) {
					REX::WARN("D3D12Compositor: Present slot 8 already points into '{}' — another overlay/hook tool "
							  "is ahead of us; chaining after it. If the overlay misbehaves, include this line in reports.",
						owner);
				} else {
					REX::DEBUG("D3D12Compositor: Present slot 8 owner before hook: '{}'", owner);
				}

				// The slot-owner check misses code-patching hooks (BetterConsole
				// et al.): the slot still reads as dxgi.dll while Present's first
				// instruction jumps into the tool. Follow the jmp chain and
				// report where it lands. Chaining still works — we call the
				// patched code as "original". Best-effort: such tools may install
				// their patch only later in the session.
				const auto* slotTarget = reinterpret_cast<const void*>(g_originalPresent.load());
				const auto* effective = FollowInlineJmps(slotTarget);
				if (effective != slotTarget) {
					REX::WARN("D3D12Compositor: Present's code is inline-patched — effective handler is in '{}'; "
							  "chaining after it. Include this line in coexistence reports.",
						ModuleNameOfAddress(effective));
				}

				DWORD oldProtect = 0;
				if (::VirtualProtect(&slot8, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
					slot8 = reinterpret_cast<void*>(&PresentThunk);
					::VirtualProtect(&slot8, sizeof(void*), oldProtect, &oldProtect);
					// Read back in case another tool raced the same slot between
					// our write and here.
					if (slot8 != reinterpret_cast<void*>(&PresentThunk)) {
						REX::WARN("D3D12Compositor: Present slot changed immediately after our write "
								  "(now in '{}') — another overlay re-hooked; relying on it chaining to us",
							ModuleNameOfAddress(slot8));
					}
					ok = true;
					REX::DEBUG("D3D12Compositor: hooked IDXGISwapChain::Present slot 8 (original 0x{:X})",
						reinterpret_cast<std::uintptr_t>(g_originalPresent.load()));
				} else {
					REX::ERROR("D3D12Compositor: VirtualProtect on the Present vtable slot failed");
				}
			}

			SafeRelease(probeSwap);
			SafeRelease(probeQueue);
			SafeRelease(factory);
			if (dummyWnd) {
				::DestroyWindow(dummyWnd);
			}
			if (dummyClassRegistered) {
				::UnregisterClassW(wc.lpszClassName, wc.hInstance);
			}
			return ok;
		}

		// Present, on whichever thread presents — the game's render thread, or
		// a frame-generation pacing thread when Streamline FG is active.
		static HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swap, UINT a_sync, UINT a_flags)
		{
			if (auto* self = static_cast<Impl*>(g_overlay.load())) {
				if (!(a_flags & DXGI_PRESENT_TEST) &&
					self->renderStatsEnabled.load(std::memory_order_relaxed)) {
					self->statsPresents.fetch_add(1, std::memory_order_relaxed);
				}
				// Stamp the watchdog before any gate: it answers "does the
				// present stream still reach our thunk at all", independent of
				// the single-flight gate.
				self->lastPresentMs.store(::GetTickCount64(), std::memory_order_relaxed);

				if (!self->presentBusy.exchange(true, std::memory_order_acquire)) {
					self->OnPresent(a_swap, a_flags, _ReturnAddress());
					self->presentBusy.store(false, std::memory_order_release);
				} else {
					self->skippedConcurrent.fetch_add(1, std::memory_order_relaxed);
					if (!self->concurrentWarned.exchange(true, std::memory_order_relaxed)) {
						REX::DEBUG("D3D12Compositor: overlapping Present calls from two threads — "
								   "a frame-generation pacing thread is likely active. Overlapped "
								   "presents skip discovery (counted, logged once); the seam draw "
								   "is unaffected.");
					}
				}
			}
			const auto original = g_originalPresent.load();
			return original ? original(a_swap, a_sync, a_flags) : S_OK;
		}

		// Plumbing only — the Present hook records nothing. All drawing happens
		// at the engine seam (RecordSeamOverlay). What still has to happen here
		// is what only the present stream can tell us:
		//   * the real output size, so the runtime can size the web view;
		//   * which swapchains a Frame Generation pacer drives, which decides
		//     the UI hand-off the seam is allowed to decorate; and
		//   * shared-ring adoption, on a thread guaranteed past device setup.
		void OnPresent(IDXGISwapChain* a_swap, UINT a_flags, const void* a_callerRet)
		{
			if (!setupOk || !a_swap || (a_flags & DXGI_PRESENT_TEST)) {
				return;
			}

			// Discover the real output even while hidden: the first submitted
			// manifest-sized frame installs this hook, but Runtime keeps it
			// invisible until the resize callback below has resized the web view.
			// AcquireTarget only reads GetDesc1 and holds no backbuffer ref.
			auto* target = AcquireTarget(a_swap);
			if (!target) {
				return;
			}

			UpdatePresentCaller(*target, a_callerRet);
			frameGenActiveSignal.store(AnyFrameGenActive(), std::memory_order_release);

			// Real output size -> runtime, so the view is aspect-correct. Fires
			// on first sight and on any change.
			if (onOutputResize && (target->width != notifiedOutputW || target->height != notifiedOutputH)) {
				notifiedOutputW = target->width;
				notifiedOutputH = target->height;
				onOutputResize(target->width, target->height);
				outputSizeKnown.store(true, std::memory_order_release);
			}

			// Adopt the ring as soon as the host has published a frame, so the
			// seam has SRVs the moment the overlay opens.
			bool ready = false;
			{
				std::scoped_lock lk(frameMutex);
				ready = sharedFrameReady;
			}
			if (ready) {
				(void)EnsureSharedRing();
			}
		}

		// Present-thread only (like all of targets[]). The 2s staleness window
		// releases the signal once the FG chain is torn down even if its target
		// entry has not been evicted yet.
		[[nodiscard]] bool AnyFrameGenActive() const
		{
			const auto nowMs = ::GetTickCount64();
			for (const auto& t : targets) {
				if (t.key != 0 && t.fgDriven && nowMs - t.lastSeenMs < 2000) {
					return true;
				}
			}
			return false;
		}

		// Re-classify a target when its Present call site changes (cheap pointer
		// compare per present; module resolution only on change). Rationale for
		// the two triggers is on Target::fgDriven.
		void UpdatePresentCaller(Target& a_t, const void* a_callerRet)
		{
			if (a_callerRet == a_t.callerRet) {
				return;
			}
			a_t.callerRet = a_callerRet;
			const auto owner = ModuleNameOfAddress(a_callerRet);
			const auto ret = reinterpret_cast<std::uintptr_t>(a_callerRet);
			const bool dlssFg = ModuleFileNameLower(owner).rfind("sl.dlss_g", 0) == 0;
			const bool fsrFg = ffxFrameInterp.lo != 0 && ret >= ffxFrameInterp.lo && ret < ffxFrameInterp.hi;
			const bool fg = dlssFg || fsrFg;
			if (fg && !a_t.fgDriven) {
				REX::INFO("D3D12Compositor: swapchain 0x{:X} is presented from {} (caller 0x{:X}) — Frame "
						  "Generation is pacing this swapchain. The overlay rides the engine's UI layer, so "
						  "it composites into real and generated frames alike; the seam now decorates only "
						  "the transparent UI hand-off consumed by frame interpolation.",
					a_t.key,
					dlssFg ? "'" + owner + "' (NVIDIA DLSS Frame Generation)"
					       : "the game's built-in FSR3 frame-interpolation code",
					ret);
			} else if (!fg && a_t.fgDriven) {
				REX::DEBUG("D3D12Compositor: swapchain 0x{:X} presents from '{}' again — Frame Generation "
						  "no longer pacing it",
					a_t.key, owner);
			} else if (!a_t.callerLogged) {
				// One coexistence line per swapchain: who normally presents it
				// (Starfield.exe directly, sl.interposer forwarding, or another
				// overlay's chained hook).
				REX::DEBUG("D3D12Compositor: swapchain 0x{:X} presents from '{}'", a_t.key, owner);
			}
			a_t.callerLogged = true;
			a_t.fgDriven = fg;
		}

		// Seam draw (UiPassSeam, render worker, inside the engine's UI-buffer
		// hand-off): record the overlay quad onto the ENGINE's list into the
		// engine's UI buffer — upstream of Frame Generation, so both real and
		// generated frames carry it. v1 transport: the produce fence is checked
		// on the CPU (we cannot wait on a queue we don't control) and the
		// consume fence is signaled from the CPU; ring depth covers the gap, a
		// lost race is a rare torn overlay frame, never a hazard.
		[[nodiscard]] bool RecordSeamOverlay(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			const auto statsStarted = std::chrono::steady_clock::now();
			if (!setupOk || !seamRtvHeap || !visible.load(std::memory_order_relaxed)) {
				return false;
			}
			// In the FG graph the RT->pixel-SRV candidate is the already-opaque
			// scene/composite image. Drawing there puts the overlay into the frame
			// interpolation input, then FFX composites the transparent UI layer on
			// top a second time. Opaque pixels hide that duplication; translucent
			// pixels alternate between one and two blends. The COPY_SOURCE target
			// is the actual transparent UI layer and later feeds both paths.
			if (a_fgTarget) {
				frameGenActiveSignal.store(true, std::memory_order_release);
			}
			const bool fgActive = frameGenActiveSignal.load(std::memory_order_acquire);
			if (fgActive && !a_fgTarget) {
				if (!seamFgLayerOnlyLogged.exchange(true, std::memory_order_relaxed)) {
					REX::DEBUG("D3D12Compositor: FG seam uses only the transparent COPY_SOURCE UI layer");
				}
				return false;
			}
			bool ready = false;
			std::uint32_t ringSlot = 0;
			std::uint64_t serial = 0;
			std::uint64_t sourceTimeMs = 0;
			{
				std::scoped_lock lk(frameMutex);
				ready = sharedFrameReady;
				ringSlot = gpuSlot;
				serial = gpuSerial;
				sourceTimeMs = gpuSourceTimeMs;
			}

			std::scoped_lock ring(ringMutex);
			if (!ready) {
				// Normally a brief startup transient: the overlay can be revealed
				// on the frame the host publishes its first shared slot.
				if (!noSharedFrameLogged) {
					noSharedFrameLogged = true;
					REX::DEBUG("D3D12Compositor: seam hand-off reached before the WebView2 host "
							   "published a shared-ring frame; nothing to draw yet");
				}
				return false;
			}
			// Promote the newest frame to "ready" once its produce fence has
			// completed; an incomplete newest frame falls back to the last
			// ready one (see seamReadySlot) instead of skipping the draw.
			// Under FG the preceding opaque target was deliberately skipped, so
			// the transparent target becomes the effective first draw.
			const bool effectiveRegionFirst = a_regionFirst || (fgActive && a_fgTarget);
			if (effectiveRegionFirst &&
				serial != 0 && ringSlot < sharedSlotCount && sharedSlots[ringSlot] &&
				(!sharedProduce || sharedProduce->GetCompletedValue() >= serial)) {
				seamReadySlot = ringSlot;
				seamReadySerial = serial;
			}
			if (seamReadySerial == 0 || seamReadySlot >= sharedSlotCount || !sharedSlots[seamReadySlot]) {
				return false;  // no fully-produced frame yet this ring generation
			}
			ringSlot = seamReadySlot;
			serial = seamReadySerial;

			auto* pso = EnsureSeamPipeline();
			if (!pso) {
				return false;
			}

			const auto desc = a_buffer->GetDesc();
			const auto rtvSlot = seamRtvNext.fetch_add(1, std::memory_order_relaxed) % kSeamRtvSlots;
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
				seamRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(rtvSlot) * rtvStride
			};
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // typed view on the typeless UI buffer
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			engine.device->CreateRenderTargetView(a_buffer, &rtvDesc, rtv);

			ID3D12DescriptorHeap* heaps[]{ srvHeap };
			a_list->SetDescriptorHeaps(1, heaps);
			a_list->SetGraphicsRootSignature(rootSig);
			const D3D12_GPU_DESCRIPTOR_HANDLE srv{
				srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
				static_cast<UINT64>(1 + ringSlot) * srvStride
			};
			a_list->SetGraphicsRootDescriptorTable(0, srv);

			const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
			const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
			a_list->RSSetViewports(1, &vp);
			a_list->RSSetScissorRects(1, &scissor);
			a_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			a_list->SetPipelineState(pso);
			a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_list->DrawInstanced(3, 1, 0, 0);

			if (sharedConsume && serial > seamLastConsumeSignaled) {
				// CPU-side pacing: lets the host reuse older slots. The GPU may
				// not have sampled yet, but ring depth (4 slots) covers it.
				sharedConsume->Signal(serial);
				seamLastConsumeSignaled = serial;
			}

			const auto drawIndex = seamDraws.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto fgIndex = a_fgTarget
				? seamDrawsFgTarget.fetch_add(1, std::memory_order_relaxed) + 1
				: 0;
			CountRenderStatsDraw(serial, sourceTimeMs, statsStarted);
			// One-time log per target kind: if the FG line never appears, its
			// hand-off is not being matched.
			if (drawIndex == 1 || fgIndex == 1) {
				REX::INFO("D3D12Compositor: FIRST SEAM OVERLAY DRAW [{}] (ring slot {} serial {} -> {}x{} UI buffer 0x{:X})",
					a_fgTarget ? "premultiplied / FG UI input" : "premultiplied / composite input",
					ringSlot, serial, static_cast<std::uint64_t>(desc.Width), desc.Height,
					reinterpret_cast<std::uintptr_t>(a_buffer));
			}
			return true;
		}

		static bool SeamDrawThunk(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			auto* self = static_cast<Impl*>(g_overlay.load(std::memory_order_acquire));
			return self && self->RecordSeamOverlay(a_list, a_buffer, a_fgTarget, a_regionFirst);
		}

		// Get-or-create the one seam PSO. Called under ringMutex, so the cache
		// needs no separate synchronization. `seamPsoFailed` stops a failed
		// creation from retrying (and spamming the log) on every hand-off.
		[[nodiscard]] ID3D12PipelineState* EnsureSeamPipeline()
		{
			if (seamPso || seamPsoFailed) {
				return seamPso;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = rootSig;
			desc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
			desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;

			// Premultiplied-alpha "over" blend for BGRA browser frames, matching
			// the engine's own UI composition (docs/seam-draw-design.md).
			auto& rt = desc.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			if (FAILED(engine.device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&seamPso)))) {
				REX::ERROR("D3D12Compositor: seam CreateGraphicsPipelineState failed");
				seamPso = nullptr;
				seamPsoFailed = true;
				return nullptr;
			}
			REX::INFO("D3D12Compositor: seam pipeline ready (premultiplied over, R8G8B8A8_UNORM)");
			return seamPso;
		}

		// Find or (re)build the discovery entry for this swapchain pointer. The
		// returned target's `swap` is a borrowed pointer valid only for the
		// remainder of this present (see the Target comment): the QI ref is
		// dropped immediately so this table never extends a swapchain's
		// lifetime past the game's own release.
		[[nodiscard]] Target* AcquireTarget(IDXGISwapChain* a_swap)
		{
			IDXGISwapChain3* swap3 = nullptr;
			if (FAILED(a_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap3))) || !swap3) {
				return nullptr;
			}
			swap3->Release();  // borrow: the in-progress Present call keeps it alive

			const auto key = reinterpret_cast<std::uintptr_t>(a_swap);
			const auto nowMs = ::GetTickCount64();
			Target* freeSlot = nullptr;
			Target* stalest = nullptr;
			for (auto& t : targets) {
				if (t.key == key) {
					t.swap = swap3;
					t.lastSeenMs = nowMs;
					return RefreshTarget(t) ? &t : nullptr;
				}
				if (t.key == 0 && !freeSlot) {
					freeSlot = &t;
				}
				if (t.key != 0 && (!stalest || t.lastSeenMs < stalest->lastSeenMs)) {
					stalest = &t;
				}
			}
			if (!freeSlot) {
				// Evict an entry that stopped presenting (its swapchain was
				// almost certainly destroyed in a recreation) rather than
				// ignoring the new one forever.
				if (stalest && nowMs - stalest->lastSeenMs > 2000) {
					REX::INFO("D3D12Compositor: swapchain 0x{:X} silent for {} ms; evicting for new swapchain 0x{:X}",
						stalest->key, nowMs - stalest->lastSeenMs, key);
					*stalest = Target{};  // no COM refs held (swap is borrowed)
					freeSlot = stalest;
				} else {
					if (!targetsFullLogged) {
						targetsFullLogged = true;
						REX::WARN("D3D12Compositor: more than {} swapchains presenting concurrently; ignoring extras", kMaxSwapchains);
					}
					return nullptr;
				}
			}

			freeSlot->swap = swap3;
			freeSlot->key = key;
			freeSlot->lastSeenMs = nowMs;
			return RefreshTarget(*freeSlot) ? freeSlot : nullptr;
		}

		// Refresh the cached swapchain dimensions. GetDesc1 only, holds no
		// backbuffer refs, so it cannot block the game's ResizeBuffers. The
		// backbuffer format is deliberately not inspected: the seam renders into
		// the engine's UI buffer, so an HDR or _SRGB backbuffer — which the
		// retired present-time draw had to refuse — is no longer our concern.
		[[nodiscard]] bool RefreshTarget(Target& a_t)
		{
			DXGI_SWAP_CHAIN_DESC1 desc{};
			if (FAILED(a_t.swap->GetDesc1(&desc))) {
				return false;
			}
			const bool changed = !a_t.seen || a_t.width != desc.Width || a_t.height != desc.Height;
			a_t.seen = true;
			a_t.width = desc.Width;
			a_t.height = desc.Height;
			if (changed) {
				REX::DEBUG("D3D12Compositor: swapchain 0x{:X} is {}x{}", a_t.key, a_t.width, a_t.height);
			}
			return true;
		}

		// Tick thread. The game is ticking (we're being called), so it is
		// presenting — if no present has reached our thunk for a while, another
		// tool re-hooked Present over us without chaining. Seam draws already
		// running survive that, but ring adoption, output sizing and FG
		// classification all stop, so an overlay that has not opened yet never
		// will. Warn once; log recovery if presents return.
		void CheckPresentLiveness()
		{
			if (!setupOk) {
				return;
			}
			constexpr std::uint64_t kStallMs = 10'000;
			const auto now = ::GetTickCount64();
			const auto last = (std::max)(lastPresentMs.load(std::memory_order_relaxed), setupCompletedMs);
			if (now - last <= kStallMs) {
				if (bypassWarned) {
					bypassWarned = false;
					REX::INFO("D3D12Compositor: presents are reaching the hook again");
				}
				return;
			}
			if (!bypassWarned) {
				bypassWarned = true;
				REX::WARN("D3D12Compositor: no present has reached our hook for >{}s while the game is ticking — "
						  "another overlay (ReShade/RTSS/Steam overlay/frame-gen tool) appears to have re-hooked "
						  "IDXGISwapChain::Present without chaining. Drawing happens at the engine seam and is "
						  "not itself hooked, but the shared frame ring is adopted here — so if the overlay has "
						  "not appeared yet, it will not until this is restored. Try changing the load/injection "
						  "order of overlay tools, and include this line plus your overlay stack in reports.",
					kStallMs / 1000);
			}
		}

		// Submit: just remember which ring slot/serial the seam should draw.
		// No pixel copy — the seam samples the shared texture directly.
		void CacheSharedFrame(const FrameBufferView& a_frame)
		{
			if (a_frame.frameIndex == lastSubmittedIndex) {
				return;
			}
			lastSubmittedIndex = a_frame.frameIndex;
			std::scoped_lock lk(frameMutex);
			sharedFrameReady = true;
			gpuSlot = static_cast<std::uint32_t>(a_frame.sharedSlot);
			gpuSerial = a_frame.frameIndex;
			gpuSourceTimeMs = a_frame.sourceTimeMs;
			if (renderStatsEnabled.load(std::memory_order_relaxed)) {
				statsSubmits.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// The seam samples the host's shared texture on the engine's own command
		// list; there is no path that uploads CPU pixels any more. The shipping
		// renderer (WebView2 host) always publishes shared-slot frames, so this
		// only fires for a renderer that has no GPU transport.
		void WarnCpuFrameUnsupported()
		{
			if (cpuFrameWarned) {
				return;
			}
			cpuFrameWarned = true;
			REX::WARN("D3D12Compositor: the renderer submitted a CPU-staged frame, which the seam "
					  "cannot draw — this compositor requires the shared-texture GPU transport");
		}
	};

	D3D12Compositor::D3D12Compositor() = default;
	D3D12Compositor::~D3D12Compositor() = default;

	bool D3D12Compositor::Initialize()
	{
		_impl = std::make_unique<Impl>();
		REX::INFO("D3D12Compositor: initialized (seam overlay; engine device/queue + Present discovery hook "
				  "are set up on the first submitted frame)");
		return true;
	}

	void D3D12Compositor::Shutdown()
	{
		if (_impl) {
			REX::INFO("D3D12Compositor: shutdown after {} seam overlay draw(s)",
				_impl->seamDraws.load(std::memory_order_relaxed));
			_impl.reset();
		}
	}

	void D3D12Compositor::Submit(const FrameBufferView& a_frame)
	{
		if (!_impl) {
			return;
		}
		if (a_frame.sharedSlot >= 0) {
			_impl->CacheSharedFrame(a_frame);
		} else {
			_impl->WarnCpuFrameUnsupported();
		}
		_impl->EnsureSetup();
		_impl->CheckPresentLiveness();
	}

	void D3D12Compositor::SetSeamDrawMode(const bool a_enabled)
	{
		if (_impl) {
			_impl->seamMode.store(a_enabled, std::memory_order_relaxed);
		}
	}

	void D3D12Compositor::SetRenderStatsEnabled(const bool a_enabled)
	{
		if (_impl) {
			_impl->SetRenderStatsEnabled(a_enabled);
		}
	}

	CompositorStats D3D12Compositor::GetRenderStats() const
	{
		return _impl ? _impl->GetRenderStats() : CompositorStats{};
	}

	bool RecordSeamOverlayDraw(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
		const bool a_fgTarget, const bool a_regionFirst)
	{
		const auto fn = g_seamDrawFn.load(std::memory_order_acquire);
		return fn && a_list && a_buffer && fn(a_list, a_buffer, a_fgTarget, a_regionFirst);
	}

	void D3D12Compositor::SetSharedRing(const SharedRingDesc& a_desc)
	{
		if (_impl) {
			_impl->SetSharedRing(a_desc);
		} else {
			// Not initialized: still own the handles — close them.
			SharedRingDesc desc = a_desc;
			Impl::CloseRingHandles(desc);
		}
	}

	void D3D12Compositor::SetVisible(bool a_visible)
	{
		if (_impl) {
			_impl->visible.store(a_visible, std::memory_order_relaxed);
		}
	}

	void D3D12Compositor::SetOutputResizeCallback(OutputResizeCallback a_callback)
	{
		if (_impl) {
			_impl->onOutputResize = std::move(a_callback);
		}
	}

	bool D3D12Compositor::IsOutputSizeKnown() const
	{
		return _impl && _impl->outputSizeKnown.load(std::memory_order_acquire);
	}
}
