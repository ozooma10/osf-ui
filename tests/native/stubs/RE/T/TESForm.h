#pragma once

// Desktop-test stand-in for CommonLibSF's TESForm: identity fields plus a
// process-global test registry behind LookupByID, so PapyrusApi.cpp's form
// serialization and GetFormById resolvers run UNCHANGED against test forms
// (see papyrus_form_tests.cpp). Never used by the real plugin build.

#include <cstdint>
#include <map>

#include "RE/F/FormTypes.h"

namespace RE
{
	class TESForm
	{
	public:
		virtual ~TESForm() = default;

		// Real signature is a virtual returning the (usually empty-at-runtime)
		// editor id; the default mirrors that best-effort behavior.
		virtual const char* GetFormEditorID() const { return ""; }

		[[nodiscard]] std::uint32_t GetFormID() const noexcept { return formID; }
		[[nodiscard]] FormType      GetFormType() const noexcept { return formType; }

		[[nodiscard]] static TESForm* LookupByID(std::uint32_t a_formID)
		{
			auto&      reg = Registry();
			const auto it = reg.find(a_formID);
			return it == reg.end() ? nullptr : it->second;
		}

		// Test seam: the process-global form table LookupByID resolves from.
		// Tests insert/erase entries directly (erase models a deleted form).
		[[nodiscard]] static std::map<std::uint32_t, TESForm*>& Registry()
		{
			static std::map<std::uint32_t, TESForm*> registry;
			return registry;
		}

		std::uint32_t formID{ 0 };
		FormType      formType{ FormType::kNONE };
	};
}
