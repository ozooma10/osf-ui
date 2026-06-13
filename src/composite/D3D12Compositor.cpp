#include "composite/D3D12Compositor.h"

#include "composite/EngineD3D12.h"
#include "core/Log.h"

// GDI-free Win32/D3D12 so the ERROR macro never collides with REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>

#include <cstring>

namespace SWUI
{
	namespace
	{
		constexpr std::uint32_t kUploadRingSlots = 3;
		constexpr std::uint32_t kLocateRetryInterval = 600;  // submits between lookup retries
		constexpr std::uint32_t kLocateMaxAttempts = 10;
		constexpr std::uint32_t kRowPitchAlignment = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

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
	}

	struct D3D12Compositor::Impl
	{
		// engine objects (owned references from LocateEngineD3D12)
		EngineD3D12 engine{};
		bool        locateGaveUp{ false };
		std::uint32_t locateAttempts{ 0 };
		std::uint64_t submitCount{ 0 };
		std::uint64_t skippedBusy{ 0 };

		// GPU resources (all created from the engine device)
		bool            resourcesFailed{ false };
		ID3D12Resource* texture{ nullptr };
		std::uint32_t   textureWidth{ 0 };
		std::uint32_t   textureHeight{ 0 };
		DXGI_FORMAT     textureFormat{ DXGI_FORMAT_UNKNOWN };
		std::uint32_t   uploadRowPitch{ 0 };

		struct Slot
		{
			ID3D12CommandAllocator*    allocator{ nullptr };
			ID3D12GraphicsCommandList* list{ nullptr };
			ID3D12Resource*            upload{ nullptr };
			std::uint8_t*              mapped{ nullptr };
			std::uint64_t              fenceValue{ 0 };
		};
		Slot slots[kUploadRingSlots]{};

		ID3D12Fence*  fence{ nullptr };
		HANDLE        fenceEvent{ nullptr };
		std::uint64_t nextFenceValue{ 1 };
		std::uint64_t uploadedFrames{ 0 };
		std::uint64_t lastUploadedFrameIndex{ 0 };
		bool          roundTripVerified{ false };
		bool          devMode{ false };

		~Impl() { ReleaseAll(); }

		void ReleaseAll()
		{
			WaitForGpuIdle();
			for (auto& slot : slots) {
				if (slot.upload && slot.mapped) {
					slot.upload->Unmap(0, nullptr);
					slot.mapped = nullptr;
				}
				SafeRelease(slot.list);
				SafeRelease(slot.allocator);
				SafeRelease(slot.upload);
			}
			SafeRelease(texture);
			SafeRelease(fence);
			if (fenceEvent) {
				::CloseHandle(fenceEvent);
				fenceEvent = nullptr;
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

		[[nodiscard]] bool EnsureLocated()
		{
			if (engine) {
				return true;
			}
			if (locateGaveUp) {
				return false;
			}
			// First submit, then every kLocateRetryInterval submits.
			if (locateAttempts > 0 && (submitCount % kLocateRetryInterval) != 0) {
				return false;
			}
			++locateAttempts;
			engine = LocateEngineD3D12();
			if (!engine) {
				if (locateAttempts >= kLocateMaxAttempts) {
					locateGaveUp = true;
					REX::ERROR(
						"D3D12Compositor: giving up locating the engine device/queue after {} attempts — "
						"frames will be dropped (layout drift? see reverse-engineering-notes.md §2)",
						locateAttempts);
				}
				return false;
			}
			if (!CreateFenceObjects()) {
				SafeRelease(engine.directQueue);
				SafeRelease(engine.device);
				locateGaveUp = true;
				return false;
			}
			return true;
		}

		[[nodiscard]] bool CreateFenceObjects()
		{
			if (FAILED(engine.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)))) {
				REX::ERROR("D3D12Compositor: CreateFence failed");
				return false;
			}
			fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!fenceEvent) {
				REX::ERROR("D3D12Compositor: CreateEvent failed");
				return false;
			}
			return true;
		}

