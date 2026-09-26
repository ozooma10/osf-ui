#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osfui::wv2
{
	inline std::string GameMessageCoalesceKey(std::string_view a_type,
		std::string_view a_kind = {}, std::string_view a_identity = {})
	{
		if (a_type == "mouse" && a_kind == "move") return "mouse.move";
		if (a_type == "resize" || a_type == "focus") return std::string(a_type);
		if ((a_type == "setHidden" || a_type == "setOrder") &&
			!a_identity.empty()) {
			return std::string(a_type) + ":" + std::string(a_identity);
		}
		return {};
	}

	template <class T>
	class BoundedQueue
	{
	public:
		struct Item
		{
			T             value;
			std::string   coalesceKey;
			std::uint64_t sequence{ 0 };
		};

		enum class PushResult
		{
			Queued,
			Coalesced,
			Full,
			Closed
		};

		explicit BoundedQueue(std::size_t a_capacity) :
			m_capacity(a_capacity)
		{}

		PushResult Push(T a_value, std::string a_coalesceKey = {},
			std::uint64_t a_sequence = 0)
		{
			std::unique_lock lock(m_mutex);
			if (m_closed) return PushResult::Closed;
			if (!a_coalesceKey.empty() && !m_items.empty() &&
				m_items.back().coalesceKey == a_coalesceKey) {
				m_items.back() = Item{
					std::move(a_value), std::move(a_coalesceKey), a_sequence };
				return PushResult::Coalesced;
			}
			if (m_items.size() >= m_capacity) return PushResult::Full;
			m_items.push_back(Item{
				std::move(a_value), std::move(a_coalesceKey), a_sequence });
			lock.unlock();
			m_ready.notify_one();
			return PushResult::Queued;
		}

		bool Prepend(std::vector<Item> a_items)
		{
			std::unique_lock lock(m_mutex);
			if (m_closed || a_items.size() + m_items.size() > m_capacity) return false;
			m_items.insert(m_items.begin(),
				std::make_move_iterator(a_items.begin()),
				std::make_move_iterator(a_items.end()));
			lock.unlock();
			m_ready.notify_one();
			return true;
		}

		bool WaitPop(Item& a_item)
		{
			std::unique_lock lock(m_mutex);
			m_ready.wait(lock, [this] { return m_closed || !m_items.empty(); });
			if (m_items.empty()) return false;
			a_item = std::move(m_items.front());
			m_items.pop_front();
			return true;
		}

		bool TryPop(Item& a_item)
		{
			std::scoped_lock lock(m_mutex);
			if (m_items.empty()) return false;
			a_item = std::move(m_items.front());
			m_items.pop_front();
			return true;
		}

		void Clear()
		{
			std::scoped_lock lock(m_mutex);
			m_items.clear();
		}

		void Close()
		{
			{
				std::scoped_lock lock(m_mutex);
				m_closed = true;
			}
			m_ready.notify_all();
		}

		void Reset()
		{
			std::scoped_lock lock(m_mutex);
			m_items.clear();
			m_closed = false;
		}

		[[nodiscard]] std::size_t Size() const
		{
			std::scoped_lock lock(m_mutex);
			return m_items.size();
		}

	private:
		std::size_t m_capacity;
		mutable std::mutex m_mutex;
		std::condition_variable m_ready;
		std::deque<Item> m_items;
		bool m_closed{ false };
	};
}
