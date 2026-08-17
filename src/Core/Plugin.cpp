#include "Core/Plugin.h"

#include "API/PapyrusApi.h"
#include "Core/NativeMainThreadQueue.h"
#include "Core/Version.h"
#include "Runtime/Runtime.h"

namespace OSFUI::Plugin
{
	namespace
	{
		class FrameTickTask final : public SFSE::ITaskDelegate
		{
		public:
			void Run() override
			{
				// At most one tick may be queued or running. If main thread stalls, shed redundant notifications
				if (m_tickPending.exchange(true, std::memory_order_acq_rel)) {
					return;
				}
				const auto result = NativeMainThreadQueue::Post([this]() { RunTickOnMain(); }, "FrameTick.RuntimeTick", [this]() { m_tickPending.store(false, std::memory_order_release); });
				if (result == NativeMainThreadQueue::PostResult::Unavailable) {
					m_tickPending.store(false, std::memory_order_release);
				}
			}

			void Destroy() override {}

		private:
			void RunTickOnMain()
			{
				const auto now = std::chrono::steady_clock::now();
				double dt = 0.0;
				if (m_lastMainTick) {
					dt = std::chrono::duration<double>(now - *m_lastMainTick).count();
				}
				m_lastMainTick = now;
				dt = std::clamp(dt, 0.0, 0.1);

				try {
					Runtime::Get().Tick(dt);
				} catch (const std::exception& e) {
					m_faulted.store(true, std::memory_order_release);
					REX::ERROR("FrameTick: Runtime::Tick threw '{}'; disabling further UI ticks to contain the failure", e.what());
				} catch (...) {
					m_faulted.store(true, std::memory_order_release);
					REX::ERROR("FrameTick: Runtime::Tick threw an unknown exception; disabling further UI ticks to contain the failure");
				}
				m_tickPending.store(false, std::memory_order_release);
			}

			std::atomic_bool                                      m_tickPending{ false };
			std::optional<std::chrono::steady_clock::time_point>  m_lastMainTick;
		};

		void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg) {
				return;
			}
			switch (a_msg->type) {
				case SFSE::MessagingInterface::kPostLoad:
					REX::INFO("Plugin: SFSE message kPostLoad");
					Runtime::Get().InstallOverlayDrawPath();
					break;
				case SFSE::MessagingInterface::kPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostDataLoad");
					Runtime::Get().OnDataLoaded();
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostPostDataLoad");
					Runtime::Get().OnPostDataLoaded();
					break;
			}
		}
	}

	bool OnLoad()
	{
		REX::INFO("{} v{}: load entered", kPluginName, kOsfuiReleaseVersion);

		if (const auto* messaging = SFSE::GetMessagingInterface()) {
			if (!messaging->RegisterListener(OnSFSEMessage)) {
				REX::WARN("Plugin: failed to register SFSE message listener (non-fatal)");
			}
		}

		try {
			if (!Runtime::Get().Initialize()) {
				REX::ERROR("{}: Runtime initialization failed", kPluginName);
				return false;
			}
		} catch (const std::exception& e) {
			REX::ERROR("{}: Runtime initialization threw '{}'; plugin load aborted", kPluginName, e.what());
			return false;
		} catch (...) {
			REX::ERROR("{}: Runtime initialization threw an unknown exception; plugin load aborted", kPluginName);
			return false;
		}

		static FrameTickTask s_frameTick;
		tasks->AddPermanentTask(&s_frameTick);
		return true;
	}
}
