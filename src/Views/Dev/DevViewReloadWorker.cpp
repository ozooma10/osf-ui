#include "Views/Dev/DevViewReloadWorker.h"

#include <algorithm>

#include "Core/Log.h"
#include "Views/Dev/DevViewFiles.h"

namespace OSFUI
{
	namespace
	{
		using namespace std::chrono_literals;
		constexpr auto kScanInterval = 250ms;
		constexpr auto kSettleTime = 250ms;
		constexpr auto kRetryDelay = 1s;
	}  // namespace

	DevViewReloadWorker::DevViewReloadWorker(std::filesystem::path a_viewsRoot, Refresh a_refresh) :
		_viewsRoot(std::move(a_viewsRoot)), _refresh(std::move(a_refresh)),
		_thread([this](std::stop_token stop) { Run(stop); })
	{
	}

	DevViewReloadWorker::~DevViewReloadWorker()
	{
		_thread.request_stop();
		_wake.notify_all();
	}

	void DevViewReloadWorker::SetTargets(std::vector<Target> a_targets)
	{
		std::ranges::sort(a_targets, {}, &Target::id);
		{
			std::scoped_lock lock(_mutex);
			if (_targets == a_targets)
				return;
			_targets = std::move(a_targets);
			// Mark targets dirty before notifying the predicate-based wait.
			_targetsChanged = true;
		}
		_wake.notify_all();
	}

	std::vector<DevViewReloadWorker::Target> DevViewReloadWorker::DrainCompleted()
	{
		std::scoped_lock lock(_mutex);
		auto             completed = std::move(_completed);
		_completed.clear();
		return completed;
	}

	void DevViewReloadWorker::Run(std::stop_token a_stop)
	{
		while (!a_stop.stop_requested()) {
			std::vector<Target> targets;
			{
				std::unique_lock lock(_mutex);
				_wake.wait_for(lock, a_stop, kScanInterval, [this] { return _targetsChanged; });
				if (a_stop.stop_requested())
					return;
				_targetsChanged = false;
				targets = _targets;
			}

			const auto                      now = std::chrono::steady_clock::now();
			std::unordered_set<std::string> watched;
			for (const auto& target : targets) {
				watched.insert(target.id);
				// Settle the whole mod and canonical shared assets because both are
				// projected beneath the mod's virtual-host root.
				const auto modFingerprint =
					DevViewFiles::Fingerprint(_viewsRoot / DevViewFiles::ModFolder(target.id));
				const auto sharedFingerprint = DevViewFiles::Fingerprint(_viewsRoot / "shared");
				if (!modFingerprint || !sharedFingerprint)
					continue;
				const auto fingerprint = *modFingerprint ^
					(*sharedFingerprint + 0x9e3779b97f4a7c15ull +
						(*modFingerprint << 6) + (*modFingerprint >> 2));
				auto& state = _states[target.id];
				if (!state.initialized) {
					state.fingerprint = fingerprint;
					state.initialized = true;
					continue;
				}
				if (state.fingerprint != fingerprint) {
					state.fingerprint = fingerprint;
					state.changedAt = now;
					state.pending = true;
					continue;
				}
				if (!state.pending || now - state.changedAt < kSettleTime || now < state.retryAt) {
					continue;
				}
				if (!_refresh(target.id)) {
					state.retryAt = now + kRetryDelay;
					continue;
				}
				state.pending = false;
				{
					std::scoped_lock lock(_mutex);
					_completed.push_back(target);
				}
			}
			std::erase_if(_states, [&](const auto& item) { return !watched.contains(item.first); });
		}
	}
}  // namespace OSFUI
