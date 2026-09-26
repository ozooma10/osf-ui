#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
	// Dev worker polls, debounces, and mirrors off-thread; Runtime drains completions on Tick.
	class DevViewReloadWorker
	{
	public:
		// Refresh and completion IDs name whole mod folders, not individual views.
		using Refresh = std::function<bool(std::string_view)>;

		DevViewReloadWorker(std::filesystem::path a_viewsRoot, Refresh a_refresh);
		~DevViewReloadWorker();

		DevViewReloadWorker(const DevViewReloadWorker&) = delete;
		DevViewReloadWorker& operator=(const DevViewReloadWorker&) = delete;

		void                                   SetMods(std::vector<std::string> a_mods);
		[[nodiscard]] std::vector<std::string> DrainCompleted();

	private:
		struct State
		{
			std::uint64_t                         fingerprint{ 0 };
			std::chrono::steady_clock::time_point changedAt{};
			std::chrono::steady_clock::time_point retryAt{};
			bool                                  initialized{ false };
			bool                                  pending{ false };
		};

		void Run(std::stop_token a_stop);

		std::filesystem::path                  m_viewsRoot;
		Refresh                                m_refresh;
		std::mutex                             m_mutex;
		std::condition_variable_any            m_wake;
		std::vector<std::string>               m_mods;
		bool                                   m_modsChanged{ false };  // guarded by m_mutex
		std::vector<std::string>               m_completed;
		std::unordered_map<std::string, State> m_states;
		std::jthread                           m_thread;
	};
}  // namespace OSFUI
