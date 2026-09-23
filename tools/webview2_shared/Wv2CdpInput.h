#pragma once

#include "Wv2Messages.h"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace osfui::wv2
{
	// CDP completion order is not guaranteed unless calls are serialized. This
	// queue is confined to the browser STA; Close makes late callbacks harmless.
	class CdpInputQueue : public std::enable_shared_from_this<CdpInputQueue>
	{
	public:
		using Completion = std::function<void(bool)>;
		using Dispatch = std::function<void(const std::string&, const nlohmann::json&, Completion)>;
		CdpInputQueue(Dispatch a_dispatch, std::function<void()> a_failure) :
			_dispatch(std::move(a_dispatch)), _failure(std::move(a_failure)) {}
		bool Idle() const { return _closed || (!_busy && _pending.empty()); }
		void CheckTimeout(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now())
		{
			if (!_closed && _busy && a_now - _started > std::chrono::seconds(5)) Fail();
		}

		void Push(std::string a_method, nlohmann::json a_params)
		{
			if (_closed) return;
			if (_pending.size() >= 256) { Fail(); return; }
			_pending.emplace_back(std::move(a_method), std::move(a_params));
			Pump();
		}

		void Close()
		{
			_closed = true;
			_pending.clear();
			_dispatch = {};
			_failure = {};
		}

	private:
		void Fail()
		{
			auto failure = _failure;
			Close();
			if (failure) failure();
		}
		void Pump()
		{
			if (_closed || _busy || _pending.empty()) return;
			_busy = true;
			_started = std::chrono::steady_clock::now();
			// Keep arguments alive even if a synchronous failure closes the queue.
			const auto command = _pending.front();
			const auto dispatch = _dispatch;
			dispatch(command.first, command.second, [self = shared_from_this()](bool ok) {
				if (self->_closed) return;
				if (!ok) { self->Fail(); return; }
				self->_pending.pop_front();
				self->_busy = false;
				self->Pump();
			});
		}
		Dispatch _dispatch;
		std::function<void()> _failure;
		std::deque<std::pair<std::string, nlohmann::json>> _pending;
		bool _busy{ false }, _closed{ false };
		std::chrono::steady_clock::time_point _started;
	};

	inline nlohmann::json CdpKeyParams(const msg::Keyboard& a_key)
	{
		nlohmann::json out{
			{ "type", a_key.down ? "rawKeyDown" : "keyUp" },
			{ "windowsVirtualKeyCode", a_key.vk },
			{ "modifiers", a_key.modifiers },
			{ "autoRepeat", a_key.repeat },
			{ "isSystemKey", a_key.system },
			{ "isKeypad", a_key.keypad },
			{ "location", a_key.location },
		};
		if (!a_key.key.empty()) out["key"] = a_key.key;
		if (!a_key.code.empty()) out["code"] = a_key.code;
		return out;
	}

	class CdpPressedKeys
	{
	public:
		void Observe(const msg::Keyboard& a_key)
		{
			const auto id = std::pair{ a_key.vk, a_key.location };
			if (a_key.down) _keys[id] = a_key;
			else _keys.erase(id);
		}
		std::vector<msg::Keyboard> ReleaseAll()
		{
			std::vector<msg::Keyboard> releases;
			for (auto& [id, key] : _keys) {
				key.down = false;
				key.repeat = false;
				key.modifiers = 0;
				releases.push_back(std::move(key));
			}
			_keys.clear();
			return releases;
		}
	private:
		std::map<std::pair<std::uint32_t, std::uint32_t>, msg::Keyboard> _keys;
	};

	// WM_CHAR carries UTF-16 code units. Never put half of a surrogate pair on
	// the UTF-8 JSON wire, including when a menu closes between its two messages.
	class Utf16Input
	{
	public:
		void Reset() { _high = 0; }
		std::u16string Push(char16_t c)
		{
			if (c >= 0xD800 && c <= 0xDBFF) { _high = c; return {}; }
			const auto high = std::exchange(_high, 0);
			if (c >= 0xDC00 && c <= 0xDFFF) {
				return high ? std::u16string{ high, c } : std::u16string{};
			}
			return std::u16string{ c };
		}
	private:
		char16_t _high{ 0 };
	};
}
