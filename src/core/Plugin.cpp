#include "core/Plugin.h"

#include "api/PapyrusApi.h"
#include "core/Version.h"
#include "input/FocusMenu.h"
#include "input/MainThreadMenuPump.h"
#include "input/MenuEventSink.h"
#include "input/OverlayInputHook.h"
#include "input/UiLayoutGuard.h"
#include "runtime/Runtime.h"

namespace OSFUI::Plugin
{
	namespace
	{
		// Per-frame tick source. SFSE permanent tasks run every frame on the
		// main thread without being deleted (sfse/PluginAPI.h); SFSE owns the
		// underlying game hook, so no addresses live on our side.
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
				const auto now = std::chrono::steady_clock::now();
				double dt = 0.0;
				if (_last) {
					dt = std::chrono::duration<double>(now - *_last).count();
				}
				_last = now;
				// Clamp: the game pauses on focus loss and this task stalls
				// with it; don't feed a huge step on resume.
				dt = std::clamp(dt, 0.0, 0.1);

				++_ticks;
				if (_ticks == 1) {
					// One-shot boot marker: proves the SFSE task pump reached
					// us. No periodic heartbeat — it flooded the log at
					// menu-uncapped frame rates.
					REX::INFO("FrameTick: first per-frame task received from SFSE TaskInterface");
				}

				Runtime::Get().Tick(dt);
			}

			void Destroy() override
			{
				// Permanent task: it is never destroyed.
			}

		private:
			std::optional<std::chrono::steady_clock::time_point> _last;
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
					REX::DEBUG("Plugin: SFSE message kPostLoad");
					break;
				case SFSE::MessagingInterface::kPostPostLoad:
					REX::DEBUG("Plugin: SFSE message kPostPostLoad");
					break;
				case SFSE::MessagingInterface::kPostDataLoad:
					REX::DEBUG("Plugin: SFSE message kPostDataLoad");
					// GameVM exists from here. Bind the Papyrus natives even
					// when the overlay is disabled — scripts then read schema
					// defaults through the mirror instead of hard-failing.
					API::Papyrus::Install();
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::DEBUG("Plugin: SFSE message kPostPostDataLoad");
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
						// PauseMenuEntry's Scaleform access and the menu-state
						// snapshots Runtime reads run on the thread that owns the
						// AS3 VM. SFSE tasks run on a render-graph worker, NOT the
						// main thread (crash-stack-proven 2026-07-23) — without
						// this, per-tick RE::UI access races the engine.
						MainThreadMenuPump::Install();
						// Register the engine-built IMenu so the engine enters
						// menu mode with the overlay (registration, open and
						// long-session survival verified on 1.16.244; see
						// input/FocusMenu.h). It is only opened (UIMessageQueue
						// kShow) when the overlay becomes visible, from
						// Runtime's main-thread tick.
						if (Runtime::Get().GetConfig().focusMenu) {
							REX::DEBUG("Plugin: focusMenu on — registering OSFUI_FocusMenu");
							FocusMenu::Register();
						}
						// The WndProc subclass is the only input path: it drives
						// the toggle key and, while capturing, consumes
						// keyboard/mouse and routes them into the overlay.
						if (Runtime::Get().GetConfig().inputSource == "ui") {
							OverlayInputHook::Install();
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

		if (!Runtime::Get().Initialize()) {
			REX::ERROR("{}: Runtime initialization failed", kPluginName);
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

		// NOTE: SFSE has no shutdown/unload callback. Runtime::Shutdown() is
		// implemented but unreachable today; OS teardown at process exit is
		// what actually ends us, so all state must be safe against that.
		return true;
	}
}
