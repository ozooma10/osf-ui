#include "Surfaces/SurfaceFrames.h"
#include "Composite/D3D12Prologue.h"
#include <wrl/client.h>

namespace OSFUI
{
    using Microsoft::WRL::ComPtr;
    struct SurfaceFrames::Impl
    {
        std::mutex mutex;
        std::shared_ptr<SharedFrameConsumer> frames;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence, producer;
        std::array<ComPtr<ID3D12Resource>, SharedRingDesc::kMaxSlots> ring;
        std::uint64_t generation{}, nextFence{}, lastFrame{}, lastGeneration{};
        ID3D12Resource* destination{}; // Identity only; submitted copies retain resources.
        std::uint64_t destinationTag{};
        struct Copy
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Resource> output;
            std::uint64_t completion{}, generation{}, destinationTag{};
        };
        std::array<Copy, 3> copies;
        API::Surfaces::FrameStatus status;

        std::optional<FrameBufferView> Poll()
        {
            status.idle = true;
            const auto completed = fence ? fence->GetCompletedValue() : 0;
            if (completed == UINT64_MAX) { status.failed = true; status.ready = false; }
            for (auto& copy : copies) {
                if (!copy.completion) continue;
                if (completed != UINT64_MAX && copy.completion != UINT64_MAX && copy.completion <= completed) {
                    frames->Completed(reinterpret_cast<std::uintptr_t>(fence.Get()), copy.completion);
                    ++status.completed;
                    if (copy.generation == generation && copy.output.Get() == destination && copy.destinationTag == destinationTag && !status.failed) status.ready = true;
                    copy.output.Reset();
                    copy.completion = 0;
                } else status.idle = false;
            }
            auto latest = frames->Latest();
            if (!latest || latest->ringGeneration != generation) status.ready = false;
            return latest;
        }
        bool Setup(ID3D12CommandQueue* value)
        {
            if (queue) return queue.Get() == value;
            if (!value || value->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
            ComPtr<ID3D12Device> queueDevice;
            if (FAILED(value->GetDevice(IID_PPV_ARGS(&queueDevice))) || (device && device != queueDevice)) return false;
            device = queueDevice;
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
            for (auto& copy : copies) {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator))) ||
                    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(), nullptr, IID_PPV_ARGS(&copy.list))) ||
                    FAILED(copy.list->Close())) return false;
            }
            queue = value;
            return true;
        }
        bool Adopt()
        {
            if (auto announced = frames->TakeRingIfIdle()) {
                ring = {};
                producer.Reset();
                generation = announced->generation;
                status.ready = false;
                bool ok = announced->slotCount && announced->slotCount <= ring.size();
                for (std::size_t i = 0; ok && i < announced->slotCount; ++i)
                    ok = SUCCEEDED(device->OpenSharedHandle(announced->slotHandles[i], IID_PPV_ARGS(&ring[i])));
                if (ok) ok = SUCCEEDED(device->OpenSharedHandle(announced->produceFence, IID_PPV_ARGS(&producer)));
                SharedFrameConsumer::CloseHandles(*announced);
                if (!ok) { status.failed = true; return false; }
            }
            return producer != nullptr;
        }
        static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
            return barrier;
        }
        API::Surfaces::FrameStatus CopyTo(ID3D12Resource* output, std::uint64_t tag, ID3D12CommandQueue* onQueue)
        {
            const auto current = Poll();
            // Callers may poll on every queue submission; an already-copied frame skips the checks below.
            // A newly announced ring publishes under a new generation, so it never matches here.
            if (current && current->ringGeneration == lastGeneration && current->frameIndex == lastFrame &&
                destination == output && destinationTag == tag && queue.Get() == onQueue) return status;
            if (destination != output || destinationTag != tag) status.ready = false;
            if (status.failed || !output || !tag || !Setup(onQueue) || !Adopt()) return status;
            const auto produced = producer->GetCompletedValue();
            if (produced == UINT64_MAX) { status.ready = false; return status; } // dead host may recover
            auto latest = frames->Latest();
            if (!latest || latest->ringGeneration != generation || latest->frameIndex > produced) return status;
            const auto desc = output->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != latest->width || desc.Height != latest->height ||
                desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS)) return status;
            if (latest->sharedSlot >= ring.size() || !ring[latest->sharedSlot]) { status.failed = true; status.ready = false; return status; }
            const auto sourceDesc = ring[latest->sharedSlot]->GetDesc();
            if (sourceDesc.Dimension != desc.Dimension || sourceDesc.Width != desc.Width || sourceDesc.Height != desc.Height ||
                sourceDesc.MipLevels != 1 || sourceDesc.DepthOrArraySize != 1 || sourceDesc.SampleDesc.Count != 1 ||
                (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS)) {
                status.failed = true; status.ready = false; return status;
            }
            ComPtr<ID3D12Device> outputDevice;
            if (FAILED(output->GetDevice(IID_PPV_ARGS(&outputDevice))) || outputDevice.Get() != device.Get()) return status;
            if (lastFrame == latest->frameIndex && lastGeneration == generation && destination == output && destinationTag == tag) return status;
            auto available = std::ranges::find_if(copies, [](const Copy& copy) { return !copy.completion; });
            if (available == copies.end()) return status;
            auto& copy = *available;
            if (FAILED(copy.allocator->Reset()) || FAILED(copy.list->Reset(copy.allocator.Get(), nullptr))) { status.failed = true; return status; }
            const auto listId = reinterpret_cast<std::uintptr_t>(copy.list.Get());
            auto frame = frames->Record(listId, generation, produced);
            if (!frame) { copy.list->Close(); return status; }
            constexpr auto shader = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            auto* source = ring[frame->sharedSlot].Get();
            const D3D12_RESOURCE_BARRIER before[]{Transition(output, shader, D3D12_RESOURCE_STATE_COPY_DEST), Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
            copy.list->ResourceBarrier(2, before);
            copy.list->CopyResource(output, source);
            const D3D12_RESOURCE_BARRIER after[]{Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, shader), Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)};
            copy.list->ResourceBarrier(2, after);
            copy.output = output;
            copy.generation = generation; copy.destinationTag = tag;
            copy.completion = UINT64_MAX; // failures retain all resources
            const std::array<std::uintptr_t, 1> ids{listId};
            if (FAILED(copy.list->Close())) {
                frames->Submitted(ids, 0, 0); status.failed = true; status.idle = false; return status;
            }
            if (destination != output || destinationTag != tag) status.ready = false;
            destination = output; destinationTag = tag;
            ID3D12CommandList* lists[]{copy.list.Get()};
            queue->ExecuteCommandLists(1, lists);
            const auto value = ++nextFence;
            if (FAILED(queue->Signal(fence.Get(), value))) {
                frames->Submitted(ids, 0, 0); status.failed = true; status.ready = false;
            } else {
                copy.completion = value;
                frames->Submitted(ids, reinterpret_cast<std::uintptr_t>(fence.Get()), value);
            }
            lastFrame = frame->frameIndex; lastGeneration = generation;
            ++status.submitted;
            status.idle = false;
            return status;
        }
    };
    std::optional<SurfaceFrames::DrawFrame> SurfaceFrames::RecordDraw(ID3D12GraphicsCommandList* list, ID3D12Device* onDevice)
    {
        std::scoped_lock lock(m_impl->mutex);
        auto& impl = *m_impl;
        if (!list || !onDevice || impl.status.failed || (impl.device && impl.device.Get() != onDevice)) return std::nullopt;
        if (!impl.device) impl.device = onDevice;
        if (!impl.Adopt()) return std::nullopt;
        const auto produced = impl.producer->GetCompletedValue();
        if (produced == UINT64_MAX) return std::nullopt;
        const auto frame = impl.frames->Record(reinterpret_cast<std::uintptr_t>(list), impl.generation, produced);
        if (!frame || frame->sharedSlot >= impl.ring.size() || !impl.ring[frame->sharedSlot]) return std::nullopt;
        return DrawFrame{ impl.ring[frame->sharedSlot].Get(), impl.generation, frame->sharedSlot, frame->width, frame->height };
    }
    const std::shared_ptr<SharedFrameConsumer>& SurfaceFrames::Consumer() const { return m_impl->frames; }
    SurfaceFrames::SurfaceFrames(std::shared_ptr<SharedFrameConsumer> frames) : m_impl(std::make_unique<Impl>()) { m_impl->frames = std::move(frames); }
    SurfaceFrames::~SurfaceFrames() { if (!Idle()) (void)m_impl.release(); } // uncertain GPU completion pins resources until process exit
    API::Surfaces::FrameStatus SurfaceFrames::Copy(ID3D12Resource* output, std::uint64_t tag, ID3D12CommandQueue* queue) { std::scoped_lock lock(m_impl->mutex); return m_impl->CopyTo(output, tag, queue); }
    bool SurfaceFrames::Idle() { std::scoped_lock lock(m_impl->mutex); m_impl->Poll(); return m_impl->status.idle && !m_impl->frames->HasReads(); }
    bool SurfaceFrames::Ready() { std::scoped_lock lock(m_impl->mutex); m_impl->Poll(); return m_impl->status.ready; }
}
