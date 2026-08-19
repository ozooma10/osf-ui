#pragma once

#include <string>

namespace osfui::wv2
{
	enum class LaunchMethod
	{
		kNone,
		kExplorer,
		kTaskScheduler,
		kDirect,
	};

	struct LaunchResult
	{
		bool         ok{ false };
		LaunchMethod method{ LaunchMethod::kNone };
		std::string  detail;  // per-attempt diagnostics (also on success)
	};

	[[nodiscard]] const char* LaunchMethodName(LaunchMethod a_method);

	[[nodiscard]] LaunchResult LaunchDetached(const std::wstring& a_exe,
		const std::wstring& a_args, bool a_preferBroker);
}
