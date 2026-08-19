#pragma once


#include <cstdint>
#include <map>

#include "RE/F/FormTypes.h"

namespace RE
{
	class TESForm
	{
	public:
		virtual ~TESForm() = default;

		virtual const char* GetFormEditorID() const { return ""; }

		[[nodiscard]] std::uint32_t GetFormID() const noexcept { return formID; }
		[[nodiscard]] FormType      GetFormType() const noexcept { return formType; }

		[[nodiscard]] static TESForm* LookupByID(std::uint32_t a_formID)
		{
			auto&      reg = Registry();
			const auto it = reg.find(a_formID);
			return it == reg.end() ? nullptr : it->second;
		}

		[[nodiscard]] static std::map<std::uint32_t, TESForm*>& Registry()
		{
			static std::map<std::uint32_t, TESForm*> registry;
			return registry;
		}

		std::uint32_t formID{ 0 };
		FormType      formType{ FormType::kNONE };
	};
}
