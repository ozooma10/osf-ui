#pragma once
#include "OSFUI_Surfaces.h"
#include "Render/BrowserHostClient.h"
#include "Render/BrowserHostRecovery.h"
#include "Surfaces/SurfaceFrames.h"
#include "Bridge/MessageBridge.h"
#include <future>
#include <mutex>
#include <deque>

namespace OSFUI
{
    class SurfaceService final : public API::Surfaces::IService
    {
    public:
        static SurfaceService& Get();
        void Configure(const std::vector<ViewManifest>&);
        void Initialize(MessageBridge&, bool developerMode, std::function<std::optional<std::string>()> language);
        void Update(bool dataLoaded);
        bool WantsInteraction() const;
        void UpdateInteraction(bool allowed, bool neutral, const std::function<bool()>& admit);
        void CancelInteraction() { ++m_cancelGeneration; }
        void FinishInteraction(const char* reason);
        std::shared_ptr<BrowserHostClient> InputRenderer() const { return m_input.load(); }
        std::optional<std::string> InputView() const;
        bool IsInstantiated(std::string_view) const;
        bool HasAvailableViews() const;
        const char* LoadState(std::string_view) const;
        bool Send(std::string_view, std::string_view);

        void VisitViews(API::Surfaces::Visitor, void*) noexcept override;
        bool RegisterTick(API::Surfaces::Tick, void*) noexcept override;
        API::Surfaces::Handle Acquire(const char*) noexcept override;
        void Release(API::Surfaces::Handle) noexcept override;
        API::Surfaces::FrameStatus CopyFrame(API::Surfaces::Handle, ID3D12Resource*, std::uint64_t destinationGeneration, ID3D12CommandQueue*) noexcept override;
        bool IsIdle(API::Surfaces::Handle) noexcept override;
        API::Surfaces::Interaction BeginInteraction(API::Surfaces::Handle, API::Surfaces::InteractionCallback, void*) noexcept override;
        bool EndInteraction(API::Surfaces::Interaction) noexcept override;
    private:
        struct Entry
        {
            ViewManifest manifest;
            std::shared_ptr<BrowserHostClient> host;
            std::shared_ptr<SurfaceFrames> gpu;
            std::future<void> retirement;
            BrowserHostRecovery recovery;
            std::size_t demand{};
            bool loaded{}, failed{};
        };
        struct Request
        {
            API::Surfaces::Interaction token{};
            API::Surfaces::Handle lease{};
            API::Surfaces::InteractionCallback callback{};
            void* context{};
            std::chrono::steady_clock::time_point requested;
            bool started{};
            std::string view;
            std::uint64_t cancelGeneration{};
        };
        std::shared_ptr<Entry> Find(API::Surfaces::Handle) const;
        void Start(const std::shared_ptr<Entry>&);
        void Navigate(Entry&);
        mutable std::mutex m_mutex;
        std::vector<std::shared_ptr<Entry>> m_entries;
        std::unordered_map<API::Surfaces::Handle, std::shared_ptr<Entry>> m_leases;
        std::uint64_t m_next{1};
        API::Surfaces::Tick m_tick{};
        void* m_tickContext{};
        std::deque<Request> m_requests;
        std::optional<Request> m_interaction;
        std::atomic<std::uint64_t> m_interactionToken{};
        std::atomic<std::shared_ptr<BrowserHostClient>> m_input;
        std::atomic<std::uint64_t> m_cancelGeneration{};
        unsigned m_neutral{};
        MessageBridge* m_bridge{};
        bool m_developerMode{};
        std::function<std::optional<std::string>()> m_language;
    };
}
