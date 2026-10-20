#pragma once
#include "OSFUI_Surfaces.h"
#include "Render/SharedFrameConsumer.h"
#include <memory>
#include <optional>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace OSFUI
{
    class SurfaceFrames
    {
    public:
        explicit SurfaceFrames(std::shared_ptr<SharedFrameConsumer> frames);
        ~SurfaceFrames();
        API::Surfaces::FrameStatus Copy(ID3D12Resource*, std::uint64_t, ID3D12CommandQueue*);
        // Render thread. Reserves the current frame for a read recorded on a_list; the caller
        // reports that list's submission and completion through Consumer(), keyed by its own fence.
        struct DrawFrame { ID3D12Resource* texture; std::uint64_t generation; std::uint32_t slot, width, height; };
        std::optional<DrawFrame> RecordDraw(ID3D12GraphicsCommandList* a_list, ID3D12Device* a_device);
        const std::shared_ptr<SharedFrameConsumer>& Consumer() const;
        // True when no copy or draw read of the shared ring is outstanding.
        bool Idle();
        bool Ready();
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
