#include "Surfaces/SurfaceFrames.h"
#include <atomic>

namespace RuntimeTest
{
    std::atomic_bool surfaceGpuIdle{true}, surfaceGpuReady{true};
}
namespace OSFUI
{
    struct SurfaceFrames::Impl {};
    SurfaceFrames::SurfaceFrames(std::shared_ptr<SharedFrameConsumer>) : m_impl(std::make_unique<Impl>()) {}
    SurfaceFrames::~SurfaceFrames() = default;
    API::Surfaces::FrameStatus SurfaceFrames::Copy(ID3D12Resource*, std::uint64_t, ID3D12CommandQueue*) { return {}; }
    std::optional<SurfaceFrames::DrawFrame> SurfaceFrames::RecordDraw(ID3D12GraphicsCommandList*, ID3D12Device*) { return std::nullopt; }
    const std::shared_ptr<SharedFrameConsumer>& SurfaceFrames::Consumer() const { static const std::shared_ptr<SharedFrameConsumer> none; return none; }
    bool SurfaceFrames::Idle() { return RuntimeTest::surfaceGpuIdle; }
    bool SurfaceFrames::Ready() { return RuntimeTest::surfaceGpuReady; }
}
