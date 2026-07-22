#pragma once

// Desktop-test stand-in for CommonLibSF's FORM_ENUM_STRING: the real one is a
// span over a relocated game-memory table mapping FormType -> the 4-char
// record signature. Here it is a static table with the few rows the tests
// serialize; every other slot is {nullptr, kNONE}, which exercises the
// numeric-fallback path for unmapped types.

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
