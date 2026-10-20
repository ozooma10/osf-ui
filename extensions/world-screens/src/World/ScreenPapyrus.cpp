#include "World/ScreenPapyrus.h"
#include "World/ScreenBindings.h"
#include "World/ScreenBindingRuntime.h"
#include "Core/StringUtil.h"
#include "RE/B/BSScriptUtil.h"
#include "Composite/D3D12Prologue.h"
#include <objbase.h>
#include <charconv>
namespace OSFUI::ScreenPapyrus
{
    namespace
    {
        using PapVM=RE::BSScript::IVirtualMachine;
        using StringUtil::ToLowerAscii;
		// The process UUID prevents a saved Papyrus string from aliasing a new
		// process's token. Registry tokens themselves never repeat across loads.
		const std::string& ScreenEpoch()
		{
			static const auto epoch=[] {
                ::GUID id{};
				if (FAILED(CoCreateGuid(&id))) return std::string{};
				return std::format("{:08x}{:04x}{:04x}{:016x}:",id.Data1,id.Data2,id.Data3,std::bit_cast<std::uint64_t>(id.Data4));
			}();
			return epoch;
		}
		RE::BSFixedString BindScreen(PapVM&, std::uint32_t, std::monostate, RE::TESObjectREFR* reference, RE::BSFixedString view)
		{
			if (!reference || !ScreenBindingRuntime::Enabled() || ScreenEpoch().empty()) return {};
			const auto token=ScreenBindings::Bind(reference->GetFormID(),view.c_str());
			return token ? RE::BSFixedString((ScreenEpoch()+std::to_string(token)).c_str()) : RE::BSFixedString{};
		}
		bool UnbindScreen(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString handle)
		{
			const auto text=ToLowerAscii(handle.c_str());
			if (ScreenEpoch().empty() || !text.starts_with(ScreenEpoch())) return false;
			std::uint32_t token{};
			const auto first=text.data()+ScreenEpoch().size(), last=text.data()+text.size();
			const auto parsed=std::from_chars(first,last,token);
			return parsed.ec==std::errc{} && parsed.ptr==last && ScreenBindings::Unbind(token);
		}

    }
    void BindNatives(RE::BSScript::IVirtualMachine& vm)
    {
        vm.BindNativeMethod("OSFUIWorldScreens", "BindScreen", &BindScreen, true, false);
        vm.BindNativeMethod("OSFUIWorldScreens", "UnbindScreen", &UnbindScreen, true, false);
    }
}
