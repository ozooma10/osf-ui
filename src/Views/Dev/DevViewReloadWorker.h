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

		std::filesystem::path                  _viewsRoot;
		Refresh                                _refresh;
		std::mutex                             _mutex;
		std::condition_variable_any            _wake;
		std::vector<Target>                    _targets;
		bool                                   _targetsChanged{ false };  // guarded by _mutex
		std::vector<Target>                    _completed;
		std::unordered_map<std::string, State> _states;
		std::jthread                           _thread;
	};
}  // namespace OSFUI
