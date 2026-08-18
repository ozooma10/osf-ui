#include "Core/Log.h"

#include <spdlog/spdlog.h>

namespace OSFUI::Log
{
	namespace
	{
		std::atomic_bool g_debugEnabled{ false };
	}

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::WARN("{}", a_message); });
	}

	bool DebugEnabled()
	{
		return g_debugEnabled.load(std::memory_order_relaxed);
	}

	void SetDebugLogging(bool a_enabled)
	{
		g_debugEnabled.store(a_enabled, std::memory_order_relaxed);

		// SFSE::Init opens the log at Debug so nothing before the persisted
		// startup setting is loaded is lost. Once Runtime has latched the effective
		// developer profile, raise the floor for normal play: a player's SFSE log
		// should hold boot markers, settings, warnings and errors — not the
		// per-view / per-hook DEBUG chatter. Debug logging restores the full
		// development firehose. flush_on tracks the level so whatever we keep
		// still survives a crash that never flushes.
		if (auto logger = spdlog::default_logger()) {
			const auto level = a_enabled ? spdlog::level::debug : spdlog::level::info;
			logger->set_level(level);
			logger->flush_on(level);
		}
	}
}
