#include "core/Plugin.h"

#include "api/PapyrusApi.h"
#include "core/Version.h"
#include "input/FocusMenu.h"
#include "input/MainThreadMenuPump.h"
#include "input/MenuEventSink.h"
#include "input/OverlayInputHook.h"
#include "input/UiLayoutGuard.h"
#include "runtime/Runtime.h"

#include "RE/B/BSService.h"

namespace OSFUI::Plugin
{
	namespace
	{
		// Queue-only counterpart to BSService::TaskQueue::AddTask. QueueTask
		// leaves the caller's reference intact when it would use an inline
		// fallback; destroy that delegate without running it so worker-owned
		// producers can retry rather than violating main-thread affinity.
		template <class Fn>
			requires(std::invocable<std::remove_cvref_t<Fn>&>)
		[[nodiscard]] bool TryQueueMainThread(Fn&& a_fn)
		{
			auto* queue = RE::BSService::TaskQueue::GetSingleton();
			if (!queue) {
				return false;
			}
			RE::BSService::QueuedDelegate* task =
				new RE::BSService::detail::QueuedFunctorDelegate<std::remove_cvref_t<Fn>>(
					std::forward<Fn>(a_fn));
			queue->QueueTask(task);
			if (task) {
				delete task;
				return false;
			}
			return true;
		}

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
				if (!TryQueueMainThread([this]() { RunTickOnMain(); })) {
					// Queueing can be disabled during early boot. The helper
					// deliberately refuses BSService's off-main inline fallback;
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
		// SFSE broadcast messages. kPostPostDataLoad is the earliest point
		// where game data is fully available, so anything needing loaded forms
		// belongs there.
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
					// GameVM exists from here. Bind the Papyrus natives even
					// when the overlay is disabled — scripts then read schema
					// defaults through the mirror instead of hard-failing.
					API::Papyrus::Install();
					// ControlMap is fully populated here, before the player ever opens
					// the Controls panel. The provider copies its live rows and active
					// context stack into OSF UI-owned snapshots.
					Runtime::Get().OnDataLoaded();
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostPostDataLoad");
					// Earliest point game singletons (the UI event source) are
					// treated as safely constructed.
					if (Runtime::Get().GetConfig().enabled) {
						if (!UiLayoutGuard::VerifyUiLayout()) {
							REX::ERROR("Plugin: UI layout guard failed; skipping ALL UI integration "
									   "(MenuEventSink + FocusMenu stay uninstalled, overlay toggle inert)");
							break;
						}
						MenuEventSink::Install();
						// Engine-UI work station: hooks the main-loop UI update so
						// PauseMenuEntry's Scaleform access runs not only on the
						// main thread, but specifically after active movies have
						// advanced and nothing else is inside the AS3 VM.
						MainThreadMenuPump::Install();
						// Register the engine-built IMenu so the engine enters
						// menu mode with the overlay (registration, open and
						// long-session survival verified on 1.16.244; see
						// input/FocusMenu.h). It is only opened (UIMessageQueue
						// kShow) when the overlay becomes visible, from
						// Runtime's main-thread tick.
						if (Runtime::Get().GetConfig().focusMenu) {
							REX::INFO("Plugin: focusMenu on — registering OSFUI_FocusMenu");
							FocusMenu::Register();
						}
						// The WndProc subclass is the only input path: it drives
						// the toggle key and, while capturing, consumes
						// keyboard/mouse and routes them into the overlay.
						if (Runtime::Get().GetConfig().inputSource == "ui") {
							[[maybe_unused]] const bool inputInstalled = OverlayInputHook::Install();
						} else {
							REX::INFO("Plugin: inputSource=none; input hook not installed (toggle key inert)");
						}
					}
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
		REX::INFO("{} v{}: preload entered", kPluginName, kPluginVersion);
		return true;
	}

	bool OnLoad()
	{
		REX::INFO("{} v{}: load entered", kPluginName, kPluginVersion);

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
		// the process-owned runtime and helper connection.
		return true;
	}
}
