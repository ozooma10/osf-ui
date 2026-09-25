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
		m_viewsRoot(std::move(a_viewsRoot)), m_refresh(std::move(a_refresh)),
		m_thread([this](std::stop_token stop) { Run(stop); })
	{
	}

	DevViewReloadWorker::~DevViewReloadWorker()
	{
		m_thread.request_stop();
		m_wake.notify_all();
	}

	void DevViewReloadWorker::SetTargets(std::vector<Target> a_targets)
	{
		std::ranges::sort(a_targets, {}, &Target::id);
		{
			std::scoped_lock lock(m_mutex);
			if (m_targets == a_targets)
				return;
			m_targets = std::move(a_targets);
			// Mark targets dirty before notifying the predicate-based wait.
			m_targetsChanged = true;
		}
		m_wake.notify_all();
	}

	std::vector<DevViewReloadWorker::Target> DevViewReloadWorker::DrainCompleted()
	{
		std::scoped_lock lock(m_mutex);
		auto             completed = std::move(m_completed);
		m_completed.clear();
		return completed;
	}

	void DevViewReloadWorker::Run(std::stop_token a_stop)
	{
		while (!a_stop.stop_requested()) {
			std::vector<Target> targets;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait_for(lock, a_stop, kScanInterval, [this] { return m_targetsChanged; });
				if (a_stop.stop_requested())
					return;
				m_targetsChanged = false;
				targets = m_targets;
			}

			const auto                      now = std::chrono::steady_clock::now();
			std::unordered_set<std::string> watched;
			for (const auto& target : targets) {
				watched.insert(target.id);
				// Settle the whole mod because view entries load sibling hashed assets.
				const auto fingerprint =
					DevViewFiles::Fingerprint(m_viewsRoot / DevViewFiles::ModFolder(target.id));
				if (!fingerprint)
					continue;
				auto& state = m_states[target.id];
				if (!state.initialized) {
					state.fingerprint = *fingerprint;
					state.initialized = true;
					// A view may first be opened after the source changed while it was
					// unwatched. Refresh the mirror before treating this baseline as current.
					state.changedAt = now - kSettleTime;
					state.pending = true;
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
				if (!m_refresh(target.id)) {
					state.retryAt = now + kRetryDelay;
					continue;
				}
				state.pending = false;
				{
					std::scoped_lock lock(m_mutex);
					m_completed.push_back(target);
				}
			}
			std::erase_if(m_states, [&](const auto& item) { return !watched.contains(item.first); });
		}
	}
}  // namespace OSFUI
