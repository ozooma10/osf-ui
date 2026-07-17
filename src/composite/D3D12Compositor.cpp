#include "composite/D3D12Compositor.h"

#include "composite/EngineD3D12.h"
#include "core/Log.h"

// GDI-free Win32/D3D12 so the ERROR macro never collides with REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace OSFUI
{
	namespace
	{
		// Ring depth must cover every present that can be in flight before the
		// oldest slot's GPU work completes. The game drives MORE THAN ONE
		// swapchain through this one ring (two seen in-game), so each swapchain
		// only gets kCmdSlots/N frames of depth — keep this comfortably above
		// (max swapchains in play) x (game's frame-queue depth) so a slot is
		// almost never still busy when we cycle back to it.
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

		// Backbuffer formats the overlay is KNOWN to render correctly into:
		// 8-bit UNORM, sRGB-encoded SDR — the in-game-verified path. Everything
		// else is refused per swapchain with a loud warning instead of drawing
		// wrong: HDR10 (R10G10B10A2 + PQ) and scRGB (FP16) need a different
		// encode in the pixel shader, and an _SRGB view would double-encode our
		// already-sRGB pixels. Full-HDR scope lives in docs/ROADMAP.md (P1).
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
		// Used to log who owns the Present slot before we hook it — if ReShade/
		// RTSS/Steam overlay hooked first, their module shows up here instead of
		// dxgi.dll, which is exactly what a coexistence bug report needs.
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

		// Best-effort diagnostic for the unsupported-format warning: is the
		// output this swapchain sits on in HDR (advanced color) mode? DXGI has
		// no getter for a swapchain's own color space, so the output's state is
		// the closest signal a log reader can act on.
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

		// The overlay texture is BGRA8 premultiplied alpha (Ultralight's CPU
		// surface). Sample straight through; the premultiplied-over blend is
		// configured in the PSO.
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
		// ---- located engine objects ----
		EngineD3D12   engine{};
		bool          devMode{ false };
		std::atomic_bool visible{ false };

		// Output-size signal -> runtime (resize the view to match the screen).
		OutputResizeCallback onOutputResize;
		std::uint32_t        notifiedOutputW{ 0 };
		std::uint32_t        notifiedOutputH{ 0 };
		std::atomic_bool     outputSizeKnown{ false };

		// ---- setup state ----
		bool setupAttempted{ false };
		bool setupOk{ false };

		// ---- CPU frame staging (Submit thread <-> present thread) ----
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

		// ---- shared GPU objects (created once) ----
		ID3D12Fence*          fence{ nullptr };
		HANDLE                fenceEvent{ nullptr };
		std::uint64_t         nextFenceValue{ 1 };
		ID3D12RootSignature*  rootSig{ nullptr };
		// One PSO per *supported* backbuffer format actually seen — the game and
		// an injected frame-gen/overlay swapchain can legitimately differ, so
		// first-seen-wins would starve one of them. `failed` marks a format whose
		// PSO creation failed so we don't retry (and spam the log) every present.
		struct PsoEntry
		{
			DXGI_FORMAT          format{ DXGI_FORMAT_UNKNOWN };
			ID3D12PipelineState* pso{ nullptr };
			bool                 failed{ false };
		};
		PsoEntry              psoCache[kMaxPsoFormats]{};
		ID3D12DescriptorHeap* srvHeap{ nullptr };  // shader-visible, 1 SRV (the texture)
		ID3D12DescriptorHeap* rtvHeap{ nullptr };  // backbuffer RTVs
		std::uint32_t         rtvStride{ 0 };
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

		// ---- overlay texture (sized to the frame) ----
		ID3D12Resource* texture{ nullptr };
		std::uint32_t   texWidth{ 0 };
		std::uint32_t   texHeight{ 0 };
		DXGI_FORMAT     texFormat{ DXGI_FORMAT_UNKNOWN };
		std::uint32_t   uploadRowPitch{ 0 };
		// A freshly (re)created texture has undefined contents — the next
		// upload must cover the whole frame regardless of the dirty region.
		// Present-thread-only (set in EnsureTextureForFrame, consumed in
		// RecordAndExecute).
		bool            needsFullUpload{ true };

		// ---- per-swapchain backbuffer targets ----
		// NOTE: we deliberately do NOT cache the swapchain's backbuffer
		// resources. Holding a backbuffer reference blocks the game's
		// IDXGISwapChain::ResizeBuffers (resolution change / alt-tab /
		// fullscreen transition), which crashed the game (2026-06-12). Each
		// present does GetBuffer + CreateRTV fresh and releases the buffer,
		// so we never hold a ref across frames. (The swapchain ref itself is
		// fine — only outstanding BACKBUFFER refs block ResizeBuffers.)
		struct Target
		{
			IDXGISwapChain3* swap{ nullptr };  // owned (QI'd), key is the raw self ptr
			std::uintptr_t   key{ 0 };
			std::uint32_t    width{ 0 };
			std::uint32_t    height{ 0 };
			DXGI_FORMAT      format{ DXGI_FORMAT_UNKNOWN };
			std::uint32_t    bufferCount{ 0 };
			std::uint32_t    rtvBase{ 0 };  // base index into rtvHeap
			bool             seen{ false };  // dims known yet?
			bool             logged{ false };
			// The overlay renders correctly into this swapchain's format
			// (IsSupportedRtFormat). Re-evaluated on every format change (e.g.
			// the user toggles HDR mid-session), warned once per change.
			bool             supported{ false };
		};
		Target targets[kMaxSwapchains]{};

		// ---- stats ----
		std::uint64_t drawnFrames{ 0 };
		std::uint64_t waitedBusy{ 0 };   // presents that had to wait for a busy slot
		std::uint64_t skippedBusy{ 0 };  // presents dropped because the wait timed out (GPU hung)
		bool          firstDrawLogged{ false };
		bool          targetsFullLogged{ false };

		// ---- hook-liveness watchdog (coexistence diagnostic) ----
		// Another overlay that (re)hooks Present non-chainingly after us would
		// silently stop our thunk from ever running — the overlay just vanishes.
		// OnPresent stamps this every call (present thread); Submit's watchdog
		// (tick thread) warns when the game keeps ticking but no present has
		// reached us for a while. No false positive on focus loss: the game
		// pauses the tick loop too, so the watchdog isn't polled then.
		std::atomic<std::uint64_t> lastPresentMs{ 0 };
		std::uint64_t              setupCompletedMs{ 0 };  // tick thread only
		bool                       bypassWarned{ false };  // tick thread only

		~Impl()
		{
			g_overlay.store(nullptr);
			WaitForGpuIdle();
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
			SafeRelease(a_t.swap);  // no cached backbuffers to release
			a_t = Target{};
		}

		// ================= setup (Submit / tick thread) =================

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

			g_overlay.store(this);
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

			// Descriptor heaps.
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.NumDescriptors = 1;
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(dev->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&srvHeap)))) {
				REX::ERROR("D3D12Compositor: CreateDescriptorHeap(SRV) failed");
				return false;
			}

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
			return true;
		}

		// Capture the swapchain Present vtable from a throwaway swapchain (the
		// "Kiero" technique): the vtable is shared by every swapchain in the
		// process, so one slot-8 swap hooks the game's real presents too. No
		// engine offsets involved — pure DXGI.
		[[nodiscard]] bool InstallPresentHook()
		{
			IDXGIFactory2* factory = nullptr;
			if (FAILED(::CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
				REX::ERROR("D3D12Compositor: CreateDXGIFactory2 failed");
				return false;
			}

			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = ::DefWindowProcW;
			wc.hInstance = ::GetModuleHandleW(nullptr);
			wc.lpszClassName = L"OSFUIOverlayDummyWnd";
			::RegisterClassExW(&wc);
			HWND dummyWnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
				0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
			if (!dummyWnd) {
				REX::ERROR("D3D12Compositor: dummy window creation failed");
				SafeRelease(factory);
				::UnregisterClassW(wc.lpszClassName, wc.hInstance);
				return false;
			}

			DXGI_SWAP_CHAIN_DESC1 scDesc{};
			scDesc.Width = 1;
			scDesc.Height = 1;
			scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			scDesc.SampleDesc.Count = 1;
			scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			scDesc.BufferCount = 2;
			scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

			IDXGISwapChain1* dummySwap = nullptr;
			const auto hr = factory->CreateSwapChainForHwnd(engine.directQueue, dummyWnd, &scDesc, nullptr, nullptr, &dummySwap);
			bool ok = false;
			if (SUCCEEDED(hr) && dummySwap) {
				auto** vtbl = *reinterpret_cast<void***>(dummySwap);
				auto& slot8 = vtbl[8];  // IDXGISwapChain::Present
				g_originalPresent.store(reinterpret_cast<PresentFn>(slot8));

				// Coexistence diagnostic: who owns Present before we hook it?
				// Clean stack -> dxgi.dll. ReShade/RTSS/Steam-overlay/frame-gen
				// hooked first -> their module. Either way we CHAIN (we call
				// whatever was there as "original"), so hooking after others is
				// the safe order; the log makes the stack order reportable.
				const auto owner = ModuleNameOfAddress(reinterpret_cast<const void*>(g_originalPresent.load()));
				if (owner.find("dxgi.dll") == std::string::npos && owner.find("<unknown") == std::string::npos) {
					REX::WARN("D3D12Compositor: Present slot 8 already points into '{}' — another overlay/hook tool "
							  "is ahead of us; chaining after it. If the overlay misbehaves, include this line in reports.",
						owner);
				} else {
					REX::INFO("D3D12Compositor: Present slot 8 owner before hook: '{}'", owner);
				}

				DWORD oldProtect = 0;
				if (::VirtualProtect(&slot8, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
					slot8 = reinterpret_cast<void*>(&PresentThunk);
					::VirtualProtect(&slot8, sizeof(void*), oldProtect, &oldProtect);
					// Paranoia read-back: if another tool raced the same slot
					// between our write and here, say so rather than wonder later.
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
			} else {
				REX::ERROR("D3D12Compositor: dummy CreateSwapChainForHwnd failed (hr=0x{:08X})", static_cast<std::uint32_t>(hr));
			}

			SafeRelease(dummySwap);
			SafeRelease(factory);
			::DestroyWindow(dummyWnd);
			::UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return ok;
		}

		// ================= present (render thread) =================

		static HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swap, UINT a_sync, UINT a_flags)
		{
			if (auto* self = static_cast<Impl*>(g_overlay.load())) {
				self->OnPresent(a_swap, a_flags);
			}
			const auto original = g_originalPresent.load();
			return original ? original(a_swap, a_sync, a_flags) : S_OK;
		}

		void OnPresent(IDXGISwapChain* a_swap, UINT a_flags)
		{
			// Watchdog stamp FIRST (before any early-out): it answers "does the
			// present stream still reach our thunk at all", independent of
			// visibility or frame availability.
			lastPresentMs.store(::GetTickCount64(), std::memory_order_relaxed);

			if (!setupOk || !a_swap || (a_flags & DXGI_PRESENT_TEST)) {
				return;
			}

			// Discover the real output even while hidden. The first submitted
			// manifest-sized frame installs this hook, but Runtime deliberately
			// keeps it invisible until this callback has resized the web view.
			// AcquireTarget only reads GetDesc1 and holds no backbuffer reference.
			auto* target = AcquireTarget(a_swap);
			if (!target) {
				return;
			}

			// Tell the runtime the real output size so it can size the view to
			// match (aspect-correct). Fire on first sight and on any change.
			if (onOutputResize && (target->width != notifiedOutputW || target->height != notifiedOutputH)) {
				notifiedOutputW = target->width;
				notifiedOutputH = target->height;
				onOutputResize(target->width, target->height);
				outputSizeKnown.store(true, std::memory_order_release);
			}

			if (!visible.load(std::memory_order_relaxed)) {
				return;
			}

			// Make sure the texture matches the latest frame dims; upload if a
			// new frame arrived. EnsureTexture/Upload read the CPU staging
			// buffer under the lock.
			if (!EnsureTextureForFrame()) {
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
				// allocator/upload buffer. Skipping the draw here (the old
				// behavior) left THIS present without the overlay -> visible
				// flicker (~7% of presents, since multiple swapchains share this
				// ring). Wait instead: the overlay is one triangle, so the fence
				// is almost always already signaled and this returns at once.
				// Only a genuinely stuck GPU (timeout) falls through to a skip,
				// which stays safe — reusing in-flight resources would corrupt.
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

			RecordAndExecute(slot, *target, backIndex, pipeline);
			++cmdIndex;
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

		// Get-or-create the PSO for one supported backbuffer format. Returns
		// nullptr (warn-once via `failed`) when creation fails or more distinct
		// formats appear than the cache holds. Callers gate on Target::supported
		// first, so only formats the overlay renders correctly into land here.
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

			// Premultiplied-alpha "over" blend (Ultralight's BGRA is premult).
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
		[[nodiscard]] Target* AcquireTarget(IDXGISwapChain* a_swap)
		{
			const auto key = reinterpret_cast<std::uintptr_t>(a_swap);
			Target* freeSlot = nullptr;
			for (auto& t : targets) {
				if (t.key == key) {
					return RefreshTarget(t) ? &t : nullptr;
				}
				if (t.key == 0 && !freeSlot) {
					freeSlot = &t;
				}
			}
			if (!freeSlot) {
				if (!targetsFullLogged) {
					targetsFullLogged = true;
					REX::WARN("D3D12Compositor: more than {} swapchains seen; ignoring extras", kMaxSwapchains);
				}
				return nullptr;
			}

			IDXGISwapChain3* swap3 = nullptr;
			if (FAILED(a_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap3))) || !swap3) {
				return nullptr;  // pre-3 swapchains: no GetCurrentBackBufferIndex
			}
			freeSlot->swap = swap3;
			freeSlot->key = key;
			freeSlot->rtvBase = static_cast<std::uint32_t>(freeSlot - targets) * kMaxBackBuffers;
			return RefreshTarget(*freeSlot) ? freeSlot : nullptr;
		}

		// Refresh the cached swapchain dimensions/format. Cheap (GetDesc1
		// only) and holds no backbuffer refs, so it never blocks ResizeBuffers
		// — the actual backbuffer is fetched per-present in RecordAndExecute.
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
				// renders wrong colors, so an unsupported format means we skip
				// this swapchain entirely and say so once per change. Re-checked
				// whenever the format changes (e.g. HDR toggled mid-session).
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
			// Fetch the backbuffer fresh each present and release it before we
			// return — never hold a ref across frames (that blocks the game's
			// ResizeBuffers). The swapchain keeps its own ref, and the command
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

			// Upload a new frame into the texture (only when one arrived). Only
			// the dirty region is copied into the mapped upload buffer AND box-
			// copied to the texture: each ring slot's buffer holds stale bytes
			// outside whatever rect was last written into it, which is safe
			// precisely because pSrcBox never reads them. A recreated texture
			// forces a full-frame pass (needsFullUpload).
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

			// Drop our backbuffer ref now — the queue holds the resource alive
			// until the GPU finishes; the swapchain owns it regardless.
			backbuffer->Release();

			++drawnFrames;
			if (!firstDrawLogged) {
				firstDrawLogged = true;
				REX::INFO("D3D12Compositor: FIRST OVERLAY DRAW submitted over the game backbuffer "
						  "({}x{} overlay -> {}x{} target) — Phase 3 composition live",
					texWidth, texHeight, a_target.width, a_target.height);
			} else if ((drawnFrames % 1200) == 0) {
				REX::DEBUG("D3D12Compositor: {} overlay draws ({} waited on busy slot, {} dropped on timeout)",
					drawnFrames, waitedBusy, skippedBusy);
			}
		}

		// ================= submit (tick thread) =================

		// Coexistence watchdog: the game is ticking (we're being called), so it
		// is presenting — if no present has reached our thunk for a while, some
		// other tool re-hooked Present over us WITHOUT chaining, and the overlay
		// silently stopped drawing. Warn once with the actionable context; log
		// recovery if presents come back (e.g. the tool was closed).
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

		void CacheFrame(const FrameBufferView& a_frame)
		{
			if (a_frame.frameIndex == lastSubmittedIndex) {
				return;  // renderer returned its cached frame; nothing new
			}
			lastSubmittedIndex = a_frame.frameIndex;

			const auto rowBytes = a_frame.width * 4u;
			std::scoped_lock lk(frameMutex);
			// cpuPixels persists across frames, so only the delivered dirty
			// rect needs copying — except when the staging buffer was just
			// (re)sized (its other pixels are garbage) or the rect is absent/
			// out of bounds, which fall back to a full copy.
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
		_impl->CacheFrame(a_frame);
		_impl->EnsureSetup();
		_impl->CheckPresentLiveness();
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
