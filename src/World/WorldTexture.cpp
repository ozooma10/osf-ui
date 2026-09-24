#include "World/WorldTexture.h"
#include "World/EngineTextureIdentity.h"
#include "World/WorldAssets.h"

#include "Composite/D3D12Prologue.h"
#include "Composite/EngineD3D12.h"
#include "Core/Ids.h"
#include "Core/Log.h"
#include "REL/Utility.h"
#include "Win32Util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>

namespace OSFUI::WorldTexture
{
	namespace
	{
		using osfui::win32::SafeRelease;
		using CreateSrvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
		constexpr std::size_t kCreateSrvSlot = 18;
		constexpr std::size_t kExecuteSlot = 10;
		constexpr std::size_t kCopyContexts = 3;
		constexpr std::size_t kNoSurface = (std::numeric_limits<std::size_t>::max)();
		constexpr auto        kShaderState = static_cast<D3D12_RESOURCE_STATES>(
			static_cast<unsigned>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | static_cast<unsigned>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		constexpr auto           kFenceFailure = (std::numeric_limits<std::uint64_t>::max)();
		std::atomic<CreateSrvFn> g_createSrv{ nullptr };
		std::atomic<ExecuteFn>   g_execute{ nullptr };

		struct OwnedHandles
		{
			SharedRingDesc desc;
			~OwnedHandles()
			{
				for (auto handle : desc.slotHandles) {
					if (handle)
						::CloseHandle(handle);
				}
				if (desc.produceFence)
					::CloseHandle(desc.produceFence);
				for (auto handle : desc.consumeFences) {
					if (handle)
						::CloseHandle(handle);
				}
			}
		};

		struct Ring
		{
			std::array<ID3D12Resource*, SharedRingDesc::kMaxSlots> slots{};
			std::array<ID3D12Fence*, SharedRingDesc::kMaxSlots>    consume{};
			ID3D12Fence*                                           produce{ nullptr };
			std::uint32_t                                          count{ 0 };
			std::uint64_t                                          generation{ 0 };
			std::array<std::uint64_t, SharedRingDesc::kMaxSlots>   pending{};
			std::array<std::uint64_t, SharedRingDesc::kMaxSlots>   signaled{};

			~Ring()
			{
				for (auto*& slot : slots) SafeRelease(slot);
				for (auto*& fence : consume) SafeRelease(fence);
				SafeRelease(produce);
			}
		};

		struct MemoryAccounting
		{
			std::atomic<std::uint64_t> resident{ 0 };
			std::atomic<std::uint64_t> retired{ 0 };
		};

		struct Output
		{
			ID3D12Resource*            texture{ nullptr };
			MemoryAccounting*          accounting{ nullptr };
			std::uint64_t              bytes{ 0 };
			std::uint64_t              generation{ 0 };
			std::uint64_t              retirementFence{ 0 };
			std::atomic<std::uint32_t> engineOwners{ 0 };
			~Output()
			{
				SafeRelease(texture);
				if (accounting) {
					accounting->resident.fetch_sub(bytes);
					accounting->retired.fetch_add(1);
				}
			}
		};

		// Engine resource -> lease -> output. There is intentionally no pointer
		// back to the engine resource: this does not pin engine streaming or
		// form a reference cycle. Submitted copies independently pin Output.
		constexpr GUID kOutputLeaseGuid{ 0x0d52d6ed, 0x9a34, 0x4978, { 0x82, 0x73, 0x14, 0x7a, 0x5e, 0x2b, 0xb9, 0xf1 } };
		class OutputLease final : public IUnknown
		{
		public:
			OutputLease(std::shared_ptr<Output> value, WorldAssets::Key key, std::size_t index) : output(std::move(value)), asset(key), surface(index)
			{
				output->engineOwners.fetch_add(1);
				output->retirementFence = 0;
			}
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
			{
				if (!result)
					return E_POINTER;
				*result = nullptr;
				if (iid != __uuidof(IUnknown))
					return E_NOINTERFACE;
				*result = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const auto remaining = --references;
				if (!remaining) {
					const auto owners = output->engineOwners.fetch_sub(1) - 1;
					REX::INFO("WorldTexture: asset {:08X}:{:08X}:{:08X} engine resource retired (output {}, owners {})",
						asset.file, asset.extension, asset.directory, output->generation, owners);
					delete this;
				}
				return remaining;
			}
			std::shared_ptr<Output> output;
			WorldAssets::Key        asset;
			std::size_t             surface;

		private:
			std::atomic<ULONG> references{ 1 };
		};

		struct Surface
		{
			SurfaceStats            stats;
			std::uint64_t           lastCompletion{ 0 };
			std::shared_ptr<Output> output;
			WorldAssets::Key        asset;
			bool                    evictRequested{ false };
			std::uint64_t           lastUse{ 0 };
			std::shared_ptr<Ring>   ring;
		};

		struct CopyContext
		{
			ID3D12CommandAllocator*    allocator{ nullptr };
			ID3D12GraphicsCommandList* list{ nullptr };
			std::uint64_t              completion{ 0 };
			std::size_t                surface{ kNoSurface };
			std::uint64_t              serial{ 0 };
			std::shared_ptr<Output>    output;
			// Only our command lists reference ring resources. These references
			// retire with our fence, independently of engine material lifetimes.
			std::shared_ptr<Ring> ring;
		};

		struct Initialization
		{
			ID3D12CommandAllocator*    allocator{ nullptr };
			ID3D12GraphicsCommandList* list{ nullptr };
			ID3D12Resource*            upload{ nullptr };
			std::shared_ptr<Output>    output;
			std::uint64_t              completion{ kFenceFailure };
			~Initialization()
			{
				SafeRelease(list);
				SafeRelease(allocator);
				SafeRelease(upload);
			}
		};

		struct State
		{
			std::mutex                                   mutex;
			std::mutex                                   queueMutex;
			std::vector<Surface>                         surfaces;
			MemoryAccounting                             memory;
			std::uint64_t                                budgetBytes{ kDefaultBudgetBytes };
			std::uint64_t                                nextOutput{ 1 };
			std::uint64_t                                allocationDeferrals{ 0 };
			std::uint64_t                                clock{ 0 };
			std::size_t                                  count{ 0 };
			std::size_t                                  nextSurface{ 0 };
			EngineD3D12                                  engine;
			ID3D12Fence*                                 completionFence{ nullptr };
			std::array<CopyContext, kCopyContexts>       copies;
			std::vector<std::unique_ptr<Initialization>> initializations;
			std::uint64_t                                nextCompletion{ 1 };
			bool                                         hooksAttempted{ false };
			std::atomic<bool>                            enabled{ false };
			std::atomic<bool>                            installed{ false };
			bool                                         gpuFailed{ false };
		};

		State& GetState()
		{
			// Engine descriptors and hooks may be used during teardown. Reclaiming
			// these resources at DLL static destruction would invalidate them.
			static auto* state = new State;
			return *state;
		}

		bool PlainTexture(const D3D12_RESOURCE_DESC& a_desc)
		{
			return a_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
			       a_desc.DepthOrArraySize == 1 && a_desc.MipLevels == 1 &&
			       a_desc.SampleDesc.Count == 1 && a_desc.SampleDesc.Quality == 0;
		}

		bool Placeholder(const D3D12_RESOURCE_DESC& a_desc)
		{
			return PlainTexture(a_desc) && a_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
			       a_desc.Flags == D3D12_RESOURCE_FLAG_NONE && a_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN;
		}

		bool PlainSrv(const D3D12_SHADER_RESOURCE_VIEW_DESC* a_desc)
		{
			return a_desc && a_desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
			       (a_desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM || a_desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
			       a_desc->Texture2D.MostDetailedMip == 0 &&
			       (a_desc->Texture2D.MipLevels == 1 || a_desc->Texture2D.MipLevels == UINT_MAX) &&
			       a_desc->Texture2D.PlaneSlice == 0 && a_desc->Texture2D.ResourceMinLODClamp == 0.0f;
		}

		std::shared_ptr<Output> AllocateOutput(State& state, std::size_t index);

		void STDMETHODCALLTYPE CreateSrvThunk(ID3D12Device* device, ID3D12Resource* resource,
			const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
		{
			const auto original = g_createSrv.load(std::memory_order_acquire);
			if (!original)
				return;
			auto& state = GetState();
			if (!resource || !PlainSrv(desc) || device != state.engine.device || !state.installed) {
				original(device, resource, desc, handle);
				return;
			}
			// Both locks follow the queue -> state order used by ExecuteThunk.
			// Initialization is ordered before any descriptor can expose Output.
			std::scoped_lock queueLock(state.queueMutex);
			std::scoped_lock lock(state.mutex);
			OutputLease*     lease = nullptr;
			UINT             bytes = sizeof(lease);
			resource->GetPrivateData(kOutputLeaseGuid, &bytes, &lease);
			const auto asset = EngineTextureIdentity::CurrentAsset();
			if (lease && asset && lease->asset != *asset) {
				REX::ERROR("WorldTexture: resource reused for a different asset; refusing conflicting binding");
				lease->Release();
				original(device, resource, desc, handle);
				return;
			}
			if (!lease && asset && !state.gpuFailed && Placeholder(resource->GetDesc())) {
				for (std::size_t i = 0; i < state.count; ++i) {
					auto& surface = state.surfaces[i];
					if (surface.asset != *asset)
						continue;
					if (!surface.output)
						surface.output = AllocateOutput(state, i);
					if (surface.output) {
						lease = new OutputLease(surface.output, *asset, i);
						const auto hr = resource->SetPrivateDataInterface(kOutputLeaseGuid, lease);
						if (FAILED(hr)) {
							lease->Release();
							lease = nullptr;
						}
					}
					break;
				}
			}
			if (lease) {
				auto replacement = *desc;
				replacement.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
				replacement.Texture2D.MipLevels = 1;
				auto& surface = state.surfaces[lease->surface];
				surface.lastUse = ++state.clock;
				original(device, lease->output->texture, &replacement, handle);
				++surface.stats.boundDescriptors;
				REX::INFO("WorldTexture: '{}' asset '{}' bound to output {} (engine owners {})",
					surface.stats.id, surface.stats.texture, lease->output->generation, lease->output->engineOwners.load());
				lease->Release();
				return;
			}
			original(device, resource, desc, handle);
		}

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before,
			D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		void GpuFailure(State& a_state, const char* a_operation, HRESULT a_hr)
		{
			if (!a_state.gpuFailed) {
				REX::ERROR("WorldTexture: {} failed (0x{:08X}); GPU copies disabled, resources retained", a_operation, static_cast<std::uint32_t>(a_hr));
			}
			a_state.gpuFailed = true;
		}

		void DiscardRing(Surface& a_surface)
		{
			// In-flight contexts keep their references until our local fence ends
			// those reads. The material still owns the stable destination texture.
			a_surface.ring.reset();
			a_surface.stats.ringGeneration = 0;
			a_surface.stats.lastCopiedFrame = 0;
			a_surface.stats.lastCompletedFrame = 0;
		}

		bool ProducerFailure(State& a_state, Surface& a_surface, const char* a_operation, HRESULT a_hr)
		{
			// A fence created by the browser process can become UINT64_MAX when
			// that producer exits. It does not establish loss of the game device.
			const auto gameReason = a_state.engine.device->GetDeviceRemovedReason();
			if (FAILED(gameReason)) {
				GpuFailure(a_state, a_operation, gameReason);
				return false;
			}
			REX::WARN("WorldTexture: '{}' producer disconnected during {} (0x{:08X}, generation {}); game device healthy, retaining last image until a new ring arrives",
				a_surface.stats.id, a_operation, static_cast<std::uint32_t>(a_hr), a_surface.stats.ringGeneration);
			++a_surface.stats.producerDisconnects;
			DiscardRing(a_surface);
			return true;
		}

		// Run only with queueMutex and mutex held. After the final engine owner
		// disappears, order a fence after prior DIRECT submissions as well as
		// our copies. Snapshot alone must never make an output GPU-retireable.
		bool QueueRetirements(State& state)
		{
			if (state.gpuFailed)
				return false;
			for (auto& surface : state.surfaces) {
				auto& output = surface.output;
				if (!output || output->engineOwners.load() || output->retirementFence)
					continue;
				const auto completion = state.nextCompletion++;
				const auto hr = state.engine.directQueue->Signal(state.completionFence, completion);
				if (FAILED(hr)) {
					GpuFailure(state, "engine retirement signal", hr);
					return false;
				}
				output->retirementFence = completion;
				REX::INFO("WorldTexture: '{}' output {} queued retirement fence {} after final engine owner", surface.stats.id, output->generation, completion);
			}
			return true;
		}

		bool SafeToRetire(const std::shared_ptr<Output>& output, std::uint64_t completed)
		{
			return output && output->engineOwners.load() == 0 && output.use_count() == 1 &&
			       output->retirementFence != 0 && output->retirementFence <= completed;
		}

		bool RetireCopies(State& a_state)
		{
			if (!a_state.completionFence || a_state.gpuFailed)
				return false;
			const auto completed = a_state.completionFence->GetCompletedValue();
			if (completed == kFenceFailure) {
				const auto reason = a_state.engine.device->GetDeviceRemovedReason();
				GpuFailure(a_state, "local copy completion fence", FAILED(reason) ? reason : E_FAIL);
				return false;
			}
			for (auto& copy : a_state.copies) {
				if (copy.completion && copy.completion <= completed) {
					if (copy.surface < a_state.count) {
						auto& surface = a_state.surfaces[copy.surface];
						++surface.stats.completedFrames;
						if (copy.ring->generation == surface.stats.ringGeneration && copy.output == surface.output && copy.completion > surface.lastCompletion) {
							surface.lastCompletion = copy.completion;
							surface.stats.lastCompletedFrame = copy.serial;
						}
					}
					copy.ring.reset();
					copy.output.reset();
					copy.completion = 0;
				}
			}
			std::erase_if(a_state.initializations, [&](const auto& init) { return init->completion <= completed; });
			for (auto& surface : a_state.surfaces) {
				if (surface.evictRequested && SafeToRetire(surface.output, completed)) {
					REX::INFO("WorldTexture: '{}' evicted output {} after engine and GPU retirement", surface.stats.id, surface.output->generation);
					surface.output.reset();
					surface.evictRequested = false;
					surface.stats.lastCompletedFrame = 0;
				}
			}
			return true;
		}

		// queueMutex serializes the copy and engine ExecuteCommandLists calls.
		// Stable textures are shader-readable before and after every submission;
		// the engine never sees our temporary COPY_DEST state.
		void CopyPending(State& a_state, ExecuteFn a_execute)
		{
			std::scoped_lock lock(a_state.mutex);
			if (!a_state.installed || a_state.gpuFailed)
				return;
			if (!QueueRetirements(a_state) || !RetireCopies(a_state))
				return;
			const auto first = a_state.nextSurface;
			for (std::size_t offset = 0; offset < a_state.count; ++offset) {
				const auto i = (first + offset) % a_state.count;
				auto&      surface = a_state.surfaces[i];
				if (!surface.ring || !surface.output || (surface.evictRequested && surface.output->engineOwners.load() == 0))
					continue;
				auto&      ring = *surface.ring;
				const auto produced = ring.produce->GetCompletedValue();
				if (produced == kFenceFailure) {
					if (!ProducerFailure(a_state, surface, "producer completion", DXGI_ERROR_DEVICE_REMOVED))
						return;
					continue;
				}
				std::uint32_t selected = ring.count;
				std::uint64_t serial = 0;
				for (std::uint32_t slot = 0; slot < ring.count; ++slot) {
					if (ring.pending[slot] > ring.signaled[slot] && ring.pending[slot] <= produced && ring.pending[slot] > serial) {
						selected = slot;
						serial = ring.pending[slot];
					}
				}
				if (selected == ring.count)
					continue;
				auto available = std::ranges::find_if(a_state.copies, [](const CopyContext& a_copy) { return a_copy.completion == 0; });
				if (available == a_state.copies.end()) {
					++surface.stats.skippedBusy;
					continue;
				}
				auto& copy = *available;
				auto  hr = copy.allocator->Reset();
				if (SUCCEEDED(hr))
					hr = copy.list->Reset(copy.allocator, nullptr);
				if (FAILED(hr)) {
					GpuFailure(a_state, "copy allocator/list reset", hr);
					return;
				}
				D3D12_RESOURCE_BARRIER before[]{
					Transition(surface.output->texture, kShaderState, D3D12_RESOURCE_STATE_COPY_DEST),
					Transition(ring.slots[selected], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)
				};
				copy.list->ResourceBarrier(2, before);
				copy.list->CopyResource(surface.output->texture, ring.slots[selected]);
				D3D12_RESOURCE_BARRIER after[]{
					Transition(surface.output->texture, D3D12_RESOURCE_STATE_COPY_DEST, kShaderState),
					Transition(ring.slots[selected], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				copy.list->ResourceBarrier(2, after);
				hr = copy.list->Close();
				if (FAILED(hr)) {
					GpuFailure(a_state, "copy list close", hr);
					return;
				}
				copy.ring = surface.ring;
				copy.output = surface.output;
				surface.lastUse = ++a_state.clock;
				copy.surface = i;
				copy.serial = serial;
				// Only failure of our local completion signal pins this context. A
				// dead producer's consume fence must not disable the copy pool.
				copy.completion = kFenceFailure;
				ID3D12CommandList* lists[]{ copy.list };
				a_execute(a_state.engine.directQueue, 1, lists);
				HRESULT consumeResult = S_OK;
				for (std::uint32_t slot = 0; slot < ring.count; ++slot) {
					const auto consumed = ring.pending[slot];
					if (consumed <= ring.signaled[slot] || consumed > serial)
						continue;
					// Earlier unselected frames can now be dropped too. Each slot
					// has its own fence; none is released ahead of a submitted read.
					hr = a_state.engine.directQueue->Signal(ring.consume[slot], consumed);
					if (FAILED(hr)) {
						consumeResult = hr;
						break;
					}
					ring.signaled[slot] = consumed;
				}
				const auto completion = a_state.nextCompletion++;
				hr = a_state.engine.directQueue->Signal(a_state.completionFence, completion);
				if (FAILED(hr)) {
					GpuFailure(a_state, "copy completion signal", hr);
					return;
				}
				copy.completion = completion;
				a_state.nextSurface = (i + 1) % a_state.count;
				surface.stats.lastCopiedFrame = serial;
				if (++surface.stats.copiedFrames == 1) {
					REX::INFO("WorldTexture: '{}' first GPU copy (generation {}, frame {}, slot {})", surface.stats.id, ring.generation, serial, selected);
				}
				// Always queue our own completion after the submitted copy, even
				// if the remote process died between the readiness check and Signal.
				if (FAILED(consumeResult) && !ProducerFailure(a_state, surface, "consume signal", consumeResult))
					return;
			}
		}

		void STDMETHODCALLTYPE ExecuteThunk(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			const auto original = g_execute.load(std::memory_order_acquire);
			if (!original)
				return;
			auto& state = GetState();
			if (a_queue != state.engine.directQueue) {
				original(a_queue, a_count, a_lists);
				return;
			}
			std::scoped_lock queueLock(state.queueMutex);
			CopyPending(state, original);
			original(a_queue, a_count, a_lists);
		}

		void ReleaseUninstalled(State& a_state)
		{
			for (auto& surface : a_state.surfaces) surface.output.reset();
			for (auto& copy : a_state.copies) {
				copy.output.reset();
				SafeRelease(copy.list);
				SafeRelease(copy.allocator);
			}
			SafeRelease(a_state.completionFence);
			SafeRelease(a_state.engine.directQueue);
			SafeRelease(a_state.engine.device);
		}

		bool CreateResources(State& state)
		{
			auto* device = state.engine.device;
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.completionFence))))
				return false;
			for (auto& copy : state.copies) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator))) ||
					FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator, nullptr, IID_PPV_ARGS(&copy.list))) ||
					FAILED(copy.list->Close()))
					return false;
			}
			return true;
		}

		std::shared_ptr<Output> AllocateOutput(State& state, std::size_t index)
		{
			auto& surface = state.surfaces[index];
			auto* device = state.engine.device;
			if (!QueueRetirements(state) || !RetireCopies(state))
				return {};
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = surface.stats.width;
			desc.Height = surface.stats.height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			desc.SampleDesc.Count = 1;
			const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
			if (allocation.SizeInBytes == kFenceFailure || allocation.SizeInBytes > state.budgetBytes) {
				++state.allocationDeferrals;
				++surface.stats.allocationDeferrals;
				REX::WARN("WorldTexture: '{}' output exceeds {} byte budget; material remains its placeholder", surface.stats.id, state.budgetBytes);
				return {};
			}
			while (state.memory.resident.load() > state.budgetBytes - allocation.SizeInBytes) {
				Surface* oldest = nullptr;
				for (auto& candidate : state.surfaces) {
					if (!SafeToRetire(candidate.output, state.completionFence->GetCompletedValue()))
						continue;
					if (!oldest || candidate.lastUse < oldest->lastUse)
						oldest = &candidate;
				}
				if (!oldest) {
					++state.allocationDeferrals;
					++surface.stats.allocationDeferrals;
					REX::WARN("WorldTexture: '{}' allocation deferred: {} / {} bytes resident, remaining outputs owned by engine/GPU; retry on asset reload",
						surface.stats.id, state.memory.resident.load(), state.budgetBytes);
					return {};
				}
				REX::INFO("WorldTexture: '{}' output {} evicted under memory pressure", oldest->stats.id, oldest->output->generation);
				oldest->output.reset();
				oldest->stats.lastCompletedFrame = 0;
			}
			auto                  output = std::make_shared<Output>();
			auto                  init = std::make_unique<Initialization>();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap.CreationNodeMask = 1;
			heap.VisibleNodeMask = 1;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr, IID_PPV_ARGS(&output->texture))))
				return {};
			output->bytes = allocation.SizeInBytes;
			output->generation = state.nextOutput++;
			output->accounting = &state.memory;
			state.memory.resident.fetch_add(output->bytes);
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT64                             uploadBytes = 0;
			device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
			D3D12_RESOURCE_DESC uploadDesc{};
			uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadDesc.Width = uploadBytes;
			uploadDesc.Height = 1;
			uploadDesc.DepthOrArraySize = 1;
			uploadDesc.MipLevels = 1;
			uploadDesc.SampleDesc.Count = 1;
			uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr, IID_PPV_ARGS(&init->upload))) ||
				FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&init->allocator))) ||
				FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, init->allocator, nullptr, IID_PPV_ARGS(&init->list))))
				return {};
			void*             mapped = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			if (FAILED(init->upload->Map(0, &noRead, &mapped)))
				return {};
			std::memset(mapped, 0, static_cast<std::size_t>(uploadBytes));
			for (std::uint32_t row = 0; row < surface.stats.height; ++row) {
				auto* pixels = reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch);
				std::fill_n(pixels, surface.stats.width, 0xff000000u);
			}
			init->upload->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = init->upload;
			source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			source.PlacedFootprint = footprint;
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = output->texture;
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			init->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			const auto barrier = Transition(output->texture, D3D12_RESOURCE_STATE_COPY_DEST, kShaderState);
			init->list->ResourceBarrier(1, &barrier);
			if (FAILED(init->list->Close()))
				return {};
			init->output = output;
			auto* pending = init.get();
			state.initializations.push_back(std::move(init));  // Pin before submission, including signal failure.
			ID3D12CommandList* lists[]{ pending->list };
			g_execute.load(std::memory_order_acquire)(state.engine.directQueue, 1, lists);
			const auto completion = state.nextCompletion++;
			const auto hr = state.engine.directQueue->Signal(state.completionFence, completion);
			if (FAILED(hr)) {
				GpuFailure(state, "output initialization signal", hr);
				return {};
			}
			pending->completion = completion;
			REX::INFO("WorldTexture: '{}' allocated output {} ({} bytes; {} / {} resident)",
				surface.stats.id, output->generation, output->bytes, state.memory.resident.load(), state.budgetBytes);
			return output;
		}

	}

	bool Configure(std::span<const SurfaceDesc> descriptions, std::uint64_t budgetBytes)
	{
		auto&            state = GetState();
		std::scoped_lock lock(state.mutex);
		if (state.hooksAttempted || budgetBytes == 0)
			return false;
		for (std::size_t i = 0; i < descriptions.size(); ++i) {
			const auto& desc = descriptions[i];
			if (desc.id.empty() || !WorldAssets::IsGeneratedPath(desc.texture) || desc.texture != WorldAssets::TexturePath(desc.id) ||
				desc.width == 0 || desc.height == 0 || desc.width > 4096 || desc.height > 4096) {
				REX::ERROR("WorldTexture: '{}' has an invalid or foreign generated texture binding", desc.id);
				return false;
			}
			for (std::size_t j = 0; j < i; ++j) {
				if (Ids::EqualsCaseInsensitiveAscii(descriptions[j].id, desc.id) || WorldAssets::KeyForPath(descriptions[j].texture) == WorldAssets::KeyForPath(desc.texture)) {
					REX::ERROR("WorldTexture: duplicate feed or asset claim: '{}' / '{}'", desc.id, descriptions[j].id);
					return false;
				}
			}
		}
		state.surfaces.resize(descriptions.size());
		state.count = descriptions.size();
		state.budgetBytes = budgetBytes;
		for (std::size_t i = 0; i < state.count; ++i) {
			const auto& desc = descriptions[i];
			state.surfaces[i].stats = { .id = desc.id, .texture = desc.texture, .width = desc.width, .height = desc.height };
			state.surfaces[i].asset = WorldAssets::KeyForPath(desc.texture);
		}
		state.enabled.store(state.count != 0, std::memory_order_release);
		return true;
	}

	void RequestEviction(std::size_t index)
	{
		auto&            state = GetState();
		std::scoped_lock lock(state.mutex);
		if (index < state.count)
			state.surfaces[index].evictRequested = true;
	}

	CacheStats SnapshotCache()
	{
		auto&            state = GetState();
		std::scoped_lock lock(state.mutex);
		return { state.budgetBytes, state.memory.resident.load(), state.memory.retired.load(), state.allocationDeferrals };
	}

	bool Install()
	{
		auto& state = GetState();
		if (state.installed.load(std::memory_order_acquire))
			return true;
		if (!state.enabled.load(std::memory_order_acquire))
			return false;
		std::scoped_lock queueLock(state.queueMutex);
		std::scoped_lock lock(state.mutex);
		if (state.installed)
			return true;
		if (state.hooksAttempted || state.count == 0)
			return false;
		state.engine = LocateEngineD3D12();
		if (!state.engine)
			return false;
		if (!CreateResources(state)) {
			REX::ERROR("WorldTexture: GPU resource creation failed");
			ReleaseUninstalled(state);
			state.hooksAttempted = true;
			return false;
		}
		// Publish the originals before patching: render threads can enter either
		// hook immediately. Output initialization is queued before each asset binding.
		state.hooksAttempted = true;

		if (!EngineTextureIdentity::Install())
			return false;
		auto** queueSlot = &(*reinterpret_cast<void***>(state.engine.directQueue))[kExecuteSlot];
		g_execute.store(reinterpret_cast<ExecuteFn>(*queueSlot), std::memory_order_release);
		if (!REL::WriteSafeData(queueSlot, reinterpret_cast<void*>(&ExecuteThunk)) && *queueSlot != reinterpret_cast<void*>(&ExecuteThunk)) {
			REX::ERROR("WorldTexture: ExecuteCommandLists hook installation failed");
			return false;
		}
		auto** deviceSlot = &(*reinterpret_cast<void***>(state.engine.device))[kCreateSrvSlot];
		g_createSrv.store(reinterpret_cast<CreateSrvFn>(*deviceSlot), std::memory_order_release);
		if (!REL::WriteSafeData(deviceSlot, reinterpret_cast<void*>(&CreateSrvThunk)) && *deviceSlot != reinterpret_cast<void*>(&CreateSrvThunk)) {
			REX::ERROR("WorldTexture: CreateShaderResourceView hook installation failed");
			return false;
		}
		state.installed = true;
		REX::INFO("WorldTexture: installed for {} surfaces (named material textures, {} GPU copy contexts)", state.count, kCopyContexts);
		return true;
	}

	bool IsInstalled()
	{
		return GetState().installed.load(std::memory_order_acquire);
	}

	void SetSharedRing(std::size_t a_surface, const SharedRingDesc& a_desc)
	{
		const OwnedHandles handles{ a_desc };
		auto&              state = GetState();
		std::scoped_lock   lock(state.mutex);
		if (a_surface >= state.count)
			return;
		auto& surface = state.surfaces[a_surface];
		DiscardRing(surface);
		if (a_desc.slotCount == 0)
			return;
		if (!state.installed || state.gpuFailed || a_desc.slotCount > SharedRingDesc::kMaxSlots ||
			a_desc.generation == 0 || a_desc.width != surface.stats.width || a_desc.height != surface.stats.height) {
			++surface.stats.ringOpenFailures;
			return;
		}
		auto    ring = std::make_shared<Ring>();
		auto*   device = state.engine.device;
		HRESULT hr = S_OK;
		for (std::uint32_t slot = 0; slot < a_desc.slotCount && SUCCEEDED(hr); ++slot) {
			hr = a_desc.slotHandles[slot] ? device->OpenSharedHandle(a_desc.slotHandles[slot], IID_PPV_ARGS(&ring->slots[slot])) : E_HANDLE;
			if (SUCCEEDED(hr)) {
				const auto desc = ring->slots[slot]->GetDesc();
				if (!PlainTexture(desc) || desc.Width != a_desc.width || desc.Height != a_desc.height ||
					(desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS))
					hr = E_INVALIDARG;
			}
			if (SUCCEEDED(hr))
				hr = a_desc.consumeFences[slot] ? device->OpenSharedHandle(a_desc.consumeFences[slot], IID_PPV_ARGS(&ring->consume[slot])) : E_HANDLE;
		}
		if (SUCCEEDED(hr))
			hr = a_desc.produceFence ? device->OpenSharedHandle(a_desc.produceFence, IID_PPV_ARGS(&ring->produce)) : E_HANDLE;
		if (FAILED(hr)) {
			++surface.stats.ringOpenFailures;
			REX::ERROR("WorldTexture: '{}' shared ring rejected (0x{:08X}, generation {})", surface.stats.id, static_cast<std::uint32_t>(hr), a_desc.generation);
			return;
		}
		ring->count = a_desc.slotCount;
		ring->generation = a_desc.generation;
		surface.ring = std::move(ring);
		surface.stats.ringGeneration = a_desc.generation;
		REX::INFO("WorldTexture: '{}' shared ring adopted ({}x{}, {} slots, generation {})", surface.stats.id, a_desc.width, a_desc.height, a_desc.slotCount, a_desc.generation);
	}

	void Submit(std::size_t a_surface, const FrameBufferView& a_frame)
	{
		auto&            state = GetState();
		std::scoped_lock lock(state.mutex);
		if (a_surface >= state.count)
			return;
		auto& surface = state.surfaces[a_surface];
		if (!surface.ring || a_frame.ringGeneration != surface.ring->generation || a_frame.frameIndex == 0 ||
			a_frame.sharedSlot >= surface.ring->count || a_frame.width != surface.stats.width || a_frame.height != surface.stats.height) {
			++surface.stats.rejectedFrames;
			return;
		}
		auto& pending = surface.ring->pending[a_frame.sharedSlot];
		if (a_frame.frameIndex <= pending)
			return;
		pending = a_frame.frameIndex;
		++surface.stats.submittedFrames;
	}

	std::vector<SurfaceStats> Snapshot()
	{
		auto&            state = GetState();
		std::scoped_lock lock(state.mutex);
		(void)RetireCopies(state);
		std::vector<SurfaceStats> result;
		result.reserve(state.count);
		for (std::size_t i = 0; i < state.count; ++i) {
			result.push_back(state.surfaces[i].stats);
			if (const auto& output = state.surfaces[i].output) {
				result.back().outputGeneration = output->generation;
				result.back().residentBytes = output->bytes;
				result.back().engineOwners = output->engineOwners.load();
			}
			result.back().evictionPending = state.surfaces[i].evictRequested;
			result.back().gpuFailed = state.gpuFailed;
		}
		return result;
	}
}
