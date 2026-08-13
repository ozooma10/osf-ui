#include "papyrus_tests.h"

#ifdef NDEBUG
#	undef NDEBUG
#endif
#include <cassert>

#include "v2/Scripts/Papyrus.h"

#include "RE/B/BSScriptUtil.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace
{
	using PapyrusVM = RE::BSScript::IVirtualMachine;
	using ViewNative = bool (*)(
		PapyrusVM&,
		std::uint32_t,
		std::monostate,
		RE::BSFixedString);
	using VersionNative = std::int32_t (*)(
		PapyrusVM&,
		std::uint32_t,
		std::monostate);
	using VersionStringNative = RE::BSFixedString (*)(
		PapyrusVM&,
		std::uint32_t,
		std::monostate);

	std::string g_lastOpenedViewId;
	std::string g_lastClosedViewId;
	bool g_openResult{ false };
	bool g_closeResult{ false };

	bool RecordOpen(std::string a_viewId)
	{
		g_lastOpenedViewId = std::move(a_viewId);
		return g_openResult;
	}

	bool RecordClose(std::string a_viewId)
	{
		g_lastClosedViewId = std::move(a_viewId);
		return g_closeResult;
	}

	void TestPapyrusRegistersPlatformNatives()
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		vm->natives.clear();

		assert(Papyrus::RegisterFunctions());

		const auto getVersion = vm->GetNative<VersionNative>("GetVersion");
		const auto getVersionString =
			vm->GetNative<VersionStringNative>("GetVersionString");
		assert(vm->GetNative<ViewNative>("OpenMenu") != nullptr);
		assert(vm->GetNative<ViewNative>("CloseMenu") != nullptr);

		assert(getVersion(*vm, 0, {}) == 20000);
		assert(std::string{ getVersionString(*vm, 0, {}).c_str() } == "2.0.0");
	}

	void TestPapyrusViewNativesForwardNormalizedRequests()
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		const auto openMenu = vm->GetNative<ViewNative>("OpenMenu");
		const auto closeMenu = vm->GetNative<ViewNative>("CloseMenu");

		Papyrus::SetViewRequestHandlers(nullptr, nullptr);
		assert(!openMenu(*vm, 0, {}, RE::BSFixedString{ "OSFUI/Settings" }));
		assert(!closeMenu(*vm, 0, {}, RE::BSFixedString{ "OSFUI/Settings" }));

		g_lastOpenedViewId.clear();
		g_lastClosedViewId.clear();
		g_openResult = true;
		g_closeResult = false;
		Papyrus::SetViewRequestHandlers(&RecordOpen, &RecordClose);

		assert(openMenu(*vm, 0, {}, RE::BSFixedString{ "OSFUI/Settings" }));
		assert(g_lastOpenedViewId == "osfui/settings");

		assert(!closeMenu(
			*vm,
			0,
			{},
			RE::BSFixedString{ "Author.Mod/MyView" }));
		assert(g_lastClosedViewId == "author.mod/myview");

		g_openResult = false;
		g_closeResult = true;
		assert(!openMenu(*vm, 0, {}, RE::BSFixedString{ "Known/View" }));
		assert(closeMenu(*vm, 0, {}, RE::BSFixedString{ "Known/View" }));

		Papyrus::SetViewRequestHandlers(nullptr, nullptr);
	}
}

void RunPapyrusTests()
{
	TestPapyrusRegistersPlatformNatives();
	TestPapyrusViewNativesForwardNormalizedRequests();
}
