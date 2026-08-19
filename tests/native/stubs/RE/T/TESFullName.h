#pragma once


#include <string>

namespace RE
{
	class TESFullName
	{
	public:
		virtual ~TESFullName() = default;

		virtual const char* GetFullName() const { return fullName.c_str(); }

		std::string fullName;
	};
}
