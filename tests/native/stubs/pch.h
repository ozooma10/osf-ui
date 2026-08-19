#pragma once


#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef NOGDI
#		define NOGDI
#	endif
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace REX
{
	namespace test
	{
		inline std::vector<std::string>& Entries()
		{
			static std::vector<std::string> entries;
			return entries;
		}

		inline void Log(std::string_view a_level, std::string a_message)
		{
			std::fprintf(stderr, "    [%.*s] %s\n", static_cast<int>(a_level.size()), a_level.data(), a_message.c_str());
			Entries().push_back(std::format("{}: {}", a_level, a_message));
		}
	}

// Mirrors CommonLibSF's REX::INFO(...) CTAD-struct call syntax.
#define OSFUI_TEST_LOG_LEVEL(NAME)                                              \
	template <class... T>                                                       \
	struct NAME                                                                 \
	{                                                                           \
		explicit NAME(std::format_string<T...> a_fmt, T&&... a_args)            \
		{                                                                       \
			test::Log(#NAME, std::format(a_fmt, std::forward<T>(a_args)...));   \
		}                                                                       \
	};                                                                          \
	template <class... T>                                                       \
	NAME(std::format_string<T...>, T&&...) -> NAME<T...>;

	OSFUI_TEST_LOG_LEVEL(DEBUG)
	OSFUI_TEST_LOG_LEVEL(INFO)
	OSFUI_TEST_LOG_LEVEL(WARN)
	OSFUI_TEST_LOG_LEVEL(ERROR)

#undef OSFUI_TEST_LOG_LEVEL
}
