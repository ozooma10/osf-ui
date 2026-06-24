// PrismaUI SF — example consumer plugin (sketch).
//
// This is a *reference sketch*, not a buildable target in this repo: a real
// consumer is its own SFSE/CommonLibSF plugin with its own build. It shows the
// whole API surface end-to-end. Copy sdk/PrismaUI_API.h into your project and
// adapt this pattern.
//
// Ship your view's HTML/CSS/JS under:
//   <game>/Data/SFSE/Plugins/PrismaUI/views/MyMod/index.html

#include "PrismaUI_API.h"  // copied from PrismaUI SF's sdk/ (or SFSE/Plugins/PrismaUI/api/)

#include <SFSE/SFSE.h>     // your script-extender headers

namespace
{
	PRISMA_UI_API::IVPrismaUI2* g_prisma = nullptr;  // request V2 for console capture
	PrismaView                  g_view = 0;

	// JS -> native: window.onSave(jsonString) in the page calls this (main thread).
	void OnSave(const char* a_data)
	{
		SFSE::log::info("[example] page saved: {}", a_data ? a_data : "");
	}

	// native <- JS result: the value of the JS expression we Invoke below.
	void OnGotTitle(const char* a_result)
	{
		SFSE::log::info("[example] document.title = {}", a_result ? a_result : "");
	}

	// console.* from the view (V2).
	void OnConsole(PrismaView, PRISMA_UI_API::ConsoleMessageLevel a_level, const char* a_msg)
	{
		SFSE::log::info("[example] view console (level {}): {}",
			static_cast<int>(a_level), a_msg ? a_msg : "");
	}

	// Fires on the game main thread once the view's DOM is ready.
	void OnDomReady(PrismaView a_view)
	{
		g_prisma->RegisterConsoleCallback(a_view, OnConsole);              // V2
		g_prisma->RegisterJSListener(a_view, "onSave", OnSave);           // exposes window.onSave(str)
		g_prisma->Invoke(a_view, "initUI()");                            // run page setup
		g_prisma->Invoke(a_view, "document.title", OnGotTitle);          // read a value back
		g_prisma->InteropCall(a_view, "setUser", "Player");             // window.setUser("Player")
		g_prisma->Focus(a_view, /*pauseGame*/ true, /*disableFocusMenu*/ false);
	}
}

// Request the API after the script extender finishes loading plugins.
void OnPostLoad()
{
	g_prisma = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
	if (!g_prisma) {
		SFSE::log::warn("[example] PrismaUI SF (V2) not available — is it installed?");
		return;
	}
	g_view = g_prisma->CreateView("MyMod/index.html", OnDomReady);
	if (!g_view) {
		SFSE::log::error("[example] CreateView failed");
	}
}

// Example teardown (e.g. on your own toggle/hotkey):
//   g_prisma->Unfocus(g_view);
//   g_prisma->Hide(g_view);     // or Show(g_view) to bring it back
//   g_prisma->Destroy(g_view);  // when done entirely
