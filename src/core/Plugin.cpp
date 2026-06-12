#include "core/Plugin.h"

#include "core/Version.h"
#include "runtime/Runtime.h"

namespace SWUI::Plugin
{
	namespace
	{
		// SFSE broadcast messages. Logged for now; kPostPostDataLoad is the
		// earliest point where game data is fully available and the most
		// likely future home for anything that needs loaded forms.
		void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg) {
				return;
			}
			switch (a_msg->type) {
				case SFSE::MessagingInterface::kPostLoad:
					REX::INFO("Plugin: SFSE message kPostLoad");
					break;
				case SFSE::MessagingInterface::kPostPostLoad:
					REX::INFO("Plugin: SFSE message kPostPostLoad");
					break;
				case SFSE::MessagingInterface::kPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostDataLoad");
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostPostDataLoad");
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
		// can fail belongs in OnLoad where failure is observable and clean.
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

		// NOTE: SFSE has no shutdown/unload callback. Runtime::Shutdown() is
		// implemented but is not reachable today; OS teardown at process exit
		// is what actually ends us. Keep all state safe against that.
		return true;
	}
}
