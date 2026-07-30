#include "runtime/DevViewReloadWorker.h"

#include <algorithm>

#include "core/Log.h"
#include "runtime/DevViewFiles.h"

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
			// Without this flag the notify below was a no-op: wait_for's
			// predicate returned false, so a wake just re-waited to the same
			// deadline and new targets waited out the full scan interval.
			_targetsChanged = true;
		}
		_wake.notify_all();
	}

	std::vector<DevViewReloadWorker::Target> DevViewReloadWorker::DrainReady()
	{
		std::scoped_lock lock(_mutex);
		auto             ready = std::move(_ready);
		_ready.clear();
		return ready;
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
				// Mod scope, not view scope: the hashed bundles the entry HTML
				// points at live in views/<modId>/assets/, and settling on the
				// view folder alone would fire the reload while that sibling
				// was still being written.
				const auto fingerprint =
					DevViewFiles::Fingerprint(_viewsRoot / DevViewFiles::ModFolder(target.id));
				if (!fingerprint)
					continue;
				auto& state = _states[target.id];
				if (!state.initialized) {
					state.fingerprint = *fingerprint;
					state.initialized = true;
					continue;
				}
				if (state.fingerprint != *fingerprint) {
					state.fingerprint = *fingerprint;
					state.changedAt = now;
					state.pending = true;
					continue;
				}
				if (!state.pending || now - state.changedAt < kSettleTime || now < state.retryAt) {
					continue;
				}
				if (!_refresh(target)) {
					state.retryAt = now + kRetryDelay;
					continue;
				}
				state.pending = false;
				{
					std::scoped_lock lock(_mutex);
					_ready.push_back(target);
				}
			}
			std::erase_if(_states, [&](const auto& item) { return !watched.contains(item.first); });
		}
	}
}  // namespace OSFUI
