#include "Events/Events.h"
#include "Scripts/Papyrus.h"

namespace
{
    void OnDataLoaded() noexcept
    {
        REX::INFO("=== OSF Identity: data loaded ===");
        // NpcAppearfance::Initialize();
        REX::INFO("=== OSF Identity: ready ===");
    }

	void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
			case SFSE::MessagingInterface::kPostLoad:
				REX::INFO("Plugin: SFSE message kPostLoad");
				// if (Runtime::Get().GetConfig().enabled) {
				// 	Runtime::Get().InstallOverlayDrawPath();
				// }
				break;
			case SFSE::MessagingInterface::kPostDataLoad:
				REX::INFO("Plugin: SFSE message kPostDataLoad");

				Papyrus::RegisterFunctions();

				// GameVM and ControlMap exist from here, but this callback need not
				// share the owning thread. The enabled runtime binds Papyrus and copies
				// ControlMap on its next main-thread tick. With the runtime disabled
				// there is no permanent tick, so queue the promised Papyrus-only setup
				// directly through the same BSService main-thread queue.
				// if (Runtime::Get().GetConfig().enabled) {
				// 	Runtime::Get().OnDataLoaded();
				// } else if (!TryQueueMainThread([] { API::Papyrus::Install(); })) {
				// 	REX::ERROR("Plugin: could not queue disabled-runtime Papyrus binding on the main thread; "
				// 		"OSFUI.* natives remain unavailable");
				// }
				break;
			case SFSE::MessagingInterface::kPostPostDataLoad:
				REX::INFO("Plugin: SFSE message kPostPostDataLoad");
				Events::Register();
				break;
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 1024 });

	SFSE::GetMessagingInterface()->RegisterListener(OnSFSEMessage);
	return true;
	// return OSFUI::Plugin::OnLoad();
}
