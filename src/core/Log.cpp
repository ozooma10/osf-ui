#include "core/Log.h"

#include <spdlog/spdlog.h>

namespace OSFUI::Log
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

		// SFSE::Init opens the log at Debug so nothing before config load is
		// lost. Now that we know devMode, raise the floor for normal play: a
		// player's SFSE log should hold boot markers, config, warnings and
		// errors — not the per-view / per-hook DEBUG chatter. devMode restores
		// the full firehose for development. flush_on tracks the level so
		// whatever we keep still survives a crash that never flushes.
		if (auto logger = spdlog::default_logger()) {
			const auto level = a_enabled ? spdlog::level::debug : spdlog::level::info;
			logger->set_level(level);
			logger->flush_on(level);
		}
	}
}
