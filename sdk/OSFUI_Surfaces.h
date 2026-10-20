// Optional extension services supplied by OSFUI.dll. No STL or ownership transfer across the ABI.
#pragma once
#include "OSFUI.h"

struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace OSFUI::API::Surfaces
{
    inline constexpr std::uint32_t kVersion = 0x00010001;
    using Handle = std::uint64_t;
    using Interaction = std::uint64_t;
    struct View { const char* id; const char* texture; std::uint32_t width, height; };
    using Visitor = void (*)(const View&, void*) noexcept;
    // Called on OSF UI's serialized runtime tick; never from an SFSE/engine event callback.
    using Tick = void (*)(void*, bool dataLoaded) noexcept;
    enum class Phase : std::uint32_t { Started, Ended, Rejected };
    using InteractionCallback = void (*)(Interaction, Phase, const char* reason, void*) noexcept;
    // 1.1. InWorld: exclusive keyboard/controller, no pointer, pause, or overlay.
    // Fullscreen: menu mode (pause + cursor), the surface drawn letterboxed over the game,
    // mouse routed to it. Only views whose manifest sets "fullscreen": true are admitted;
    // others end Rejected with reason "fullscreen-unsupported".
    enum class Presentation : std::uint32_t { InWorld, Fullscreen };
    struct InteractionOptions
    {
        std::uint32_t size{ sizeof(InteractionOptions) };
        Presentation presentation{ Presentation::InWorld };
    };
    struct FrameStatus
    {
        std::uint64_t submitted{}, completed{};
        bool ready{}, idle{true}, failed{};
    };
    struct IService
    {
        // Discovery is immutable after startup. Callback strings live only for the call.
        virtual void VisitViews(Visitor, void*) noexcept = 0;
        // One process-lifetime extension tick owner. Registration is idempotent for the same owner.
        virtual bool RegisterTick(Tick, void*) noexcept = 0;
        // Thread-safe demand leases. Each Acquire returns a unique token; same view shares rendering.
        // Release is asynchronous and preserves outstanding GPU reads. Tokens must not be saved.
        virtual Handle Acquire(const char* view) noexcept = 0;
        virtual void Release(Handle) noexcept = 0;
        // Render thread, immediately before engine sampling on its DIRECT queue. Nonblocking.
        // Destination: matching BGRA8 UNORM/TYPELESS, one mip/slice, shader-readable before/after.
        // destinationGeneration must change whenever the caller replaces the resource.
        // Core retains resources until completion. Caller serializes this queue's submissions;
        // its ExecuteCommandLists hook must bypass its own recursively submitted copies.
        virtual FrameStatus CopyFrame(Handle, ID3D12Resource*, std::uint64_t destinationGeneration, ID3D12CommandQueue*) noexcept = 0;
        virtual bool IsIdle(Handle) noexcept = 0;
        // Queued and exclusive (see Presentation). Callback: Started then Ended, or Rejected.
        // Context lives until the terminal callback.
        virtual Interaction BeginInteraction(Handle, InteractionCallback, void*) noexcept = 0;
        virtual bool EndInteraction(Interaction) noexcept = 0;
        // 1.1. BeginInteraction with options; BeginInteraction equals InWorld presentation.
        virtual Interaction BeginInteractionWith(Handle, const InteractionOptions&, InteractionCallback, void*) noexcept = 0;
    protected:
        ~IService() = default;
    };
    inline IService* RequestInterface(std::uint32_t version = kVersion) noexcept
    {
        const auto module = REX::W32::GetModuleHandleW(L"OSFUI.dll");
        const auto request = module ? reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, "OSFUI_RequestSurfaces")) : nullptr;
        std::uint32_t actual{};
        auto* api = request ? request(version, &actual) : nullptr;
        return Supports(actual, version) ? static_cast<IService*>(api) : nullptr;
    }
}
