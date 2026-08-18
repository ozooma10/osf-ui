#include "Composite/D3D12Compositor.h"

#include "Composite/EngineD3D12.h"
#include "Composite/UiTargetFormat.h"
#include "Core/Log.h"
#include "REL/Utility.h"

#include "Composite/D3D12Prologue.h" 

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Win32Util.h"

namespace OSFUI
{
	namespace
	{

		using osfui::win32::SafeRelease;

		// Fullscreen triangle from SV_VertexID (no vertex buffer). UV (0,0) is the top-left so the texture's row 0 lands at the top of the screen.
		constexpr const char* kVertexShader = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

		// The overlay texture is BGRA8 premultiplied alpha. Sample straight through; the premultiplied-over blend is configured in the PSO.
		constexpr const char* kPixelShader = R"(
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return gTex.Sample(gSmp, uv);
}
)";

		std::atomic<void*> g_overlay{ nullptr };  // D3D12Compositor::Impl*

		using OverlayDrawFn = bool (*)(ID3D12GraphicsCommandList*, ID3D12Resource*, bool, bool);
		std::atomic<OverlayDrawFn> g_overlayDrawFn{ nullptr };

		using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
		using QueueExecutedFn = void (*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
		std::atomic<ExecuteCommandListsFn> g_origExecuteCommandLists{ nullptr };
		std::atomic<QueueExecutedFn> g_queueExecutedFn{ nullptr };
		constexpr std::size_t kExecuteCommandListsSlot = 10;

		void STDMETHODCALLTYPE ExecuteCommandListsThunk(ID3D12CommandQueue* a_queue, const UINT a_count, ID3D12CommandList* const* a_lists)
		{
			if (const auto original = g_origExecuteCommandLists.load(std::memory_order_acquire)) {
				original(a_queue, a_count, a_lists);
			}
			if (const auto notify = g_queueExecutedFn.load(std::memory_order_acquire)) {
				notify(a_queue, a_count, a_lists);
			}
		}

		[[nodiscard]] ID3DBlob* CompileShader(const char* a_src, const char* a_entry, const char* a_target)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			const auto hr = ::D3DCompile(a_src, std::strlen(a_src), nullptr, nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				REX::ERROR("D3D12Compositor: shader '{}' compile failed (hr=0x{:08X}): {}", a_target, static_cast<std::uint32_t>(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			}
			SafeRelease(errors);
			return code;
		}

		struct DrawResources
		{
			struct OverlayPipeline
			{
				DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
				ID3D12PipelineState* state{ nullptr };
				bool failed{ false };
			};

			ID3D12Fence* idleFence{ nullptr };
			HANDLE idleFenceEvent{ nullptr };
			std::uint64_t nextIdleFenceValue{ 1 };
			bool idleFailureLogged{ false };

			ID3D12RootSignature* rootSignature{ nullptr };
			OverlayPipeline pipelines[2]{
				{ DXGI_FORMAT_R8G8B8A8_UNORM },
				{ DXGI_FORMAT_R16G16B16A16_FLOAT },
			};
			ID3D12DescriptorHeap* srvHeap{ nullptr };
			std::uint32_t srvStride{ 0 };
			ID3D12DescriptorHeap* rtvHeap{ nullptr };
			ID3DBlob* vsBlob{ nullptr };
			ID3DBlob* psBlob{ nullptr };

			bool Create(ID3D12Device* a_device)
			{
				if (FAILED(a_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
						reinterpret_cast<void**>(&idleFence)))) {
					REX::ERROR("D3D12Compositor: CreateFence failed");
					return false;
				}
				idleFenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (!idleFenceEvent) {
					REX::ERROR("D3D12Compositor: CreateEvent failed");
					return false;
				}

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
				const auto rsHr = a_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
					__uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&rootSignature));
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

				D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
				srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				srvDesc.NumDescriptors = SharedRingDesc::kMaxSlots;
				srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(a_device->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap),
						reinterpret_cast<void**>(&srvHeap)))) {
					REX::ERROR("D3D12Compositor: CreateDescriptorHeap(SRV) failed");
					return false;
				}
				srvStride = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
				rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				rtvDesc.NumDescriptors = 1;
				if (FAILED(a_device->CreateDescriptorHeap(&rtvDesc, __uuidof(ID3D12DescriptorHeap),
						reinterpret_cast<void**>(&rtvHeap)))) {
					REX::ERROR("D3D12Compositor: RTV heap creation failed");
					return false;
				}

				return true;
			}

			bool WaitForGpuIdle(ID3D12CommandQueue* a_queue)
			{
				if (!idleFence || !idleFenceEvent || !a_queue) {
					return true;
				}
				const auto value = nextIdleFenceValue++;
				HRESULT hr = a_queue->Signal(idleFence, value);
				if (SUCCEEDED(hr) && idleFence->GetCompletedValue() < value) {
					hr = idleFence->SetEventOnCompletion(value, idleFenceEvent);
					if (SUCCEEDED(hr) && ::WaitForSingleObject(idleFenceEvent, 2000) == WAIT_OBJECT_0) {
						return idleFence->GetCompletedValue() >= value;
					}
				} else if (SUCCEEDED(hr)) {
					return true;
				}
				if (!idleFailureLogged) {
					idleFailureLogged = true;
					REX::ERROR("D3D12Compositor: GPU idle wait failed or timed out (hr=0x{:08X}); "
						       "ring retirement is deferred",
						static_cast<std::uint32_t>(hr));
				}
				return false;
			}

			ID3D12PipelineState* EnsurePipeline(ID3D12Device* a_device, DXGI_FORMAT a_rtvFormat)
			{
				auto* cached = std::ranges::find_if(pipelines,
					[a_rtvFormat](const OverlayPipeline& a_pipeline) {
						return a_pipeline.format == a_rtvFormat;
					});
				if (cached == std::end(pipelines)) {
					return nullptr;
				}
				if (cached->state || cached->failed) {
					return cached->state;
				}

				D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
				desc.pRootSignature = rootSignature;
				desc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
				desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
				desc.SampleMask = UINT_MAX;
				desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
				desc.RasterizerState.DepthClipEnable = TRUE;
				desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				desc.NumRenderTargets = 1;
				desc.RTVFormats[0] = a_rtvFormat;
				desc.SampleDesc.Count = 1;

				// Premultiplied-alpha over, matching Starfield's UI composition.
				auto& rt = desc.BlendState.RenderTarget[0];
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D12_BLEND_ONE;
				rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ONE;
				rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

				if (FAILED(a_device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState),
						reinterpret_cast<void**>(&cached->state)))) {
					REX::ERROR("D3D12Compositor: overlay CreateGraphicsPipelineState failed for {}",
						UiTargetFormat::Name(a_rtvFormat));
					cached->state = nullptr;
					cached->failed = true;
					return nullptr;
				}
				REX::INFO("D3D12Compositor: overlay pipeline ready (premultiplied over, {})",
					UiTargetFormat::Name(a_rtvFormat));
				return cached->state;
			}

			void Release()
			{
				for (auto& pipeline : pipelines) {
					SafeRelease(pipeline.state);
				}
				SafeRelease(rootSignature);
				SafeRelease(srvHeap);
				SafeRelease(rtvHeap);
				SafeRelease(vsBlob);
				SafeRelease(psBlob);
				SafeRelease(idleFence);
				if (idleFenceEvent) {
					::CloseHandle(idleFenceEvent);
					idleFenceEvent = nullptr;
				}
			}
		};

		struct SharedRingState
		{
			struct Frame
			{
				bool ready{ false };
				std::uint32_t slot{ 0 };
				std::uint64_t serial{ 0 };
			};

			std::mutex drawMutex;
			// Pending announcements are published and adopted on the game thread.
			SharedRingDesc pending{};
			bool pendingDirty{ false };
			// Open and retired GPU objects are guarded by drawMutex.
			ID3D12Resource* slots[SharedRingDesc::kMaxSlots]{};
			std::uint32_t slotCount{ 0 };
			ID3D12Fence* produceFence{ nullptr };
			ID3D12Fence* consumeFence{ nullptr };
			ID3D12Resource* retiredSlots[SharedRingDesc::kMaxSlots]{};
			ID3D12Fence* retiredProduceFence{ nullptr };
			ID3D12Fence* retiredConsumeFence{ nullptr };
			std::uint32_t readySlot{ 0 };
			std::uint64_t readySerial{ 0 };

			std::mutex frameMutex;
			std::uint64_t lastSubmittedIndex{ 0 };
			bool frameReady{ false };
			std::uint32_t frameSlot{ 0 };
			std::uint64_t frameSerial{ 0 };

			static void CloseHandles(SharedRingDesc& a_desc)
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

			void SetPending(const SharedRingDesc& a_desc)
			{
				if (pendingDirty) {
					CloseHandles(pending);
				}
				pending = a_desc;
				pendingDirty = true;
			}

			SharedRingDesc TakePending()
			{
				auto result = pending;
				pending = {};
				pendingDirty = false;
				return result;
			}

			void DeferPending(SharedRingDesc& a_desc)
			{
				pending = a_desc;
				a_desc = {};
				pendingDirty = true;
			}

			void ClosePending()
			{
				if (pendingDirty) {
					CloseHandles(pending);
					pendingDirty = false;
				}
			}

			void Retire()
			{
				// Keep the previous generation alive for already-recorded engine lists.
				for (auto*& slot : retiredSlots) {
					SafeRelease(slot);
				}
				SafeRelease(retiredProduceFence);
				SafeRelease(retiredConsumeFence);
				for (std::size_t i = 0; i < SharedRingDesc::kMaxSlots; ++i) {
					retiredSlots[i] = slots[i];
					slots[i] = nullptr;
				}
				retiredProduceFence = produceFence;
				produceFence = nullptr;
				retiredConsumeFence = consumeFence;
				consumeFence = nullptr;
				slotCount = 0;
				readySlot = 0;
				readySerial = 0;
			}

			void Release()
			{
				for (auto*& slot : retiredSlots) {
					SafeRelease(slot);
				}
				SafeRelease(retiredProduceFence);
				SafeRelease(retiredConsumeFence);
				for (auto*& slot : slots) {
					SafeRelease(slot);
				}
				slotCount = 0;
				SafeRelease(produceFence);
				SafeRelease(consumeFence);
			}

			Frame SnapshotFrame()
			{
				std::scoped_lock lock(frameMutex);
				return { frameReady, frameSlot, frameSerial };
			}

			void CacheFrame(const FrameBufferView& a_frame)
			{
				if (a_frame.frameIndex == lastSubmittedIndex) {
					return;
				}
				lastSubmittedIndex = a_frame.frameIndex;
				std::scoped_lock lock(frameMutex);
				frameReady = true;
				frameSlot = a_frame.sharedSlot;
				frameSerial = a_frame.frameIndex;
			}
		};

		class ConsumeTracker final
		{
		public:
			void Track(ID3D12GraphicsCommandList* a_list, ID3D12Fence* a_fence, std::uint64_t a_serial)
			{
				if (!a_list || !a_fence || a_serial == 0) {
					return;
				}
				std::scoped_lock lock(mutex);
				auto [it, inserted] = pending.try_emplace(a_list);
				auto& tracked = it->second;
				if (inserted || tracked.fence != a_fence) {
					a_fence->AddRef();
					SafeRelease(tracked.fence);
					tracked.fence = a_fence;
				}
				tracked.serial = (std::max)(tracked.serial, a_serial);
			}

			bool HasPending()
			{
				std::scoped_lock lock(mutex);
				return !pending.empty();
			}

			void Release()
			{
				std::scoped_lock lock(mutex);
				for (auto& [list, tracked] : pending) {
					(void)list;
					SafeRelease(tracked.fence);
				}
				pending.clear();
			}

			void OnCommandListsExecuted(ID3D12CommandQueue* a_queue, UINT a_count,
				ID3D12CommandList* const* a_lists)
			{
				if (!a_queue || !a_lists) {
					return;
				}
				std::vector<PendingConsume> completed;
				{
					std::scoped_lock lock(mutex);
					for (UINT i = 0; i < a_count; ++i) {
						const auto it = pending.find(a_lists[i]);
						if (it != pending.end()) {
							completed.push_back(it->second);
							pending.erase(it);
						}
					}
				}

				// Coalesce this submitted batch by fence so signal values never move backwards.
				std::unordered_map<ID3D12Fence*, std::uint64_t> signals;
				for (const auto& tracked : completed) {
					auto& value = signals[tracked.fence];
					value = (std::max)(value, tracked.serial);
				}
				for (const auto& [fence, serial] : signals) {
					const auto hr = a_queue->Signal(fence, serial);
					if (FAILED(hr) && !signalFailureLogged.exchange(true, std::memory_order_relaxed)) {
						REX::ERROR("D3D12Compositor: queue-ordered consume-fence signal failed "
							       "(hr=0x{:08X}); the browser host will drop frames instead of reusing a busy slot",
							static_cast<std::uint32_t>(hr));
					}
				}
				for (auto& tracked : completed) {
					SafeRelease(tracked.fence);
				}
			}

		private:
			struct PendingConsume
			{
				ID3D12Fence* fence{ nullptr };
				std::uint64_t serial{ 0 };
			};

			std::mutex mutex;
			std::unordered_map<ID3D12CommandList*, PendingConsume> pending;
			std::atomic_bool signalFailureLogged{ false };
		};
	}

	struct D3D12Compositor::Impl
	{
		EngineD3D12   engine{};
		std::atomic_bool visible{ false };

		OutputSizeObservation outputSize;

		bool setupAttempted{ false };
		bool setupOk{ false };

		DrawResources draw;
		SharedRingState sharedRing;
		ConsumeTracker consumes;

		std::atomic_bool overlayDrawLogged{ false };
		std::atomic_bool overlayDrawFgTargetLogged{ false };
		bool noSharedFrameLogged{ false };  // SharedRingState::drawMutex

		std::atomic_bool frameGenActiveSignal{ false };
		std::atomic_bool regionSawFgTarget{ false };
		std::atomic_bool fgClassificationKnown{ false };
		std::atomic_bool fgLayerOnlyLogged{ false };

		~Impl()
		{
			g_queueExecutedFn.store(nullptr, std::memory_order_release);
			g_overlayDrawFn.store(nullptr, std::memory_order_release);
			g_overlay.store(nullptr, std::memory_order_release);
			const bool gpuIdle = draw.WaitForGpuIdle(engine.directQueue);
			const bool hasUnsubmittedDraws = consumes.HasPending();
			if (gpuIdle && !hasUnsubmittedDraws) {
				sharedRing.Release();
			} else {
				REX::ERROR("D3D12Compositor: retaining GPU objects during shutdown because recorded overlay work could not be proven idle");
			}
			consumes.Release();
			sharedRing.ClosePending();
			if (gpuIdle && !hasUnsubmittedDraws) {
				draw.Release();
				SafeRelease(engine.directQueue);
				SafeRelease(engine.device);
			}
		}

		[[nodiscard]] CompositorStatus GetStatus() const
		{
			return {
				.frameGeneration = frameGenActiveSignal.load(std::memory_order_relaxed),
			};
		}

		static void QueueExecutedThunk(
			ID3D12CommandQueue* a_queue,
			const UINT a_count,
			ID3D12CommandList* const* a_lists)
		{
			auto* self = static_cast<Impl*>(g_overlay.load(std::memory_order_acquire));
			if (self) {
				self->consumes.OnCommandListsExecuted(a_queue, a_count, a_lists);
			}
		}

		[[nodiscard]] bool InstallQueueHook()
		{
			auto** vtable = *reinterpret_cast<void***>(engine.directQueue);
			auto** slot = &vtable[kExecuteCommandListsSlot];
			const auto current = reinterpret_cast<ExecuteCommandListsFn>(*slot);
			if (current == &ExecuteCommandListsThunk) {
				return g_origExecuteCommandLists.load(std::memory_order_acquire) != nullptr;
			}
			if (g_origExecuteCommandLists.load(std::memory_order_acquire) != nullptr) {
				REX::ERROR("D3D12Compositor: ExecuteCommandLists hook state conflicts with "
						   "the engine queue; overlay disabled");
				return false;
			}

			g_origExecuteCommandLists.store(current, std::memory_order_release);
			if (!REL::WriteSafeData(slot, reinterpret_cast<void*>(&ExecuteCommandListsThunk))) {
				if (*slot != reinterpret_cast<void*>(&ExecuteCommandListsThunk)) {
					g_origExecuteCommandLists.store(nullptr, std::memory_order_release);
				}
				REX::ERROR("D3D12Compositor: could not write the command-queue vtable slot; overlay disabled");
				return false;
			}
			return true;
		}

		// Submit/tick thread: adopt the latest announced ring. Returns true when a
		// usable ring is open.
		[[nodiscard]] bool EnsureSharedRing()
		{
			if (!sharedRing.pendingDirty) {
				std::scoped_lock ring(sharedRing.drawMutex);
				return sharedRing.slots[0] != nullptr;
			}
			auto pending = sharedRing.TakePending();
			// Hold the draw lock across the pending-list check and queue drain.
			// That closes the window where a render worker could record another
			// old-ring draw after the idle fence had already been queued.
			std::scoped_lock ring(sharedRing.drawMutex);
			const auto deferAdoption = [&]() {
				sharedRing.DeferPending(pending);
				return sharedRing.slots[0] != nullptr;
			};
			if (consumes.HasPending()) {
				return deferAdoption();
			}
			// Old slots may still be referenced by in-flight ENGINE lists
			// carrying overlay draws. A failed drain must keep this generation
			// alive; it is never treated as a successful timeout.
			if (!draw.WaitForGpuIdle(engine.directQueue)) {
				return deferAdoption();
			}
			sharedRing.Retire();

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
						__uuidof(ID3D12Resource), reinterpret_cast<void**>(&sharedRing.slots[i]));
					ok = SUCCEEDED(openHr);
				}
			}
			if (ok) {
				openObject = "produce fence";
				openSlot = -1;
				openHr = pending.produceFence ? dev->OpenSharedHandle(pending.produceFence,
					__uuidof(ID3D12Fence), reinterpret_cast<void**>(&sharedRing.produceFence)) : E_HANDLE;
				ok = SUCCEEDED(openHr);
			}
			if (ok) {
				openObject = "consume fence";
				openHr = pending.consumeFence ? dev->OpenSharedHandle(pending.consumeFence,
					__uuidof(ID3D12Fence), reinterpret_cast<void**>(&sharedRing.consumeFence)) : E_HANDLE;
				ok = SUCCEEDED(openHr);
			}
			SharedRingState::CloseHandles(pending);
			if (!ok) {
				const auto gameLuid = dev->GetAdapterLuid();
				REX::ERROR("D3D12Compositor: OpenSharedHandle failed for {} (slot {}, hr=0x{:08X}); "
					"game adapter LUID 0x{:08X}:0x{:08X}, browser-host adapter LUID 0x{:08X}:0x{:08X} — "
					"GPU frames from the browser host cannot be composited",
					openObject, openSlot, static_cast<std::uint32_t>(openHr),
					static_cast<std::uint32_t>(gameLuid.HighPart), gameLuid.LowPart,
					pending.adapterLuidHigh, pending.adapterLuidLow);
				sharedRing.Release();
				return false;
			}
			sharedRing.slotCount = pending.slotCount;
			for (std::uint32_t i = 0; i < sharedRing.slotCount; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				const D3D12_CPU_DESCRIPTOR_HANDLE handle{
					draw.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
					static_cast<SIZE_T>(i) * draw.srvStride
				};
				dev->CreateShaderResourceView(sharedRing.slots[i], &srv, handle);
			}
			REX::INFO("D3D12Compositor: shared ring adopted ({}x{}, {} slots, generation {})",
				pending.width, pending.height, sharedRing.slotCount, pending.generation);
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

			if (!draw.Create(engine.device)) {
				REX::ERROR("D3D12Compositor: setup failed; overlay disabled this session");
				return;
			}

			g_overlay.store(this, std::memory_order_release);
			g_queueExecutedFn.store(&Impl::QueueExecutedThunk, std::memory_order_release);
			if (!InstallQueueHook()) {
				g_queueExecutedFn.store(nullptr, std::memory_order_release);
				g_overlay.store(nullptr, std::memory_order_release);
				REX::ERROR("D3D12Compositor: setup failed; overlay disabled this session");
				return;
			}
			g_overlayDrawFn.store(&Impl::OverlayDrawThunk, std::memory_order_release);
			setupOk = true;
			REX::INFO("D3D12Compositor: UI-pass overlay armed (no IDXGISwapChain::Present hook)");
		}

		void ObserveOutputSize(const D3D12_RESOURCE_DESC& a_desc)
		{
			const auto width = static_cast<std::uint32_t>((std::min)(a_desc.Width,
				static_cast<std::uint64_t>(UINT32_MAX)));
			outputSize.Publish(width, a_desc.Height);
		}

		// Overlay draw (UiPass, render worker, inside the engine's UI-buffer
		// hand-off): record the overlay quad onto the ENGINE's list into the
		// engine's UI buffer — upstream of Frame Generation, so both real and
		// generated frames carry it. v1 transport: the produce fence is checked
		// on the CPU (we cannot wait on a queue we don't control). The command
		// queue hook signals consumption only after the exact engine command list
		// containing this draw has been submitted.
		[[nodiscard]] bool RecordOverlay(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			if (!setupOk || !draw.rtvHeap || !a_buffer) {
				return false;
			}

			const auto desc = a_buffer->GetDesc();
			ObserveOutputSize(desc);

			bool classificationKnown = fgClassificationKnown.load(std::memory_order_acquire);
			if (a_regionFirst) {
				const bool previousRegionHadFgTarget =
					regionSawFgTarget.exchange(false, std::memory_order_acq_rel);
				frameGenActiveSignal.store(previousRegionHadFgTarget, std::memory_order_release);
				classificationKnown = fgClassificationKnown.exchange(true, std::memory_order_acq_rel);
			}
			if (a_fgTarget) {
				regionSawFgTarget.store(true, std::memory_order_release);
				frameGenActiveSignal.store(true, std::memory_order_release);
			}

			if (!visible.load(std::memory_order_relaxed)) {
				return false;
			}
			// Until the first complete render region establishes whether COPY_SOURCE
			// exists, delay the ordinary target rather than risk blending twice.
			if (!classificationKnown && !a_fgTarget) {
				return false;
			}

			// In the FG graph the RT->pixel-SRV candidate is the already-opaque
			// scene/composite image. Drawing there puts the overlay into the frame
			// interpolation input, then FFX composites the transparent UI layer on
			// top a second time. Opaque pixels hide that duplication; translucent
			// pixels alternate between one and two blends. The COPY_SOURCE target
			// is the actual transparent UI layer and later feeds both paths.
			const bool fgActive = frameGenActiveSignal.load(std::memory_order_acquire);
			if (fgActive && !a_fgTarget) {
				if (!fgLayerOnlyLogged.exchange(true, std::memory_order_relaxed)) {
					REX::DEBUG("D3D12Compositor: under FG only the transparent COPY_SOURCE UI layer is drawn");
				}
				return false;
			}
			const auto frame = sharedRing.SnapshotFrame();
			auto ringSlot = frame.slot;
			auto serial = frame.serial;

			std::scoped_lock ring(sharedRing.drawMutex);
			if (!frame.ready) {
				// Normally a brief startup transient: the overlay can be revealed
				// on the frame the browser host publishes its first shared slot.
				if (!noSharedFrameLogged) {
					noSharedFrameLogged = true;
					REX::DEBUG("D3D12Compositor: UI-pass hand-off reached before the browser host "
							   "published a shared-ring frame; nothing to draw yet");
				}
				return false;
			}
			// Promote the newest frame to "ready" once its produce fence has
			// completed; an incomplete newest frame falls back to the last
			// ready one (see readySlot) instead of skipping the draw.
			// Under FG the preceding opaque target was deliberately skipped, so
			// the transparent target becomes the effective first draw.
			const bool effectiveRegionFirst = a_regionFirst || (fgActive && a_fgTarget);
			if (effectiveRegionFirst &&
				serial != 0 && ringSlot < sharedRing.slotCount && sharedRing.slots[ringSlot] &&
				(!sharedRing.produceFence || sharedRing.produceFence->GetCompletedValue() >= serial)) {
				sharedRing.readySlot = ringSlot;
				sharedRing.readySerial = serial;
			}
			if (sharedRing.readySerial == 0 || sharedRing.readySlot >= sharedRing.slotCount ||
				!sharedRing.slots[sharedRing.readySlot]) {
				return false;  // no fully-produced frame yet this ring generation
			}
			ringSlot = sharedRing.readySlot;
			serial = sharedRing.readySerial;

			const auto rtvFormat = UiTargetFormat::ResolveRtv(desc.Format);
			auto* pso = draw.EnsurePipeline(engine.device, rtvFormat);
			if (!pso) {
				return false;
			}

			const D3D12_CPU_DESCRIPTOR_HANDLE rtv = draw.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = rtvFormat;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			engine.device->CreateRenderTargetView(a_buffer, &rtvDesc, rtv);

			ID3D12DescriptorHeap* heaps[]{ draw.srvHeap };
			a_list->SetDescriptorHeaps(1, heaps);
			a_list->SetGraphicsRootSignature(draw.rootSignature);
			const D3D12_GPU_DESCRIPTOR_HANDLE srv{
				draw.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
				static_cast<UINT64>(ringSlot) * draw.srvStride
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

			consumes.Track(a_list, sharedRing.consumeFence, serial);
			const bool firstDraw = !overlayDrawLogged.exchange(true, std::memory_order_relaxed);
			const bool firstFgDraw = a_fgTarget &&
				!overlayDrawFgTargetLogged.exchange(true, std::memory_order_relaxed);
			// One-time log per target kind: if the FG line never appears, its
			// hand-off is not being matched.
			if (firstDraw || firstFgDraw) {
				REX::INFO("D3D12Compositor: FIRST UI-PASS OVERLAY DRAW [{}] (ring slot {} serial {} -> {}x{} {} UI buffer 0x{:X})",
					a_fgTarget ? "premultiplied / FG UI input" : "premultiplied / composite input",
					ringSlot, serial, static_cast<std::uint64_t>(desc.Width), desc.Height,
					UiTargetFormat::Name(rtvFormat),
					reinterpret_cast<std::uintptr_t>(a_buffer));
			}
			return true;
		}

		static bool OverlayDrawThunk(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			auto* self = static_cast<Impl*>(g_overlay.load(std::memory_order_acquire));
			return self && self->RecordOverlay(a_list, a_buffer, a_fgTarget, a_regionFirst);
		}

	};

	D3D12Compositor::D3D12Compositor() = default;
	D3D12Compositor::~D3D12Compositor() = default;

	bool D3D12Compositor::Initialize()
	{
		_impl = std::make_unique<Impl>();
		REX::INFO("D3D12Compositor: initialized (UI-pass-only overlay; engine device/queue are set up "
				  "on the first submitted frame)");
		return true;
	}


	void D3D12Compositor::Submit(const FrameBufferView& a_frame)
	{
		if (!_impl) {
			return;
		}
		_impl->sharedRing.CacheFrame(a_frame);
		_impl->EnsureSetup();
		if (_impl->setupOk) {
			(void)_impl->EnsureSharedRing();
		}
	}

	bool RecordOverlayIntoUIBuffer(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
		const bool a_fgTarget, const bool a_regionFirst)
	{
		const auto fn = g_overlayDrawFn.load(std::memory_order_acquire);
		return fn && a_list && a_buffer && fn(a_list, a_buffer, a_fgTarget, a_regionFirst);
	}

	void D3D12Compositor::SetSharedRing(const SharedRingDesc& a_desc)
	{
		if (_impl) {
			_impl->sharedRing.SetPending(a_desc);
		} else {
			// Not initialized: still own the handles — close them.
			SharedRingDesc desc = a_desc;
			SharedRingState::CloseHandles(desc);
		}
	}

	void D3D12Compositor::SetVisible(bool a_visible)
	{
		if (_impl) {
			_impl->visible.store(a_visible, std::memory_order_relaxed);
		}
	}

	CompositorStatus D3D12Compositor::GetStatus() const
	{
		if (_impl) {
			return _impl->GetStatus();
		}
		return {};
	}

	std::optional<OutputSize> D3D12Compositor::GetObservedOutputSize() const
	{
		return _impl ? _impl->outputSize.Snapshot() : std::nullopt;
	}
}
