#include "Core/Plugin.h"

#include "API/PapyrusApi.h"
#include "Core/NativeMainThreadQueue.h"
#include "Core/Version.h"
#include "Runtime/Runtime.h"

namespace OSFUI::Plugin
{
	namespace
	{
		// Per-frame tick source. SFSE permanent tasks drain on rotating
		// render-graph workers, not the game main thread. This delegate is only a
		// lightweight producer: it coalesces and posts Runtime::Tick through the
		// engine's BSService queue, whose normal drain is on the main thread.
		//
		// There is no RemovePermanentTask API, so this delegate must have
		// process lifetime (function-local static below), Destroy() must be a
		// no-op, and Run() must stay cheap: it runs under SFSE's task queue
		// lock every frame.
		class FrameTickTask final : public SFSE::ITaskDelegate
		{
		public:
			void Run() override
			{
				if (_faulted.load(std::memory_order_acquire)) {
					return;
				}
				++_ticks;
				if (_ticks == 1) {
					// One-shot boot marker: proves the SFSE task pump reached
					// us. No periodic heartbeat — it flooded the log at
					// menu-uncapped frame rates.
					REX::INFO("FrameTick: first per-frame task received from SFSE TaskInterface");
				}

				// At most one tick may be queued or running. If the main thread
				// stalls, shed redundant worker notifications rather than build
				// an unbounded queue of stale frames.
				if (_tickPending.exchange(true, std::memory_order_acq_rel)) {
					return;
				}
				const auto result = NativeMainThreadQueue::Post(
					[this]() { RunTickOnMain(); },
					"FrameTick.RuntimeTick",
					[this]() { _tickPending.store(false, std::memory_order_release); });
				if (result == NativeMainThreadQueue::PostResult::Unavailable) {
					// Queueing can be disabled during early boot. This wrapper
					// refuses BSService's off-main inline fallback;
					// retry on the next SFSE frame instead.
					_tickPending.store(false, std::memory_order_release);
				}
			}

			void Destroy() override
			{
				// Permanent task: it is never destroyed.
			}

		private:
			void RunTickOnMain()
			{
				const auto now = std::chrono::steady_clock::now();
				double dt = 0.0;
				if (_lastMainTick) {
					dt = std::chrono::duration<double>(now - *_lastMainTick).count();
				}
				_lastMainTick = now;
				// Clamp: the game pauses on focus loss and this task stalls with
				// it; don't feed a huge step on resume.
				dt = std::clamp(dt, 0.0, 0.1);

				try {
					Runtime::Get().Tick(dt);
				} catch (const std::exception& e) {
					_faulted.store(true, std::memory_order_release);
					REX::ERROR("FrameTick: Runtime::Tick threw '{}'; disabling further UI ticks "
							   "to contain the failure",
						e.what());
				} catch (...) {
					_faulted.store(true, std::memory_order_release);
					REX::ERROR("FrameTick: Runtime::Tick threw an unknown exception; "
							   "disabling further UI ticks to contain the failure");
				}
				_tickPending.store(false, std::memory_order_release);
			}

			std::atomic_bool                                      _tickPending{ false };
			std::atomic_bool                                      _faulted{ false };
			std::optional<std::chrono::steady_clock::time_point> _lastMainTick;
			std::uint64_t                                        _ticks{ 0 };
		};
		// SFSE broadcast messages. These callbacks are not guaranteed to share the
		// BSService-backed Runtime::Tick thread; thread-bound work must be handed
		// off rather than performed inline.
		void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg) {
				return;
			}
			switch (a_msg->type) {
				case SFSE::MessagingInterface::kPostLoad:
					REX::INFO("Plugin: SFSE message kPostLoad");
					if (Runtime::Get().GetConfig().enabled) {
						Runtime::Get().InstallOverlayDrawPath();
					}
					break;
				case SFSE::MessagingInterface::kPostPostLoad:
					REX::INFO("Plugin: SFSE message kPostPostLoad");
					break;
				case SFSE::MessagingInterface::kPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostDataLoad");
					// GameVM and ControlMap exist from here, but this callback need not
					// share the owning thread. The enabled runtime binds Papyrus and copies
					// ControlMap on its next main-thread tick. With the runtime disabled
					// there is no permanent tick, so queue the promised Papyrus-only setup
					// directly through the same BSService main-thread queue.
					if (Runtime::Get().GetConfig().enabled) {
						Runtime::Get().OnDataLoaded();
					} else if (NativeMainThreadQueue::Post(
							   [] { API::Papyrus::Install(); }, "Plugin.InstallPapyrus") ==
						   NativeMainThreadQueue::PostResult::Unavailable) {
						REX::ERROR("Plugin: could not queue disabled-runtime Papyrus binding on the main thread; "
							"OSFUI.* natives remain unavailable");
					}
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostPostDataLoad");
					// UI singletons now exist. Installation still belongs to the proven
					// main-thread tick, not this lifecycle callback.
					if (Runtime::Get().GetConfig().enabled) Runtime::Get().OnPostDataLoaded();
					break;
				default:
					REX::DEBUG("Plugin: SFSE message type {}", a_msg->type);
					break;
			}
		}
	}

	bool OnPreLoad()
	{
		// Keep preload minimal: no filesystem, no game objects. Anything that
		// can fail belongs in OnLoad, where failure is observable.
		REX::INFO("{} v{}: preload entered", kPluginName, kOsfuiReleaseVersion);
		return true;
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
			REX::ERROR("{}: Runtime initialization threw '{}'; plugin load aborted",
				kPluginName, e.what());
			return false;
		} catch (...) {
			REX::ERROR("{}: Runtime initialization threw an unknown exception; plugin load aborted",
				kPluginName);
			return false;
		}

		if (Runtime::Get().GetConfig().enabled) {
			if (const auto* tasks = SFSE::GetTaskInterface();
				tasks && tasks->Version() >= SFSE::TaskInterface::kVersion) {
				static FrameTickTask s_frameTick;
				tasks->AddPermanentTask(&s_frameTick);
				REX::INFO("Plugin: per-frame tick registered via SFSE TaskInterface (v{})", tasks->Version());
			} else {
				REX::ERROR("Plugin: SFSE TaskInterface unavailable; Runtime::Tick will never run "
						   "(overlay stays dormant, plugin remains loaded)");
			}
		}
		// SFSE has no shutdown/unload callback. OS teardown at process exit ends
		// the process-owned runtime and browser-host connection.
		return true;
	}
}