		[[nodiscard]] bool EnsureResources(const FrameBufferView& a_frame)
		{
			if (resourcesFailed) {
				return false;
			}
			const auto wantedFormat = ToDxgiFormat(a_frame.format);
			if (texture && textureWidth == a_frame.width && textureHeight == a_frame.height && textureFormat == wantedFormat) {
				return true;
			}
			if (!CreateResources(a_frame, wantedFormat)) {
				// Tear down any partial state and stop retrying: a per-frame
				// create/fail loop would spam the log and hammer the device.
				resourcesFailed = true;
				ReleaseFrameResources();
				REX::ERROR("D3D12Compositor: resource creation failed; compositor disabled for this session");
				return false;
			}
			return true;
		}

		void ReleaseFrameResources()
		{
			WaitForGpuIdle();
			for (auto& slot : slots) {
				if (slot.upload && slot.mapped) {
					slot.upload->Unmap(0, nullptr);
					slot.mapped = nullptr;
				}
				SafeRelease(slot.list);
				SafeRelease(slot.allocator);
				SafeRelease(slot.upload);
				slot.fenceValue = 0;
			}
			SafeRelease(texture);
		}

		[[nodiscard]] bool CreateResources(const FrameBufferView& a_frame, const DXGI_FORMAT wantedFormat)
		{

			// (Re)create: first frame, or dimensions/format changed. Never
			// reuse in-flight resources — drain the queue first.
			ReleaseFrameResources();

			textureWidth = a_frame.width;
			textureHeight = a_frame.height;
			textureFormat = wantedFormat;
			uploadRowPitch = AlignUp(a_frame.width * 4, kRowPitchAlignment);

			// Destination texture: default heap, COPY_DEST for its whole
			// Phase 2 life (no draws read it yet).
			D3D12_HEAP_PROPERTIES defaultHeap{};
			defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC textureDesc{};
			textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			textureDesc.Width = textureWidth;
			textureDesc.Height = textureHeight;
			textureDesc.DepthOrArraySize = 1;
			textureDesc.MipLevels = 1;
			textureDesc.Format = textureFormat;
			textureDesc.SampleDesc.Count = 1;
			textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			if (FAILED(engine.device->CreateCommittedResource(
					&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture)))) {
				REX::ERROR("D3D12Compositor: failed to create {}x{} overlay texture", textureWidth, textureHeight);
				return false;
			}

