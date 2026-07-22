#pragma once

// Desktop-test stand-in for CommonLibSF's TESFullName form component.
// Polymorphic (virtual dtor) so the stub starfield_cast — a plain
// dynamic_cast — can cross-cast to it from a TESForm-derived test form that
// multiple-inherits it, mirroring how real forms carry the component.

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
