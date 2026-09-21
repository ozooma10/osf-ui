#include "World/WorldTexture.h"

#include "Composite/D3D12Prologue.h"
#include "Composite/EngineD3D12.h"
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
		constexpr auto kShaderState = static_cast<D3D12_RESOURCE_STATES>(
			static_cast<unsigned>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | static_cast<unsigned>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		constexpr auto kFenceFailure = (std::numeric_limits<std::uint64_t>::max)();
		std::atomic<CreateSrvFn> g_createSrv{ nullptr };
		std::atomic<ExecuteFn> g_execute{ nullptr };

		struct OwnedHandles
		{
			SharedRingDesc desc;
			~OwnedHandles()
			{
				for (auto handle : desc.slotHandles) {
					if (handle) ::CloseHandle(handle);
				}
				if (desc.produceFence) ::CloseHandle(desc.produceFence);
				for (auto handle : desc.consumeFences) {
					if (handle) ::CloseHandle(handle);
				}
			}
		};

		struct Ring
		{
			std::array<ID3D12Resource*, SharedRingDesc::kMaxSlots> slots{};
			std::array<ID3D12Fence*, SharedRingDesc::kMaxSlots> consume{};
			ID3D12Fence* produce{ nullptr };
			std::uint32_t count{ 0 };
			std::uint64_t generation{ 0 };
			std::array<std::uint64_t, SharedRingDesc::kMaxSlots> pending{};
			std::array<std::uint64_t, SharedRingDesc::kMaxSlots> signaled{};

			~Ring()
			{
				for (auto*& slot : slots) SafeRelease(slot);
				for (auto*& fence : consume) SafeRelease(fence);
				SafeRelease(produce);
			}
		};

		struct Surface
		{
			SurfaceStats stats;
			std::uint64_t lastCompletion{ 0 };
			ID3D12Resource* texture{ nullptr };
			std::shared_ptr<Ring> ring;
		};

		struct CopyContext
		{
			ID3D12CommandAllocator* allocator{ nullptr };
			ID3D12GraphicsCommandList* list{ nullptr };
			std::uint64_t completion{ 0 };
			std::size_t surface{ kMaxSurfaces };
			std::uint64_t serial{ 0 };
			std::array<ID3D12Resource*, kMaxSurfaces> initialUploads{};
			// Only our command lists reference ring resources. These references
			// retire with our fence, independently of engine material lifetimes.
			std::shared_ptr<Ring> ring;
		};

		struct State
		{
			std::mutex mutex;
			std::mutex queueMutex;
			std::array<Surface, kMaxSurfaces> surfaces;
			std::size_t count{ 0 };
			std::size_t nextSurface{ 0 };
			EngineD3D12 engine;
			ID3D12Fence* completionFence{ nullptr };
			std::array<CopyContext, kCopyContexts> copies;
			std::uint64_t nextCompletion{ 1 };
			bool hooksAttempted{ false };
			std::atomic<bool> enabled{ false };
			std::atomic<bool> installed{ false };
			bool gpuFailed{ false };
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
			       a_desc.Flags == D3D12_RESOURCE_FLAG_NONE && a_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
			       a_desc.Width == a_desc.Height;
		}

		bool PlainSrv(const D3D12_SHADER_RESOURCE_VIEW_DESC* a_desc)
		{
			return a_desc && a_desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
			       (a_desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM || a_desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
			       a_desc->Texture2D.MostDetailedMip == 0 &&
			       (a_desc->Texture2D.MipLevels == 1 || a_desc->Texture2D.MipLevels == UINT_MAX) &&
			       a_desc->Texture2D.PlaneSlice == 0 && a_desc->Texture2D.ResourceMinLODClamp == 0.0f;
		}

		void STDMETHODCALLTYPE CreateSrvThunk(ID3D12Device* a_device, ID3D12Resource* a_resource,
			const D3D12_SHADER_RESOURCE_VIEW_DESC* a_desc, D3D12_CPU_DESCRIPTOR_HANDLE a_handle)
		{
			auto& state = GetState();
			const auto original = g_createSrv.load(std::memory_order_acquire);
			if (!original) return;
			D3D12_SHADER_RESOURCE_VIEW_DESC replacement{};
			if (a_resource && PlainSrv(a_desc)) {
				const auto desc = a_resource->GetDesc();
				if (Placeholder(desc)) {
					std::scoped_lock lock(state.mutex);
					if (a_device == state.engine.device) {
						for (std::size_t i = 0; i < state.count; ++i) {
							auto& surface = state.surfaces[i];
							if (surface.texture && a_resource != surface.texture && desc.Width == surface.stats.placeholderSize) {
								replacement = *a_desc;
								replacement.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
								replacement.Texture2D.MipLevels = 1;
								a_resource = surface.texture;
								a_desc = &replacement;
								if (++surface.stats.boundDescriptors == 1) {
									REX::INFO("WorldTexture: '{}' material bound ({}x{} placeholder -> {}x{} stable texture)",
										surface.stats.id, desc.Width, desc.Height, surface.stats.width, surface.stats.height);
								}
								break;
							}
						}
					}
				}
			}
			// Never remember a CPU descriptor address or rewrite an existing slot.
			original(a_device, a_resource, a_desc, a_handle);
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

		bool RetireCopies(State& a_state)
		{
			if (!a_state.completionFence || a_state.gpuFailed) return false;
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
						if (copy.ring->generation == surface.stats.ringGeneration && copy.completion > surface.lastCompletion) {
							surface.lastCompletion = copy.completion;
							surface.stats.lastCompletedFrame = copy.serial;
						}
					}
					copy.ring.reset();
					for (auto*& upload : copy.initialUploads) SafeRelease(upload);
					copy.completion = 0;
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
			if (!a_state.installed || a_state.gpuFailed) return;
			if (!RetireCopies(a_state)) return;
			const auto first = a_state.nextSurface;
			for (std::size_t offset = 0; offset < a_state.count; ++offset) {
				const auto i = (first + offset) % a_state.count;
				auto& surface = a_state.surfaces[i];
				if (!surface.ring) continue;
				auto& ring = *surface.ring;
				const auto produced = ring.produce->GetCompletedValue();
				if (produced == kFenceFailure) {
					if (!ProducerFailure(a_state, surface, "producer completion", DXGI_ERROR_DEVICE_REMOVED)) return;
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
				if (selected == ring.count) continue;
				auto available = std::ranges::find_if(a_state.copies, [](const CopyContext& a_copy) { return a_copy.completion == 0; });
				if (available == a_state.copies.end()) {
					++surface.stats.skippedBusy;
					continue;
				}
				auto& copy = *available;
				auto hr = copy.allocator->Reset();
				if (SUCCEEDED(hr)) hr = copy.list->Reset(copy.allocator, nullptr);
				if (FAILED(hr)) {
					GpuFailure(a_state, "copy allocator/list reset", hr);
					return;
				}
				D3D12_RESOURCE_BARRIER before[]{
					Transition(surface.texture, kShaderState, D3D12_RESOURCE_STATE_COPY_DEST),
					Transition(ring.slots[selected], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)
				};
				copy.list->ResourceBarrier(2, before);
				copy.list->CopyResource(surface.texture, ring.slots[selected]);
				D3D12_RESOURCE_BARRIER after[]{
					Transition(surface.texture, D3D12_RESOURCE_STATE_COPY_DEST, kShaderState),
					Transition(ring.slots[selected], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				copy.list->ResourceBarrier(2, after);
				hr = copy.list->Close();
				if (FAILED(hr)) {
					GpuFailure(a_state, "copy list close", hr);
					return;
				}
				copy.ring = surface.ring;
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
					if (consumed <= ring.signaled[slot] || consumed > serial) continue;
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
				if (FAILED(consumeResult) && !ProducerFailure(a_state, surface, "consume signal", consumeResult)) return;
			}
		}

		void STDMETHODCALLTYPE ExecuteThunk(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			const auto original = g_execute.load(std::memory_order_acquire);
			if (!original) return;
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
			for (auto& surface : a_state.surfaces) SafeRelease(surface.texture);
			for (auto& copy : a_state.copies) {
				for (auto*& upload : copy.initialUploads) SafeRelease(upload);
				SafeRelease(copy.list);
				SafeRelease(copy.allocator);
			}
			SafeRelease(a_state.completionFence);
			SafeRelease(a_state.engine.directQueue);
			SafeRelease(a_state.engine.device);
		}

		bool CreateResources(State& a_state)
		{
			auto* device = a_state.engine.device;
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a_state.completionFence)))) return false;
			for (auto& copy : a_state.copies) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator))) ||
					FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator, nullptr, IID_PPV_ARGS(&copy.list))) ||
					FAILED(copy.list->Close())) return false;
			}
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap.CreationNodeMask = 1;
			heap.VisibleNodeMask = 1;
			auto& initialization = a_state.copies[0];
			if (FAILED(initialization.list->Reset(initialization.allocator, nullptr))) return false;
			for (std::size_t i = 0; i < a_state.count; ++i) {
				auto& surface = a_state.surfaces[i];
				D3D12_RESOURCE_DESC desc{};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				desc.Width = surface.stats.width;
				desc.Height = surface.stats.height;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
				desc.SampleDesc.Count = 1;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr, IID_PPV_ARGS(&surface.texture)))) return false;
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
				UINT64 totalBytes = 0;
				device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
				D3D12_HEAP_PROPERTIES uploadHeap = heap;
				uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC uploadDesc{};
				uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadDesc.Width = totalBytes;
				uploadDesc.Height = 1;
				uploadDesc.DepthOrArraySize = 1;
				uploadDesc.MipLevels = 1;
				uploadDesc.SampleDesc.Count = 1;
				uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				auto*& upload = initialization.initialUploads[i];
				if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
				void* mapped = nullptr;
				const D3D12_RANGE readRange{ 0, 0 };
				if (FAILED(upload->Map(0, &readRange, &mapped))) return false;
				std::memset(mapped, 0, static_cast<std::size_t>(totalBytes));
				for (std::uint32_t row = 0; row < surface.stats.height; ++row) {
					auto* pixels = reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(mapped) + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch);
					std::fill_n(pixels, surface.stats.width, 0xFF000000u);
				}
				upload->Unmap(0, nullptr);
				D3D12_TEXTURE_COPY_LOCATION source{};
				source.pResource = upload;
				source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				source.PlacedFootprint = footprint;
				D3D12_TEXTURE_COPY_LOCATION destination{};
				destination.pResource = surface.texture;
				destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				initialization.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
				const auto barrier = Transition(surface.texture, D3D12_RESOURCE_STATE_COPY_DEST, kShaderState);
				initialization.list->ResourceBarrier(1, &barrier);
			}
			return SUCCEEDED(initialization.list->Close());
		}
	}

	bool Configure(std::span<const SurfaceDesc> a_surfaces)
	{
		auto& state = GetState();
		std::scoped_lock lock(state.mutex);
		if (state.hooksAttempted || a_surfaces.size() > kMaxSurfaces) return false;
		for (std::size_t i = 0; i < a_surfaces.size(); ++i) {
			const auto& desc = a_surfaces[i];
			if (desc.id.empty() || desc.placeholderSize < 256 || desc.placeholderSize > 4096 ||
				(desc.placeholderSize & (desc.placeholderSize - 1)) == 0 ||
				desc.width == 0 || desc.height == 0 || desc.width > 4096 || desc.height > 4096) return false;
			for (std::size_t j = 0; j < i; ++j) {
				if (a_surfaces[j].id == desc.id || a_surfaces[j].placeholderSize == desc.placeholderSize) return false;
			}
		}
		state.count = a_surfaces.size();
		for (std::size_t i = 0; i < state.count; ++i) {
			const auto& desc = a_surfaces[i];
			state.surfaces[i].stats = { .id = desc.id, .placeholderSize = desc.placeholderSize, .width = desc.width, .height = desc.height };
		}
		state.enabled.store(state.count != 0, std::memory_order_release);
		return true;
	}

	bool Install()
	{
		auto& state = GetState();
		if (state.installed.load(std::memory_order_acquire)) return true;
		if (!state.enabled.load(std::memory_order_acquire)) return false;
		std::scoped_lock queueLock(state.queueMutex);
		std::scoped_lock lock(state.mutex);
		if (state.installed) return true;
		if (state.hooksAttempted || state.count == 0) return false;
		state.engine = LocateEngineD3D12();
		if (!state.engine) return false;
		if (!CreateResources(state)) {
			REX::ERROR("WorldTexture: GPU resource creation failed");
			ReleaseUninstalled(state);
			state.hooksAttempted = true;
			return false;
		}
		// Publish the originals before patching: render threads can enter either
		// hook immediately. Every replacement texture already exists here.
		state.hooksAttempted = true;
		auto** queueSlot = &(*reinterpret_cast<void***>(state.engine.directQueue))[kExecuteSlot];
		g_execute.store(reinterpret_cast<ExecuteFn>(*queueSlot), std::memory_order_release);
		if (!REL::WriteSafeData(queueSlot, reinterpret_cast<void*>(&ExecuteThunk)) && *queueSlot != reinterpret_cast<void*>(&ExecuteThunk)) {
			REX::ERROR("WorldTexture: ExecuteCommandLists hook installation failed");
			return false;
		}
		// Queue opaque-black initialization before exposing any replacement SRV.
		// ExecuteThunk cannot interleave an engine submission while these locks
		// are held. No CPU wait is needed: the same queue orders subsequent reads.
		auto& initialization = state.copies[0];
		ID3D12CommandList* lists[]{ initialization.list };
		initialization.completion = kFenceFailure;
		g_execute.load(std::memory_order_acquire)(state.engine.directQueue, 1, lists);
		const auto completion = state.nextCompletion++;
		const auto hr = state.engine.directQueue->Signal(state.completionFence, completion);
		if (FAILED(hr)) {
			GpuFailure(state, "initialization signal", hr);
			return false;
		}
		initialization.completion = completion;
		auto** deviceSlot = &(*reinterpret_cast<void***>(state.engine.device))[kCreateSrvSlot];
		g_createSrv.store(reinterpret_cast<CreateSrvFn>(*deviceSlot), std::memory_order_release);
		if (!REL::WriteSafeData(deviceSlot, reinterpret_cast<void*>(&CreateSrvThunk)) && *deviceSlot != reinterpret_cast<void*>(&CreateSrvThunk)) {
			REX::ERROR("WorldTexture: CreateShaderResourceView hook installation failed");
			return false;
		}
		state.installed = true;
		REX::INFO("WorldTexture: installed for {} surfaces (stable material textures, {} GPU copy contexts)", state.count, kCopyContexts);
		return true;
	}

	bool IsInstalled()
	{
		return GetState().installed.load(std::memory_order_acquire);
	}

	void SetSharedRing(std::size_t a_surface, const SharedRingDesc& a_desc)
	{
		const OwnedHandles handles{ a_desc };
		auto& state = GetState();
		std::scoped_lock lock(state.mutex);
		if (a_surface >= state.count) return;
		auto& surface = state.surfaces[a_surface];
		DiscardRing(surface);
		if (a_desc.slotCount == 0) return;
		if (!state.installed || state.gpuFailed || a_desc.slotCount > SharedRingDesc::kMaxSlots ||
			a_desc.generation == 0 || a_desc.width != surface.stats.width || a_desc.height != surface.stats.height) {
			++surface.stats.ringOpenFailures;
			return;
		}
		auto ring = std::make_shared<Ring>();
		auto* device = state.engine.device;
		HRESULT hr = S_OK;
		for (std::uint32_t slot = 0; slot < a_desc.slotCount && SUCCEEDED(hr); ++slot) {
			hr = a_desc.slotHandles[slot] ? device->OpenSharedHandle(a_desc.slotHandles[slot], IID_PPV_ARGS(&ring->slots[slot])) : E_HANDLE;
			if (SUCCEEDED(hr)) {
				const auto desc = ring->slots[slot]->GetDesc();
				if (!PlainTexture(desc) || desc.Width != a_desc.width || desc.Height != a_desc.height ||
					(desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS)) hr = E_INVALIDARG;
			}
			if (SUCCEEDED(hr)) hr = a_desc.consumeFences[slot] ? device->OpenSharedHandle(a_desc.consumeFences[slot], IID_PPV_ARGS(&ring->consume[slot])) : E_HANDLE;
		}
		if (SUCCEEDED(hr)) hr = a_desc.produceFence ? device->OpenSharedHandle(a_desc.produceFence, IID_PPV_ARGS(&ring->produce)) : E_HANDLE;
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
		auto& state = GetState();
		std::scoped_lock lock(state.mutex);
		if (a_surface >= state.count) return;
		auto& surface = state.surfaces[a_surface];
		if (!surface.ring || a_frame.ringGeneration != surface.ring->generation || a_frame.frameIndex == 0 ||
			a_frame.sharedSlot >= surface.ring->count || a_frame.width != surface.stats.width || a_frame.height != surface.stats.height) {
			++surface.stats.rejectedFrames;
			return;
		}
		auto& pending = surface.ring->pending[a_frame.sharedSlot];
		if (a_frame.frameIndex <= pending) return;
		pending = a_frame.frameIndex;
		++surface.stats.submittedFrames;
	}

	std::vector<SurfaceStats> Snapshot()
	{
		auto& state = GetState();
		std::scoped_lock lock(state.mutex);
		(void)RetireCopies(state);
		std::vector<SurfaceStats> result;
		result.reserve(state.count);
		for (std::size_t i = 0; i < state.count; ++i) {
			result.push_back(state.surfaces[i].stats);
			result.back().gpuFailed = state.gpuFailed;
		}
		return result;
	}
}
