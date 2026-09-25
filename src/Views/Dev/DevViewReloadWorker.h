#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
	// Dev worker polls, debounces, and mirrors off-thread; Runtime drains completions on Tick.
	class DevViewReloadWorker
	{
	public:
		struct Target
		{
			std::string id;

			bool operator==(const Target&) const = default;
		};

		using Refresh = std::function<bool(std::string_view)>;

		DevViewReloadWorker(std::filesystem::path a_viewsRoot, Refresh a_refresh);
		~DevViewReloadWorker();

		DevViewReloadWorker(const DevViewReloadWorker&) = delete;
		DevViewReloadWorker& operator=(const DevViewReloadWorker&) = delete;

		void                              SetTargets(std::vector<Target> a_targets);
		[[nodiscard]] std::vector<Target> DrainCompleted();

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
		std::vector<Target>                    m_targets;
		bool                                   m_targetsChanged{ false };  // guarded by m_mutex
		std::vector<Target>                    m_completed;
		std::unordered_map<std::string, State> m_states;
		std::jthread                           m_thread;
	};
}  // namespace OSFUI