			// Upload ring slots: persistent-mapped upload buffers + their own
			// allocator/list each (a list can be reset while another records).
			D3D12_HEAP_PROPERTIES uploadHeap{};
			uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = static_cast<std::uint64_t>(uploadRowPitch) * textureHeight;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			for (auto& slot : slots) {
				if (FAILED(engine.device->CreateCommittedResource(
						&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&slot.upload)))) {
					REX::ERROR("D3D12Compositor: failed to create upload buffer ({} bytes)", bufferDesc.Width);
					return false;
				}
				if (FAILED(slot.upload->Map(0, nullptr, reinterpret_cast<void**>(&slot.mapped)))) {
					REX::ERROR("D3D12Compositor: failed to map upload buffer");
					return false;
				}
				if (FAILED(engine.device->CreateCommandAllocator(
						D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
						reinterpret_cast<void**>(&slot.allocator)))) {
					REX::ERROR("D3D12Compositor: failed to create command allocator");
					return false;
				}
				if (FAILED(engine.device->CreateCommandList(
						0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
						__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&slot.list)))) {
					REX::ERROR("D3D12Compositor: failed to create command list");
					return false;
				}
				slot.list->Close();  // created open; ring expects closed lists
			}

			REX::INFO(
				"D3D12Compositor: created {}x{} {} texture + {}-slot upload ring (rowPitch {}, {} KB/slot) on the game device",
				textureWidth, textureHeight,
				textureFormat == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8",
				kUploadRingSlots, uploadRowPitch,
				bufferDesc.Width / 1024);
			return true;
		}

		void Upload(const FrameBufferView& a_frame)
		{
			// The renderer hands back its cached frame when nothing repainted
			// (ticks far outpace paints). Re-uploading identical pixels is
			// pure waste — only new frameIndex values go to the GPU.
			if (a_frame.frameIndex == lastUploadedFrameIndex) {
				return;
			}

			auto& slot = slots[uploadedFrames % kUploadRingSlots];
			if (slot.fenceValue != 0 && fence->GetCompletedValue() < slot.fenceValue) {
				// Ring saturated. Never stall the game thread: drop the frame.
				++skippedBusy;
				if (skippedBusy == 1 || (skippedBusy % 600) == 0) {
					REX::DEBUG("D3D12Compositor: upload ring busy; {} frame(s) skipped so far", skippedBusy);
				}
				return;
			}

			// CPU rows -> upload buffer (256-aligned pitch).
			const auto copyBytesPerRow = (std::min)(a_frame.strideBytes, a_frame.width * 4u);
			for (std::uint32_t y = 0; y < a_frame.height; ++y) {
				std::memcpy(
					slot.mapped + static_cast<std::size_t>(y) * uploadRowPitch,
					a_frame.pixels.data() + static_cast<std::size_t>(y) * a_frame.strideBytes,
					copyBytesPerRow);
			}

			// Record + execute the buffer->texture copy on the game's queue.
			slot.allocator->Reset();
			slot.list->Reset(slot.allocator, nullptr);

			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = slot.upload;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Footprint.Format = textureFormat;
			src.PlacedFootprint.Footprint.Width = textureWidth;
			src.PlacedFootprint.Footprint.Height = textureHeight;
			src.PlacedFootprint.Footprint.Depth = 1;
			src.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;

			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = texture;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;

			slot.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			slot.list->Close();

			ID3D12CommandList* lists[]{ slot.list };
			engine.directQueue->ExecuteCommandLists(1, lists);
			slot.fenceValue = nextFenceValue++;
			engine.directQueue->Signal(fence, slot.fenceValue);

			lastUploadedFrameIndex = a_frame.frameIndex;
			++uploadedFrames;
			if (uploadedFrames == 1) {
				REX::INFO("D3D12Compositor: first frame uploaded to the GPU texture (frame #{}, {}x{})",
					a_frame.frameIndex, a_frame.width, a_frame.height);
			} else if ((uploadedFrames % 600) == 0) {
				REX::DEBUG("D3D12Compositor: {} frames uploaded (latest #{}, {} skipped busy)",
					uploadedFrames, a_frame.frameIndex, skippedBusy);
			}

			if (devMode && !roundTripVerified) {
				roundTripVerified = true;  // one-shot, even if it fails
				VerifyRoundTrip(a_frame, slot.fenceValue, copyBytesPerRow);
			}
		}

		// Phase 2 exit criterion, automated: copy the texture back to a
		// readback buffer and byte-compare with what was submitted. One-shot,
		// devMode only — it deliberately blocks (a few ms) on the fence.
		void VerifyRoundTrip(const FrameBufferView& a_frame, const std::uint64_t a_uploadFence, const std::uint32_t a_copyBytesPerRow)
		{
			// Wait for the upload itself.
			if (fence->GetCompletedValue() < a_uploadFence) {
				fence->SetEventOnCompletion(a_uploadFence, fenceEvent);
				if (::WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0) {
					REX::WARN("D3D12Compositor: round-trip verify: timed out waiting for the upload fence");
					return;
				}
			}

			D3D12_HEAP_PROPERTIES readbackHeap{};
			readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = static_cast<std::uint64_t>(uploadRowPitch) * textureHeight;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			ID3D12Resource* readback = nullptr;
			if (FAILED(engine.device->CreateCommittedResource(
					&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback)))) {
				REX::WARN("D3D12Compositor: round-trip verify: failed to create readback buffer");
				return;
			}

			// Texture (COPY_DEST) -> COPY_SOURCE -> readback -> back to COPY_DEST.
			auto& slot = slots[0];
			slot.allocator->Reset();
			slot.list->Reset(slot.allocator, nullptr);

			D3D12_RESOURCE_BARRIER toSource{};
			toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toSource.Transition.pResource = texture;
			toSource.Transition.Subresource = 0;
			toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			slot.list->ResourceBarrier(1, &toSource);

			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = texture;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = readback;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Footprint.Format = textureFormat;
			dst.PlacedFootprint.Footprint.Width = textureWidth;
			dst.PlacedFootprint.Footprint.Height = textureHeight;
			dst.PlacedFootprint.Footprint.Depth = 1;
			dst.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;
			slot.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

			D3D12_RESOURCE_BARRIER toDest = toSource;
			toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			slot.list->ResourceBarrier(1, &toDest);
			slot.list->Close();

			ID3D12CommandList* lists[]{ slot.list };
			engine.directQueue->ExecuteCommandLists(1, lists);
			const auto verifyFence = nextFenceValue++;
			engine.directQueue->Signal(fence, verifyFence);
			slot.fenceValue = verifyFence;
			fence->SetEventOnCompletion(verifyFence, fenceEvent);
			if (::WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0) {
				REX::WARN("D3D12Compositor: round-trip verify: timed out waiting for the readback fence");
				readback->Release();
				return;
			}

			std::uint8_t* readbackData = nullptr;
			if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&readbackData)))) {
				REX::WARN("D3D12Compositor: round-trip verify: failed to map readback buffer");
				readback->Release();
				return;
			}

			std::size_t mismatched = 0;
			for (std::uint32_t y = 0; y < a_frame.height && mismatched == 0; ++y) {
				mismatched = std::memcmp(
									readbackData + static_cast<std::size_t>(y) * uploadRowPitch,
									a_frame.pixels.data() + static_cast<std::size_t>(y) * a_frame.strideBytes,
									a_copyBytesPerRow) == 0
				                 ? 0
				                 : (y + 1);
			}
			readback->Unmap(0, nullptr);
			readback->Release();

			if (mismatched == 0) {
				REX::INFO(
					"D3D12Compositor: ROUND-TRIP VERIFIED — GPU texture matches the submitted frame "
					"({}x{}, {} bytes/row compared) — Phase 2 upload path proven",
					a_frame.width, a_frame.height, a_copyBytesPerRow);
			} else {
				REX::ERROR("D3D12Compositor: ROUND-TRIP MISMATCH at row {} — upload path is wrong, do not build on it", mismatched - 1);
			}
		}
	};

	D3D12Compositor::D3D12Compositor() = default;
	D3D12Compositor::~D3D12Compositor() = default;

	bool D3D12Compositor::Initialize()
	{
		_impl = std::make_unique<Impl>();
		_impl->devMode = Log::DevMode();
		REX::INFO(
			"D3D12Compositor: initialized (engine device/queue will be located on first frame; "
			"upload-only — present-time composition is Phase 3)");
		return true;
	}

	void D3D12Compositor::Shutdown()
	{
		if (_impl) {
			REX::INFO("D3D12Compositor: shutdown after {} uploaded frame(s) ({} skipped busy)",
				_impl->uploadedFrames, _impl->skippedBusy);
			_impl.reset();
		}
	}

	void D3D12Compositor::Submit(const FrameBufferView& a_frame)
	{
		if (!_impl) {
			return;
		}
		++_impl->submitCount;
		if (!_impl->EnsureLocated()) {
			return;
		}
		if (!_impl->EnsureResources(a_frame)) {
			return;
		}
		_impl->Upload(a_frame);
	}
}
