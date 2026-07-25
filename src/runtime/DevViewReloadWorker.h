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
	// Dev-only background worker: metadata polling, debounce and MO2 mirror
	// synchronization stay off Starfield's main thread. Runtime supplies the
	// current loaded targets and drains completed reloads on Tick.
	class DevViewReloadWorker
	{
	public:
		struct Target
		{
			std::string id;
			bool        overlay{ false };
			bool        world{ false };

			bool operator==(const Target&) const = default;
		};

		using Refresh = std::function<bool(const Target&)>;

		DevViewReloadWorker(std::filesystem::path a_viewsRoot, Refresh a_refresh);
		~DevViewReloadWorker();

		DevViewReloadWorker(const DevViewReloadWorker&) = delete;
		DevViewReloadWorker& operator=(const DevViewReloadWorker&) = delete;

		void                              SetTargets(std::vector<Target> a_targets);
		[[nodiscard]] std::vector<Target> DrainReady();

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
		std::vector<Target>                    _ready;
		std::unordered_map<std::string, State> _states;
		std::jthread                           _thread;
	};
}  // namespace OSFUI
