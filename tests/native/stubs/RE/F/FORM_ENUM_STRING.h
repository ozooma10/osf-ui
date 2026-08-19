#pragma once


#include <array>
#include <span>

#include "RE/F/FormTypes.h"

namespace RE
{
	struct FORM_ENUM_STRING
	{
	public:
		[[nodiscard]] static std::span<FORM_ENUM_STRING, 215> GetFormEnumString()
		{
			static std::array<FORM_ENUM_STRING, 215> table = [] {
				std::array<FORM_ENUM_STRING, 215> t{};
				t[0x04] = { "KYWD", FormType::kKYWD };
				t[0x30] = { "WEAP", FormType::kWEAP };
				t[0x69] = { "FLST", FormType::kFLST };
				return t;
			}();
			return std::span<FORM_ENUM_STRING, 215>{ table };
		}

		// members
		const char* formString{ nullptr };
		FormType    formType{ FormType::kNONE };
	};
}
