#include "core/Log.h"

namespace SWUI::Log
{
	namespace
	{
		std::atomic_bool g_devMode{ false };
	}

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::WARN("{}", a_message); });
	}

	bool DevMode()
	{
		return g_devMode.load(std::memory_order_relaxed);
	}

	void SetDevMode(bool a_enabled)
	{
		g_devMode.store(a_enabled, std::memory_order_relaxed);
	}
}
