#pragma once

namespace REX::W32
{
	using HMODULE = void*;

	inline HMODULE GetModuleHandleW(const wchar_t*) noexcept { return nullptr; }
	inline void* GetProcAddress(HMODULE, const char*) noexcept { return nullptr; }
}
