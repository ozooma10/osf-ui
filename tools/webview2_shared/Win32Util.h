#pragma once

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#  define NOGDI
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace osfui::win32
{
	[[nodiscard]] inline std::wstring ToWide(std::string_view a_text)
	{
		if (a_text.empty()) return {};
		const auto size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
		if (size <= 0) return {};
		std::wstring out(static_cast<std::size_t>(size), L'\0');
		if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), size) <= 0) return {};
		return out;
	}

	[[nodiscard]] inline std::string ToUtf8(std::wstring_view a_text)
	{
		if (a_text.empty()) return {};
		const auto size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0) return {};
		std::string out(static_cast<std::size_t>(size), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), size, nullptr, nullptr) <= 0) return {};
		return out;
	}

	[[nodiscard]] inline bool IsProcessElevated()
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
		TOKEN_ELEVATION elevation{};
		DWORD size = 0;
		const bool ok = ::GetTokenInformation(
			token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
		::CloseHandle(token);
		return ok && elevation.TokenIsElevated != 0;
	}

	template <class T>
	inline void SafeRelease(T*& a_ptr)
	{
		if (!a_ptr) return;
		a_ptr->Release();
		a_ptr = nullptr;
	}
}
