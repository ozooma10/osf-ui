#pragma once
#include "OSFUI_Surfaces.h"
#include "Render/SharedFrameConsumer.h"
#include <memory>

namespace OSFUI
{
    class SurfaceFrames
    {
    public:
        explicit SurfaceFrames(std::shared_ptr<SharedFrameConsumer> frames);
        ~SurfaceFrames();
        API::Surfaces::FrameStatus Copy(ID3D12Resource*, std::uint64_t, ID3D12CommandQueue*);
        bool Idle();
        bool Ready();
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
