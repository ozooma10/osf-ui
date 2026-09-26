#include "Views/Dev/DevViewReloadWorker.h"

#include <algorithm>

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

	void DevViewReloadWorker::SetMods(std::vector<std::string> a_mods)
	{
		std::ranges::sort(a_mods);
		a_mods.erase(std::unique(a_mods.begin(), a_mods.end()), a_mods.end());
		{
			std::scoped_lock lock(m_mutex);
			if (m_mods == a_mods)
				return;
			m_mods = std::move(a_mods);
			// Mark mods dirty before notifying the predicate-based wait.
			m_modsChanged = true;
		}
		m_wake.notify_all();
	}

	std::vector<std::string> DevViewReloadWorker::DrainCompleted()
	{
		std::scoped_lock lock(m_mutex);
		auto             completed = std::move(m_completed);
		m_completed.clear();
		return completed;
	}

	void DevViewReloadWorker::Run(std::stop_token a_stop)
	{
		while (!a_stop.stop_requested()) {
			std::vector<std::string> mods;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait_for(lock, a_stop, kScanInterval, [this] { return m_modsChanged; });
				if (a_stop.stop_requested())
					return;
				m_modsChanged = false;
				mods = m_mods;
			}

			const auto now = std::chrono::steady_clock::now();
			for (const auto& mod : mods) {
				// Settle the whole mod because view entries load sibling hashed assets.
				const auto fingerprint =
					DevViewFiles::Fingerprint(m_viewsRoot / mod);
				if (!fingerprint)
					continue;
				auto& state = m_states[mod];
				if (!state.initialized) {
					state.fingerprint = *fingerprint;
					state.initialized = true;
					// A mod may first be watched after the source changed while it was
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
				if (!m_refresh(mod)) {
					state.retryAt = now + kRetryDelay;
					continue;
				}
				state.pending = false;
				{
					std::scoped_lock lock(m_mutex);
					m_completed.push_back(mod);
				}
			}
			std::erase_if(m_states, [&](const auto& item) { return !std::ranges::binary_search(mods, item.first); });
		}
	}
}  // namespace OSFUI
