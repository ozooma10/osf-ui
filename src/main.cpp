#include "v2/Events/Events.h"
#include "v2/Scripts/Papyrus.h"

#include "v2/Runtime/ViewManifest.h"
#include "v2/Runtime/ViewDiscovery.h"
#include "v2/Runtime/ViewCatalog.h"

namespace
{
	std::filesystem::path DefaultViewsDirectory()
	{
		return std::filesystem::path{
			REX::FModule::GetCurrentModule().GetFileName()
		}.parent_path() / L"OSFUI" / L"Views";
	}

	Runtime::ViewCatalog& InstalledViews()
	{
		static Runtime::ViewCatalog catalog;
		return catalog;
	}

	void DiscoverInstalledViews()
	{
		auto result = Runtime::DiscoverViews(DefaultViewsDirectory());
		for(const auto& issue : result.issues) {
			REX::ERROR("View discovery failed at '{}': {}", issue.path.string(), issue.message);
		}

		InstalledViews().Replace(std::move(result.views));

		for(const auto& view : InstalledViews().All()) {
			REX::INFO("Discovered view '{}' title='{}' entry='{}' size={}x{} transparent={}", view.id, view.title, view.entry, view.width, view.height, view.transparent);
		}

		if (const auto* settings = InstalledViews().Find("osfui/settings")) {
			REX::INFO("Catalog lookup succeeded: '{}' -> '{}'", settings->id, settings->rootDirectory.string());
		} else {
			REX::ERROR("Catalog lookup failed: built-in view 'osfui/settings' is unavailable");
		}

		REX::INFO("View discovery completed: {} valid, {} invalid",  InstalledViews().All().size(), result.issues.size());
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

				DiscoverInstalledViews();
				if(!Papyrus::RegisterFunctions()) {
					REX::ERROR("Plugin: Papyrus natives are unavailable");
				}

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
				if(!Events::Register()) {
					REX::ERROR("Plugin: menu event integration is unavailable");
				}
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
