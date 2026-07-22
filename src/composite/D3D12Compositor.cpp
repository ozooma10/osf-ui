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
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace OSFUI
{
	namespace
	{
		// Ring depth must cover every present in flight before the oldest slot's
		// GPU work completes. The game drives more than one swapchain through
		// this single ring (two seen in-game), so each swapchain only gets
		// kCmdSlots/N frames of depth — keep this above (max swapchains) x
		// (game's frame-queue depth) or slots are still busy when we cycle back.
		constexpr std::uint32_t kCmdSlots = 6;          // command allocator/list ring depth
		constexpr std::uint32_t kMaxSwapchains = 4;     // distinct swapchains we'll draw on
		constexpr std::uint32_t kMaxBackBuffers = 4;    // per swapchain
		constexpr std::uint32_t kMaxPsoFormats = 4;     // distinct backbuffer formats we'll build PSOs for
		constexpr std::uint32_t kRowPitchAlignment = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
		constexpr DWORD         kSlotWaitTimeoutMs = 100;  // cap the wait for a busy ring slot

		template <class T>
		void SafeRelease(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		[[nodiscard]] constexpr std::uint32_t AlignUp(const std::uint32_t a_value, const std::uint32_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		[[nodiscard]] DXGI_FORMAT ToDxgiFormat(const PixelFormat a_format)
		{
			return a_format == PixelFormat::kBGRA8 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		// Formats the overlay renders correctly into: 8-bit UNORM, sRGB-encoded
		// SDR. Anything else is refused per swapchain with a warning instead of
		// drawing wrong — HDR10 (R10G10B10A2 + PQ) and scRGB (FP16) need a
		// different encode in the pixel shader, and an _SRGB view would
		// double-encode our already-sRGB pixels. Full HDR: docs/ROADMAP.md (P1).
		[[nodiscard]] constexpr bool IsSupportedRtFormat(const DXGI_FORMAT a_format)
		{
			return a_format == DXGI_FORMAT_R8G8B8A8_UNORM || a_format == DXGI_FORMAT_B8G8R8A8_UNORM;
		}

		[[nodiscard]] const char* FormatName(const DXGI_FORMAT a_format)
		{
			switch (a_format) {
				case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
				case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
				case DXGI_FORMAT_R10G10B10A2_UNORM:   return "R10G10B10A2_UNORM (10-bit / HDR10)";
				case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT (scRGB HDR)";
				default:                              return "unrecognized";
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
		// {0,0} (detection off, drawing proceeds as before) if a future patch
		// stops exporting these.
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

		// Is the output this swapchain sits on in HDR (advanced color) mode?
		// DXGI has no getter for a swapchain's own color space, so the output's
		// state is the closest signal available.
		[[nodiscard]] const char* OutputColorModeInfo(IDXGISwapChain3* a_swap)
		{
			IDXGIOutput* output = nullptr;
			if (FAILED(a_swap->GetContainingOutput(&output)) || !output) {
				return "output color mode unknown";
			}
			IDXGIOutput6* output6 = nullptr;
			const auto hr = output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6));
			output->Release();
			if (FAILED(hr) || !output6) {
				return "output color mode unknown (pre-DXGI-1.6)";
			}
			DXGI_OUTPUT_DESC1 desc{};
			const bool ok = SUCCEEDED(output6->GetDesc1(&desc));
			output6->Release();
			if (!ok) {
				return "output color mode unknown";
			}
			return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
				? "output IS in HDR mode"
				: "output is in SDR mode";
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
		bool          devMode{ false };
		std::atomic_bool visible{ false };

		// Output-size signal -> runtime (resize the view to match the screen).
		OutputResizeCallback onOutputResize;
		std::uint32_t        notifiedOutputW{ 0 };
		std::uint32_t        notifiedOutputH{ 0 };
		std::atomic_bool     outputSizeKnown{ false };

		bool setupAttempted{ false };
		bool setupOk{ false };

		// CPU frame staging (Submit thread <-> present thread).
		std::mutex                frameMutex;
		std::vector<std::uint8_t> cpuPixels;  // tightly packed w*4
		std::uint32_t             frameWidth{ 0 };
		std::uint32_t             frameHeight{ 0 };
		DXGI_FORMAT               frameFormat{ DXGI_FORMAT_UNKNOWN };
		bool                      frameDirty{ false };
		// Region of cpuPixels changed since the last consumed upload; presents
		// can lag submits, so CacheFrame unions rects until RecordAndExecute
		// drains them (guarded by frameMutex like frameDirty).
		DirtyRect                 dirtyRegion{};
		std::uint64_t             lastSubmittedIndex{ 0 };

		// GPU shared-ring transport (out-of-process WebView2 host). SetSharedRing
		// (game thread) parks the announced ring here; the present thread adopts
		// it lazily (EnsureSharedRing) because opening the handles needs the
		// located engine device. The compositor owns the handles from
		// SetSharedRing on.
		// Seam-draw mode (config uiPassDraw): UiPassSeam records the overlay
		// quad into the engine's UI buffer via RecordSeamOverlay below; the
		// present hook stays as plumbing (discovery, resize notify, ring
		// adoption) and never draws. ringMutex guards the opened ring
		// (sharedSlots/fences/SRVs) between the present thread (adoption) and
		// the seam's render workers (sampling).
		std::atomic<bool> seamMode{ false };
		std::mutex        ringMutex;
		static constexpr std::uint32_t kSeamRtvSlots = 8;
		ID3D12DescriptorHeap*      seamRtvHeap{ nullptr };  // typed RTVs onto the engine's (typeless) UI buffers
		std::atomic<std::uint32_t> seamRtvNext{ 0 };
		std::uint64_t seamLastConsumeSignaled{ 0 };  // ringMutex
		std::uint64_t seamDraws{ 0 };                // ringMutex
		bool          seamGpuWarned{ false };        // ringMutex
		// Newest slot whose produce fence is CPU-verified complete. The seam
		// cannot queue-wait on the host's fence (not our queue), and skipping
		// incomplete frames flickers under rapid production (mouse-move
		// repaints, 2026-07-21) — so an incomplete newest frame falls back to
		// this one instead: one frame stale, never absent.
		std::uint32_t seamReadySlot{ 0 };    // ringMutex
		std::uint64_t seamReadySerial{ 0 };  // ringMutex
		std::uint64_t seamDrawsFgTarget{ 0 };  // ringMutex (diagnostics)
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
		// pending GPU frame (guarded by frameMutex, like the CPU staging):
		bool          gpuMode{ false };
		std::uint32_t gpuSlot{ 0 };
		std::uint64_t gpuSerial{ 0 };
		std::uint32_t gpuWidth{ 0 }, gpuHeight{ 0 };

		// Shared GPU objects, created once.
		ID3D12Fence*          fence{ nullptr };
		HANDLE                fenceEvent{ nullptr };
		std::uint64_t         nextFenceValue{ 1 };
		ID3D12RootSignature*  rootSig{ nullptr };
		// One PSO per supported backbuffer format seen — the game and an injected
		// frame-gen/overlay swapchain can differ, so first-seen-wins would starve
		// one of them. `failed` marks a format whose PSO creation failed, so we
		// don't retry (and spam the log) every present.
		struct PsoEntry
		{
			DXGI_FORMAT          format{ DXGI_FORMAT_UNKNOWN };
			ID3D12PipelineState* pso{ nullptr };
			bool                 failed{ false };
		};
		PsoEntry              psoCache[kMaxPsoFormats]{};
		// shader-visible: slot 0 = CPU overlay texture, 1..kMaxSlots = shared ring
		ID3D12DescriptorHeap* srvHeap{ nullptr };
		ID3D12DescriptorHeap* rtvHeap{ nullptr };  // backbuffer RTVs
		std::uint32_t         rtvStride{ 0 };
		std::uint32_t         srvStride{ 0 };
		ID3DBlob*             vsBlob{ nullptr };
		ID3DBlob*             psBlob{ nullptr };

		struct CmdSlot
		{
			ID3D12CommandAllocator*    allocator{ nullptr };
			ID3D12GraphicsCommandList* list{ nullptr };
			ID3D12Resource*            upload{ nullptr };
			std::uint8_t*              mapped{ nullptr };
			std::uint64_t              fenceValue{ 0 };
		};
		CmdSlot       cmdSlots[kCmdSlots]{};
		std::uint64_t cmdIndex{ 0 };

		// Overlay texture, sized to the frame.
		ID3D12Resource* texture{ nullptr };
		std::uint32_t   texWidth{ 0 };
		std::uint32_t   texHeight{ 0 };
		DXGI_FORMAT     texFormat{ DXGI_FORMAT_UNKNOWN };
		std::uint32_t   uploadRowPitch{ 0 };
		// A freshly (re)created texture has undefined contents, so the next
		// upload must cover the whole frame regardless of the dirty region.
		// Present thread only (set in EnsureTextureForFrame, consumed in
		// RecordAndExecute).
		bool            needsFullUpload{ true };

		// Per-swapchain backbuffer targets. Backbuffer resources are not cached:
		// holding a backbuffer reference blocks the game's ResizeBuffers
		// (resolution change / alt-tab / fullscreen transition), which crashed
		// the game (2026-06-12). Each present does GetBuffer + CreateRTV fresh
		// and releases the buffer. The swapchain itself is not ref'd either:
		// a lasting ref keeps a game-released swapchain alive, its HWND stays
		// associated, and the game's next CreateSwapChainForHwnd on that window
		// fails — Starfield's built-in FSR3 frame-interpolation swapchain
		// creation null-derefs on exactly that failure (external CTD,
		// 2026-07-21, crash inside ffxCreateFrameinterpolationSwapchainForHwndDX12).
		// `swap` is borrowed: refreshed by AcquireTarget on every present and
		// only dereferenced below that call in the same present, while the
		// presenting swapchain is guaranteed alive by its own Present call.
		struct Target
		{
			IDXGISwapChain3* swap{ nullptr };  // borrowed (NOT ref'd), key is the raw self ptr
			std::uintptr_t   key{ 0 };
			std::uint32_t    width{ 0 };
			std::uint32_t    height{ 0 };
			DXGI_FORMAT      format{ DXGI_FORMAT_UNKNOWN };
			std::uint32_t    bufferCount{ 0 };
			std::uint32_t    rtvBase{ 0 };  // base index into rtvHeap
			bool             seen{ false };  // dims known yet?
			bool             logged{ false };
			// IsSupportedRtFormat for this swapchain's format. Re-evaluated on
			// every format change (e.g. HDR toggled mid-session), warned once
			// per change.
			bool             supported{ false };
			// Who calls Present on this swapchain (immediate return address of
			// our thunk, re-resolved to a module when the call site changes).
			// Two Frame Generation pacers are recognized, and fgDriven targets
			// are never drawn on because drawing races the pacer's own queue
			// work from its pacing thread:
			//  - sl.dlss_g.dll (NVIDIA DLSS-FG via Streamline; external CTD:
			//    null-deref inside sl.dlss_g during swapchain recreation
			//    seconds after our first draw). sl.interposer alone is NOT a
			//    trigger: every vanilla install ships it, and passive
			//    game-thread forwarding through it draws fine.
			//  - the exe's own statically-linked FSR3 frame-interpolation
			//    swapchain (external CTD 2026-07-21), detected by return
			//    address inside the export-bounded ffx region — its module is
			//    Starfield.exe, so a module-name check can't see it.
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
		// (resolved once in EnsureSetup; {0,0} = undetectable, treat no caller
		// as FSR-FG). See ResolveFfxFrameInterpRegion.
		FfxFrameInterpRegion ffxFrameInterp{};

		std::uint64_t drawnFrames{ 0 };
		std::uint64_t waitedBusy{ 0 };   // presents that had to wait for a busy slot
		std::uint64_t skippedBusy{ 0 };  // presents dropped because the wait timed out (GPU hung)
		bool          fgSuspendLogged{ false };  // FrameGenActive transition logging (present thread only)
		// Present threads publish FG liveness for seam render workers. With FG
		// active, the first RT->pixel-SRV match is an opaque scene buffer, not
		// the transparent Scaleform UI layer; only the RT->COPY_SOURCE hand-off
		// is safe to decorate (it later feeds both the real composite and FFX).
		std::atomic_bool frameGenActiveSignal{ false };
		std::atomic_bool seamFgLayerOnlyLogged{ false };

		// Single-flight gate for OnPresent. With Frame Generation active the
		// real swapchain presents from FG's pacing thread concurrently with the
		// game thread's presents, and everything OnPresent touches (targets,
		// cmdSlots, cmdIndex, PSO cache) is single-thread state. An overlapped
		// present skips the overlay draw instead of corrupting that state.
		std::atomic_bool           presentBusy{ false };
		std::atomic<std::uint64_t> skippedConcurrent{ 0 };
		std::atomic_bool           concurrentWarned{ false };
		bool          firstDrawLogged{ false };
		bool          targetsFullLogged{ false };

		// Hook-liveness watchdog. Another overlay that re-hooks Present after us
		// without chaining silently stops our thunk from running — the overlay
		// just vanishes. PresentThunk stamps this every call (present thread);
		// CheckPresentLiveness (tick thread) warns when the game keeps ticking
		// but no present has reached us. No false positive on focus loss: the
		// game pauses the tick loop too, so the watchdog isn't polled then.
		std::atomic<std::uint64_t> lastPresentMs{ 0 };
		std::uint64_t              setupCompletedMs{ 0 };  // tick thread only
		bool                       bypassWarned{ false };  // tick thread only

		~Impl()
		{
			g_overlay.store(nullptr);
			WaitForGpuIdle();
			ReleaseSharedRing();
			{
				std::scoped_lock lk(sharedMutex);
				if (sharedDirty) {
					CloseRingHandles(sharedPending);
					sharedDirty = false;
				}
			}
			for (auto& t : targets) {
				ReleaseTarget(t);
			}
			for (auto& s : cmdSlots) {
				if (s.upload && s.mapped) {
					s.upload->Unmap(0, nullptr);
				}
				SafeRelease(s.upload);
				SafeRelease(s.list);
				SafeRelease(s.allocator);
			}
			SafeRelease(texture);
			for (auto& e : psoCache) {
				SafeRelease(e.pso);
			}
			SafeRelease(rootSig);
			SafeRelease(srvHeap);
			SafeRelease(rtvHeap);
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

		void ReleaseTarget(Target& a_t)
		{
			a_t = Target{};  // no COM refs held (swap is borrowed, backbuffers uncached)
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
			// Old slots may still be referenced by in-flight draws (our own are
			// idle-fenced; ENGINE lists carrying seam draws are covered by ring
			// retirement). ringMutex: the seam worker must not sample mid-swap.
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
			REX::INFO("D3D12Compositor: present-time overlay armed (Present slot-8 hook installed)");
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

			// Descriptor heaps. SRV slot 0 is the CPU-upload overlay texture;
			// slots 1..kMaxSlots hold the shared-ring textures (GPU transport).
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.NumDescriptors = 1 + SharedRingDesc::kMaxSlots;
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(dev->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&srvHeap)))) {
				REX::ERROR("D3D12Compositor: CreateDescriptorHeap(SRV) failed");
				return false;
			}
			srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
			rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvDesc.NumDescriptors = kMaxSwapchains * kMaxBackBuffers;
			rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (FAILED(dev->CreateDescriptorHeap(&rtvDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtvHeap)))) {
				REX::ERROR("D3D12Compositor: CreateDescriptorHeap(RTV) failed");
				return false;
			}
			rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			// Command allocator/list ring (lists kept closed when idle).
			for (auto& slot : cmdSlots) {
				if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
						__uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&slot.allocator)))) {
					REX::ERROR("D3D12Compositor: CreateCommandAllocator failed");
					return false;
				}
				if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
						__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&slot.list)))) {
					REX::ERROR("D3D12Compositor: CreateCommandList failed");
					return false;
				}
				slot.list->Close();
			}

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
		// hooks the game's real presents too. No engine offsets — pure DXGI.
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
					REX::INFO("D3D12Compositor: probe = windowless composition swapchain on a private queue");
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
					REX::INFO("D3D12Compositor: Present slot 8 owner before hook: '{}'", owner);
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
					REX::INFO("D3D12Compositor: hooked IDXGISwapChain::Present slot 8 (original 0x{:X})",
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
				// Stamp the watchdog before any gate: it answers "does the
				// present stream still reach our thunk at all", independent of
				// visibility, frame availability, or the single-flight gate.
				self->lastPresentMs.store(::GetTickCount64(), std::memory_order_relaxed);

				if (!self->presentBusy.exchange(true, std::memory_order_acquire)) {
					self->OnPresent(a_swap, a_flags, _ReturnAddress());
					self->presentBusy.store(false, std::memory_order_release);
				} else {
					self->skippedConcurrent.fetch_add(1, std::memory_order_relaxed);
					if (!self->concurrentWarned.exchange(true, std::memory_order_relaxed)) {
						REX::WARN("D3D12Compositor: overlapping Present calls from two threads — "
								  "a frame-generation pacing thread is likely active. Overlapped "
								  "presents skip the overlay draw (counted, warned once).");
					}
				}
			}
			const auto original = g_originalPresent.load();
			return original ? original(a_swap, a_sync, a_flags) : S_OK;
		}

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

			if (seamMode.load(std::memory_order_relaxed)) {
				// Seam mode records into the engine's transparent UI layer.
				// Present discovery above also publishes FG liveness so the seam
				// ignores the opaque scene hand-off that only exists in the FG
				// graph. Here we keep the remaining plumbing alive: resize
				// notification and ring adoption so the seam has SRVs the moment
				// the overlay opens. Nothing draws from the present hook in this
				// mode.
				bool gpu = false;
				{
					std::scoped_lock lk(frameMutex);
					gpu = gpuMode;
				}
				if (gpu) {
					(void)EnsureSharedRing();
				}
				return;
			}

			if (!visible.load(std::memory_order_relaxed)) {
				return;
			}

			if (target->fgDriven) {
				return;  // FG-paced swapchain: never draw (see Target::fgDriven)
			}

			// Frame Generation suspends the overlay on EVERY swapchain, not
			// just the FG-paced one. With FSR3-FG active the game presents two
			// real swapchains (the FI-paced one plus a game-thread sibling
			// forwarded through sl.interposer), and one draw into the sibling
			// killed the game instantly (2026-07-21 local repro: log ends on
			// the same millisecond as FIRST OVERLAY DRAW; the earlier
			// sl.dlss_g+0x23EE0 crashes — a null-context dispatch stub — were
			// the downstream symptom during the resulting recreation). The FG
			// pipeline owns backbuffer state across the whole chain, so any
			// PRESENT->RT->PRESENT round trip of ours races it.
			if (FrameGenActive()) {
				return;
			}

			bool gpu = false;
			{
				std::scoped_lock lk(frameMutex);
				gpu = gpuMode;
			}
			if (gpu) {
				// GPU transport: sample the host's shared slot directly — no
				// CPU staging, no upload.
				if (!EnsureSharedRing()) {
					return;
				}
			} else if (!EnsureTextureForFrame()) {
				return;  // no frame yet, or texture creation failed
			}
			if (!target->supported) {
				return;  // HDR/unknown backbuffer format; warned in RefreshTarget
			}
			auto* pipeline = EnsurePipeline(target->format);
			if (!pipeline) {
				return;
			}

			auto& slot = cmdSlots[cmdIndex % kCmdSlots];
			if (slot.fenceValue != 0 && fence->GetCompletedValue() < slot.fenceValue) {
				// The slot's prior GPU work must finish before we reuse its
				// allocator/upload buffer. Skipping the draw instead left this
				// present without the overlay -> visible flicker (~7% of
				// presents, since multiple swapchains share this ring). Waiting
				// is cheap: the overlay is one triangle, so the fence is almost
				// always already signaled. Only a stuck GPU hits the timeout and
				// falls through to a skip — reusing in-flight resources would
				// corrupt.
				++waitedBusy;
				fence->SetEventOnCompletion(slot.fenceValue, fenceEvent);
				::WaitForSingleObject(fenceEvent, kSlotWaitTimeoutMs);
				if (fence->GetCompletedValue() < slot.fenceValue) {
					++skippedBusy;  // GPU still not done after the timeout; drop (rare)
					return;
				}
			}

			const auto backIndex = target->swap->GetCurrentBackBufferIndex();
			if (backIndex >= target->bufferCount) {
				return;
			}

			if (gpu) {
				RecordAndExecuteShared(slot, *target, backIndex, pipeline);
			} else {
				RecordAndExecute(slot, *target, backIndex, pipeline);
			}
			++cmdIndex;
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

		[[nodiscard]] bool FrameGenActive()
		{
			const bool active = AnyFrameGenActive();
			frameGenActiveSignal.store(active, std::memory_order_release);
			if (active && !fgSuspendLogged) {
				fgSuspendLogged = true;
				REX::WARN("D3D12Compositor: Frame Generation is active — overlay drawing suspended on ALL "
						  "swapchains (drawing anywhere in an FG present chain can crash the game). "
						  "Disable Frame Generation in Starfield's display settings to see the overlay.");
			} else if (!active && fgSuspendLogged) {
				fgSuspendLogged = false;
				REX::INFO("D3D12Compositor: Frame Generation no longer pacing any swapchain — overlay drawing resumed");
			}
			return active;
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
				REX::WARN("D3D12Compositor: swapchain 0x{:X} is presented from {} (caller 0x{:X}) — Frame "
						  "Generation is pacing this swapchain, and drawing into it races the frame-gen "
						  "queue work (can crash the game). The overlay will NOT draw on this swapchain. "
						  "If the overlay is invisible, disable Frame Generation in Starfield's display "
						  "settings. Frame-gen compatibility is tracked in the project roadmap.",
					a_t.key,
					dlssFg ? "'" + owner + "' (NVIDIA DLSS Frame Generation)"
					       : "the game's built-in FSR3 frame-interpolation code",
					ret);
			} else if (!fg && a_t.fgDriven) {
				REX::INFO("D3D12Compositor: swapchain 0x{:X} presents from '{}' again — Frame Generation "
						  "no longer pacing it; resuming overlay draws",
					a_t.key, owner);
			} else if (!a_t.callerLogged) {
				// One coexistence line per swapchain: who normally presents it
				// (Starfield.exe directly, sl.interposer forwarding, or another
				// overlay's chained hook).
				REX::INFO("D3D12Compositor: swapchain 0x{:X} presents from '{}'", a_t.key, owner);
			}
			a_t.callerLogged = true;
			a_t.fgDriven = fg;
		}

		// GPU-transport draw: fullscreen quad sampling the shared ring slot. The
		// queue waits on the host's produce fence before the draw and signals the
		// consume fence after it, so the slot is neither read too early nor
		// rewritten too soon. The shared textures are cross-API resources
		// (simultaneous-access); no barriers on them.
		void RecordAndExecuteShared(CmdSlot& a_slot, Target& a_target,
			const std::uint32_t a_backIndex, ID3D12PipelineState* a_pso)
		{
			std::uint32_t ringSlot = 0;
			std::uint64_t serial = 0;
			{
				std::scoped_lock lk(frameMutex);
				ringSlot = gpuSlot;
				serial = gpuSerial;
			}
			if (ringSlot >= sharedSlotCount || !sharedSlots[ringSlot] || serial == 0) {
				return;
			}

			ID3D12Resource* backbuffer = nullptr;
			if (FAILED(a_target.swap->GetBuffer(a_backIndex, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backbuffer))) || !backbuffer) {
				return;
			}
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
				rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(a_target.rtvBase + a_backIndex) * rtvStride
			};
			engine.device->CreateRenderTargetView(backbuffer, nullptr, rtv);

			a_slot.allocator->Reset();
			a_slot.list->Reset(a_slot.allocator, a_pso);
			auto* list = a_slot.list;

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = backbuffer;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			list->ResourceBarrier(1, &barrier);

			ID3D12DescriptorHeap* heaps[]{ srvHeap };
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootSignature(rootSig);
			const D3D12_GPU_DESCRIPTOR_HANDLE srv{
				srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
				static_cast<UINT64>(1 + ringSlot) * srvStride
			};
			list->SetGraphicsRootDescriptorTable(0, srv);

			D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_target.width), static_cast<float>(a_target.height), 0.0f, 1.0f };
			D3D12_RECT scissor{ 0, 0, static_cast<LONG>(a_target.width), static_cast<LONG>(a_target.height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &scissor);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->SetPipelineState(a_pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);

			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			list->ResourceBarrier(1, &barrier);
			list->Close();

			if (sharedProduce) {
				engine.directQueue->Wait(sharedProduce, serial);
			}
			ID3D12CommandList* lists[]{ list };
			engine.directQueue->ExecuteCommandLists(1, lists);
			if (sharedConsume) {
				engine.directQueue->Signal(sharedConsume, serial);
			}
			a_slot.fenceValue = nextFenceValue++;
			engine.directQueue->Signal(fence, a_slot.fenceValue);
			backbuffer->Release();

			++drawnFrames;
			if (!firstDrawLogged) {
				firstDrawLogged = true;
				REX::INFO("D3D12Compositor: FIRST OVERLAY DRAW (GPU shared-ring transport, "
						  "slot {} serial {} -> {}x{} target)",
					ringSlot, serial, a_target.width, a_target.height);
			} else if ((drawnFrames % 1200) == 0) {
				REX::DEBUG("D3D12Compositor: {} overlay draws ({} waited on busy slot, {} dropped on timeout, "
						   "{} skipped as concurrent)",
					drawnFrames, waitedBusy, skippedBusy, skippedConcurrent.load(std::memory_order_relaxed));
			}
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
					REX::INFO("D3D12Compositor: FG seam uses only the transparent COPY_SOURCE UI layer");
				}
				return false;
			}
			bool gpu = false;
			std::uint32_t ringSlot = 0;
			std::uint64_t serial = 0;
			{
				std::scoped_lock lk(frameMutex);
				gpu = gpuMode;
				ringSlot = gpuSlot;
				serial = gpuSerial;
			}

			std::scoped_lock ring(ringMutex);
			if (!gpu) {
				if (!seamGpuWarned) {
					seamGpuWarned = true;
					REX::WARN("D3D12Compositor: seam draw requires the GPU shared-ring transport "
							  "(webview2 host); CPU-staged frames are not seam-drawn");
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

			auto* pso = EnsurePipeline(DXGI_FORMAT_R8G8B8A8_UNORM);
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

			++seamDraws;
			if (a_fgTarget) {
				++seamDrawsFgTarget;
			}
			// One-time log per target kind: if the FG line never appears, its
			// hand-off is not being matched.
			if (seamDraws == 1 || (a_fgTarget && seamDrawsFgTarget == 1)) {
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

		// Resize/create the overlay texture + per-slot upload buffers + SRV to
		// match the cached frame. Returns false if there is no frame yet.
		[[nodiscard]] bool EnsureTextureForFrame()
		{
			std::uint32_t w = 0;
			std::uint32_t h = 0;
			DXGI_FORMAT   fmt = DXGI_FORMAT_UNKNOWN;
			{
				std::scoped_lock lk(frameMutex);
				if (cpuPixels.empty()) {
					return false;
				}
				w = frameWidth;
				h = frameHeight;
				fmt = frameFormat;
			}
			if (texture && texWidth == w && texHeight == h && texFormat == fmt) {
				return true;
			}

			WaitForGpuIdle();
			SafeRelease(texture);
			for (auto& slot : cmdSlots) {
				if (slot.upload && slot.mapped) {
					slot.upload->Unmap(0, nullptr);
					slot.mapped = nullptr;
				}
				SafeRelease(slot.upload);
			}

			texWidth = w;
			texHeight = h;
			texFormat = fmt;
			uploadRowPitch = AlignUp(w * 4, kRowPitchAlignment);

			D3D12_HEAP_PROPERTIES defaultHeap{};
			defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC texDesc{};
			texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			texDesc.Width = w;
			texDesc.Height = h;
			texDesc.DepthOrArraySize = 1;
			texDesc.MipLevels = 1;
			texDesc.Format = fmt;
			texDesc.SampleDesc.Count = 1;
			texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			if (FAILED(engine.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture)))) {
				REX::ERROR("D3D12Compositor: failed to create {}x{} overlay texture", w, h);
				return false;
			}

			D3D12_HEAP_PROPERTIES uploadHeap{};
			uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC bufDesc{};
			bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufDesc.Width = static_cast<std::uint64_t>(uploadRowPitch) * h;
			bufDesc.Height = 1;
			bufDesc.DepthOrArraySize = 1;
			bufDesc.MipLevels = 1;
			bufDesc.SampleDesc.Count = 1;
			bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			for (auto& slot : cmdSlots) {
				if (FAILED(engine.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
						D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&slot.upload)))) {
					REX::ERROR("D3D12Compositor: failed to create upload buffer");
					return false;
				}
				if (FAILED(slot.upload->Map(0, nullptr, reinterpret_cast<void**>(&slot.mapped)))) {
					REX::ERROR("D3D12Compositor: failed to map upload buffer");
					return false;
				}
				slot.fenceValue = 0;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = fmt;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			engine.device->CreateShaderResourceView(texture, &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());

			needsFullUpload = true;  // new texture = undefined contents

			REX::INFO("D3D12Compositor: overlay texture ready ({}x{} {})", w, h,
				fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8");
			return true;
		}

		// Get-or-create the PSO for one backbuffer format. Returns nullptr
		// (warn-once via `failed`) when creation fails or more distinct formats
		// appear than the cache holds. Callers gate on Target::supported first,
		// so only formats the overlay renders correctly into land here.
		[[nodiscard]] ID3D12PipelineState* EnsurePipeline(const DXGI_FORMAT a_rtFormat)
		{
			PsoEntry* entry = nullptr;
			for (auto& e : psoCache) {
				if (e.format == a_rtFormat) {
					entry = &e;
					break;
				}
				if (e.format == DXGI_FORMAT_UNKNOWN && !entry) {
					entry = &e;  // first free slot; keep scanning for an exact match
				}
			}
			if (!entry) {
				return nullptr;  // more than kMaxPsoFormats distinct formats (absurd)
			}
			if (entry->format == a_rtFormat) {
				return entry->failed ? nullptr : entry->pso;
			}
			entry->format = a_rtFormat;

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
			desc.RTVFormats[0] = a_rtFormat;
			desc.SampleDesc.Count = 1;

			// Premultiplied-alpha "over" blend for BGRA browser frames.
			auto& rt = desc.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			if (FAILED(engine.device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->pso)))) {
				REX::ERROR("D3D12Compositor: CreateGraphicsPipelineState failed (RT format {} [{}])",
					FormatName(a_rtFormat), static_cast<int>(a_rtFormat));
				entry->pso = nullptr;
				entry->failed = true;
				return nullptr;
			}
			REX::INFO("D3D12Compositor: overlay pipeline ready (RT format {} [{}])",
				FormatName(a_rtFormat), static_cast<int>(a_rtFormat));
			return entry->pso;
		}

		// Find or (re)build the backbuffer target for this swapchain pointer.
		// The returned target's `swap` is a borrowed pointer valid only for the
		// remainder of this present (see the Target comment): the QI ref is
		// dropped immediately so this table never extends a swapchain's
		// lifetime past the game's own release.
		[[nodiscard]] Target* AcquireTarget(IDXGISwapChain* a_swap)
		{
			IDXGISwapChain3* swap3 = nullptr;
			if (FAILED(a_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap3))) || !swap3) {
				return nullptr;  // pre-3 swapchains: no GetCurrentBackBufferIndex
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
					ReleaseTarget(*stalest);
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
			freeSlot->rtvBase = static_cast<std::uint32_t>(freeSlot - targets) * kMaxBackBuffers;
			return RefreshTarget(*freeSlot) ? freeSlot : nullptr;
		}

		// Refresh the cached swapchain dimensions/format. GetDesc1 only, holds no
		// backbuffer refs, so it cannot block ResizeBuffers — the backbuffer is
		// fetched per-present in RecordAndExecute.
		[[nodiscard]] bool RefreshTarget(Target& a_t)
		{
			DXGI_SWAP_CHAIN_DESC1 desc{};
			if (FAILED(a_t.swap->GetDesc1(&desc))) {
				return false;
			}
			const bool formatChanged = !a_t.seen || a_t.format != desc.Format;
			const bool changed = formatChanged || a_t.width != desc.Width || a_t.height != desc.Height ||
				a_t.bufferCount != desc.BufferCount;
			a_t.seen = true;
			a_t.width = desc.Width;
			a_t.height = desc.Height;
			a_t.format = desc.Format;
			a_t.bufferCount = (std::min)(desc.BufferCount, kMaxBackBuffers);

			if (formatChanged) {
				// Detect-and-degrade (ROADMAP P1 "HDR / frame-gen"): drawing our
				// sRGB-encoded pixels into an HDR (PQ/scRGB) or _SRGB backbuffer
				// renders wrong colors, so an unsupported format skips this
				// swapchain entirely, warned once per change.
				a_t.supported = IsSupportedRtFormat(a_t.format);
				if (!a_t.supported) {
					REX::WARN("D3D12Compositor: swapchain 0x{:X} uses backbuffer format {} [{}] ({}) — the overlay "
							  "cannot render correctly into it yet and will NOT draw on this swapchain. If the "
							  "overlay is invisible, switch Starfield to SDR / disable HDR and frame generation. "
							  "Full HDR output is tracked in the project roadmap.",
						a_t.key, FormatName(a_t.format), static_cast<int>(a_t.format), OutputColorModeInfo(a_t.swap));
				}
			}

			if (changed && a_t.logged) {
				REX::DEBUG("D3D12Compositor: swapchain 0x{:X} now {}x{}, {} buffers, RT format {} [{}]",
					a_t.key, a_t.width, a_t.height, a_t.bufferCount, FormatName(a_t.format), static_cast<int>(a_t.format));
			}
			if (!a_t.logged) {
				a_t.logged = true;
				REX::INFO("D3D12Compositor: {} swapchain 0x{:X} ({}x{}, {} buffers, RT format {} [{}])",
					a_t.supported ? "drawing on" : "SKIPPING (unsupported format)",
					a_t.key, a_t.width, a_t.height, a_t.bufferCount, FormatName(a_t.format), static_cast<int>(a_t.format));
			}
			return true;
		}

		void RecordAndExecute(CmdSlot& a_slot, Target& a_target, const std::uint32_t a_backIndex,
			ID3D12PipelineState* a_pso)
		{
			// Fetch the backbuffer fresh each present and release it before
			// returning: a ref held across frames blocks the game's
			// ResizeBuffers. The swapchain keeps its own ref, and the command
			// queue keeps the resource alive for the GPU work we submit.
			ID3D12Resource* backbuffer = nullptr;
			if (FAILED(a_target.swap->GetBuffer(a_backIndex, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backbuffer))) || !backbuffer) {
				return;
			}
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
				rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(a_target.rtvBase + a_backIndex) * rtvStride
			};
			engine.device->CreateRenderTargetView(backbuffer, nullptr, rtv);

			a_slot.allocator->Reset();
			a_slot.list->Reset(a_slot.allocator, a_pso);
			auto* list = a_slot.list;

			// Upload a new frame into the texture, only when one arrived. Only
			// the dirty region is copied into the mapped upload buffer and box-
			// copied to the texture: each ring slot's buffer holds stale bytes
			// outside the rect last written into it, which is safe only because
			// pSrcBox never reads them. A recreated texture forces a full-frame
			// pass (needsFullUpload).
			bool      uploaded = false;
			DirtyRect rect{};
			{
				std::scoped_lock lk(frameMutex);
				if (frameDirty && frameWidth == texWidth && frameHeight == texHeight) {
					rect = dirtyRegion;
					if (needsFullUpload || rect.Empty() ||
						rect.x + rect.width > texWidth || rect.y + rect.height > texHeight) {
						rect = DirtyRect::Full(texWidth, texHeight);
					}
					const auto rowBytes = texWidth * 4u;
					const auto rowStart = static_cast<std::size_t>(rect.x) * 4u;
					const auto rowLen = static_cast<std::size_t>(rect.width) * 4u;
					for (std::uint32_t y = rect.y; y < rect.y + rect.height; ++y) {
						std::memcpy(a_slot.mapped + static_cast<std::size_t>(y) * uploadRowPitch + rowStart,
							cpuPixels.data() + static_cast<std::size_t>(y) * rowBytes + rowStart, rowLen);
					}
					frameDirty = false;
					needsFullUpload = false;
					uploaded = true;
				}
			}
			if (uploaded) {
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = a_slot.upload;
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint.Footprint.Format = texFormat;
				src.PlacedFootprint.Footprint.Width = texWidth;
				src.PlacedFootprint.Footprint.Height = texHeight;
				src.PlacedFootprint.Footprint.Depth = 1;
				src.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = texture;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = 0;
				// Box coordinates are in the source subresource's texture space,
				// which matches the texture 1:1 (rows at y*uploadRowPitch).
				D3D12_BOX box{};
				box.left = rect.x;
				box.top = rect.y;
				box.front = 0;
				box.right = rect.x + rect.width;
				box.bottom = rect.y + rect.height;
				box.back = 1;
				list->CopyTextureRegion(&dst, rect.x, rect.y, 0, &src, &box);
			}

			const auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
				D3D12_RESOURCE_BARRIER b{};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource = r;
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				b.Transition.StateBefore = before;
				b.Transition.StateAfter = after;
				list->ResourceBarrier(1, &b);
			};

			barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			barrier(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

			ID3D12DescriptorHeap* heaps[]{ srvHeap };
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootSignature(rootSig);
			list->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());

			D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_target.width), static_cast<float>(a_target.height), 0.0f, 1.0f };
			D3D12_RECT scissor{ 0, 0, static_cast<LONG>(a_target.width), static_cast<LONG>(a_target.height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &scissor);

			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->SetPipelineState(a_pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);

			barrier(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			barrier(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

			list->Close();
			ID3D12CommandList* lists[]{ list };
			engine.directQueue->ExecuteCommandLists(1, lists);
			a_slot.fenceValue = nextFenceValue++;
			engine.directQueue->Signal(fence, a_slot.fenceValue);

			// Safe to drop now: the queue holds the resource alive until the GPU
			// finishes, and the swapchain owns it regardless.
			backbuffer->Release();

			++drawnFrames;
			if (!firstDrawLogged) {
				firstDrawLogged = true;
				REX::INFO("D3D12Compositor: FIRST OVERLAY DRAW submitted over the game backbuffer "
						  "({}x{} overlay -> {}x{} target) — Phase 3 composition live",
					texWidth, texHeight, a_target.width, a_target.height);
			} else if ((drawnFrames % 1200) == 0) {
				REX::DEBUG("D3D12Compositor: {} overlay draws ({} waited on busy slot, {} dropped on timeout, "
						   "{} skipped as concurrent)",
					drawnFrames, waitedBusy, skippedBusy, skippedConcurrent.load(std::memory_order_relaxed));
			}
		}

		// Tick thread. The game is ticking (we're being called), so it is
		// presenting — if no present has reached our thunk for a while, another
		// tool re-hooked Present over us without chaining and the overlay
		// silently stopped drawing. Warn once; log recovery if presents return.
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
						  "IDXGISwapChain::Present without chaining. The OSF UI overlay cannot draw until it is "
						  "restored. Try changing the load/injection order of overlay tools, and include this "
						  "line plus your overlay stack in reports.",
					kStallMs / 1000);
			}
		}

		// GPU-transport submit: just remember which ring slot/serial to draw.
		// No pixel copy — the present thread samples the shared texture.
		void CacheSharedFrame(const FrameBufferView& a_frame)
		{
			if (a_frame.frameIndex == lastSubmittedIndex) {
				return;
			}
			lastSubmittedIndex = a_frame.frameIndex;
			std::scoped_lock lk(frameMutex);
			gpuMode = true;
			gpuSlot = static_cast<std::uint32_t>(a_frame.sharedSlot);
			gpuSerial = a_frame.frameIndex;
			gpuWidth = a_frame.width;
			gpuHeight = a_frame.height;
		}

		void CacheFrame(const FrameBufferView& a_frame)
		{
			if (a_frame.frameIndex == lastSubmittedIndex) {
				return;  // renderer returned its cached frame; nothing new
			}
			lastSubmittedIndex = a_frame.frameIndex;

			const auto rowBytes = a_frame.width * 4u;
			std::scoped_lock lk(frameMutex);
			// cpuPixels persists across frames, so only the delivered dirty rect
			// needs copying. A just-(re)sized staging buffer (garbage elsewhere)
			// or an absent/out-of-bounds rect falls back to a full copy.
			const bool realloc = frameWidth != a_frame.width || frameHeight != a_frame.height ||
				cpuPixels.size() != static_cast<std::size_t>(rowBytes) * a_frame.height;
			if (realloc) {
				cpuPixels.resize(static_cast<std::size_t>(rowBytes) * a_frame.height);
			}
			DirtyRect rect = a_frame.dirty;
			if (realloc || rect.Empty() ||
				rect.x + rect.width > a_frame.width || rect.y + rect.height > a_frame.height) {
				rect = DirtyRect::Full(a_frame.width, a_frame.height);
			}
			const auto rowStart = static_cast<std::size_t>(rect.x) * 4u;
			const auto rowLen = static_cast<std::size_t>(rect.width) * 4u;
			for (std::uint32_t y = rect.y; y < rect.y + rect.height; ++y) {
				std::memcpy(cpuPixels.data() + static_cast<std::size_t>(y) * rowBytes + rowStart,
					a_frame.pixels.data() + static_cast<std::size_t>(y) * a_frame.strideBytes + rowStart, rowLen);
			}
			frameWidth = a_frame.width;
			frameHeight = a_frame.height;
			frameFormat = ToDxgiFormat(a_frame.format);
			// Presents can lag submits: accumulate until an upload consumes it.
			dirtyRegion = frameDirty ? DirtyRect::Union(dirtyRegion, rect) : rect;
			frameDirty = true;
		}
	};

	D3D12Compositor::D3D12Compositor() = default;
	D3D12Compositor::~D3D12Compositor() = default;

	bool D3D12Compositor::Initialize()
	{
		_impl = std::make_unique<Impl>();
		_impl->devMode = Log::DevMode();
		REX::INFO("D3D12Compositor: initialized (present-time overlay; engine device/queue + Present hook "
				  "are set up on the first submitted frame)");
		return true;
	}

	void D3D12Compositor::Shutdown()
	{
		if (_impl) {
			REX::INFO("D3D12Compositor: shutdown after {} overlay draw(s) ({} waited on busy slot, {} dropped on timeout)",
				_impl->drawnFrames, _impl->waitedBusy, _impl->skippedBusy);
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
			_impl->CacheFrame(a_frame);
		}
		_impl->EnsureSetup();
		_impl->CheckPresentLiveness();
	}

	void D3D12Compositor::SetSeamDrawMode(const bool a_enabled)
	{
		if (_impl) {
			_impl->seamMode.store(a_enabled, std::memory_order_relaxed);
			if (a_enabled) {
				REX::INFO("D3D12Compositor: seam draw active — the present hook keeps "
						  "discovery/plumbing duties and no longer draws");
			}
		}
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
