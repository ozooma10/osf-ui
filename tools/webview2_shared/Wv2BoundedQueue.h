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
	inline std::string CommandCoalesceKey(std::string_view a_type,
		std::string_view a_kind = {}, std::string_view a_identity = {})
	{
		if (a_type == "mouse" && a_kind == "move") return "mouse.move";
		if (a_type == "resize" || a_type == "focus" ||
			a_type == "accelState" || a_type == "renderStatsSample" ||
			a_type == "frameAck") {
			return std::string(a_type);
		}
		if ((a_type == "setHidden" || a_type == "setOrder" ||
				a_type == "setRenderStats") &&
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
			_capacity(a_capacity)
		{}

		PushResult Push(T a_value, std::string a_coalesceKey = {},
			std::uint64_t a_sequence = 0)
		{
			std::unique_lock lock(_mutex);
			if (_closed) return PushResult::Closed;
			if (!a_coalesceKey.empty() && !_items.empty() &&
				_items.back().coalesceKey == a_coalesceKey) {
				_items.back() = Item{
					std::move(a_value), std::move(a_coalesceKey), a_sequence };
				return PushResult::Coalesced;
			}
			if (_items.size() >= _capacity) return PushResult::Full;
			_items.push_back(Item{
				std::move(a_value), std::move(a_coalesceKey), a_sequence });
			lock.unlock();
			_ready.notify_one();
			return PushResult::Queued;
		}

		bool Prepend(std::vector<Item> a_items)
		{
			std::unique_lock lock(_mutex);
			if (_closed || a_items.size() + _items.size() > _capacity) return false;
			_items.insert(_items.begin(),
				std::make_move_iterator(a_items.begin()),
				std::make_move_iterator(a_items.end()));
			lock.unlock();
			_ready.notify_one();
			return true;
		}

		bool WaitPop(Item& a_item)
		{
			std::unique_lock lock(_mutex);
			_ready.wait(lock, [this] { return _closed || !_items.empty(); });
			if (_items.empty()) return false;
			a_item = std::move(_items.front());
			_items.pop_front();
			return true;
		}

		bool TryPop(Item& a_item)
		{
			std::scoped_lock lock(_mutex);
			if (_items.empty()) return false;
			a_item = std::move(_items.front());
			_items.pop_front();
			return true;
		}

		void Clear()
		{
			std::scoped_lock lock(_mutex);
			_items.clear();
		}

		void Close()
		{
			{
				std::scoped_lock lock(_mutex);
				_closed = true;
			}
			_ready.notify_all();
		}

		void Reset()
		{
			std::scoped_lock lock(_mutex);
			_items.clear();
			_closed = false;
		}

		[[nodiscard]] std::size_t Size() const
		{
			std::scoped_lock lock(_mutex);
			return _items.size();
		}

	private:
		std::size_t _capacity;
		mutable std::mutex _mutex;
		std::condition_variable _ready;
		std::deque<Item> _items;
		bool _closed{ false };
	};
}