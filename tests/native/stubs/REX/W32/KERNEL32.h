#pragma once

namespace REX::W32
{
	using HMODULE = void*;
	namespace test
	{
		inline HMODULE (*moduleLookup)(const wchar_t*) noexcept = nullptr;
		inline void* (*procLookup)(HMODULE, const char*) noexcept = nullptr;
	}
	inline HMODULE GetModuleHandleW(const wchar_t* name) noexcept
	{
		return test::moduleLookup ? test::moduleLookup(name) : nullptr;
	}
	inline void* GetProcAddress(HMODULE module, const char* name) noexcept
	{
		return test::procLookup ? test::procLookup(module, name) : nullptr;
	}
}
