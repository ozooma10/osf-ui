#pragma once

#include "BrowserBoundedQueue.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace OSFUI
{
	// Runtime-owned retirement barrier. The old transport must finish before its
	// replacement can consume messages queued by page rehydration. No detached
	// work: destroying the owner waits for retirement before its resources die.
	class BrowserHostRetirement
	{
	public:
		using Queue = osfui::browser::BoundedQueue<std::string>;
		struct Completion
		{
			std::vector<Queue::Item> replay;
			std::exception_ptr error;
			bool overflow{};
		};

		explicit BrowserHostRetirement(std::size_t capacity) : m_replay(capacity) {}
		~BrowserHostRetirement() { Wait(); }

		// Start, Pending, TakeReady and Wait belong to the runtime/owner thread.
		bool Start(std::function<void()> retire)
		{
			if (Pending()) return false;
			m_replay.Reset();
			m_overflow.store(false);
			m_work = std::async(std::launch::async, std::move(retire));
			return true;
		}
		[[nodiscard]] bool Pending() const { return m_work.valid(); }
		Queue::PushResult Push(std::string value, std::string key)
		{
			const auto result = m_replay.Push(std::move(value), std::move(key));
			if (result == Queue::PushResult::Full) m_overflow.store(true);
			return result;
		}
		std::optional<Completion> TakeReady()
		{
			if (!Pending() || m_work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
			Completion result;
			try { m_work.get(); }
			catch (...) { result.error = std::current_exception(); }
			result.overflow = m_overflow.load();
			Queue::Item item;
			while (m_replay.TryPop(item)) result.replay.push_back(std::move(item));
			return result;
		}
		void Wait() { if (Pending()) m_work.wait(); }

	private:
		Queue m_replay;
		std::atomic_bool m_overflow{};
		std::future<void> m_work;
	};
}
