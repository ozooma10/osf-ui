#include "Input/XInputPoller.h"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdlib>

namespace OSFUI
{
	namespace
	{
		using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

		GetStateFn ResolveGetState()
		{
			// Load from System32 explicitly: this is runtime input plumbing and must
			// never resolve a same-named DLL beside the game or plugin.
			for (const auto* dll : { L"xinput1_4.dll", L"xinput9_1_0.dll" }) {
				if (const auto module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
					if (const auto proc = GetProcAddress(module, "XInputGetState")) {
						return reinterpret_cast<GetStateFn>(proc);
					}
					FreeLibrary(module);
				}
			}
			return nullptr;
		}

		float NormalizeThumb(SHORT a_value)
		{
			// The negative endpoint has one extra representable value. Clamping
			// keeps both sides in the bridge's documented -1..1 range.
			return std::clamp(static_cast<float>(a_value) / 32767.0f, -1.0f, 1.0f);
		}

		// "The player is touching this pad" — the slot-selection signal. Uses
		// the stock XInput deadzones so stick drift on an idle pad (or a
		// charging one) does not count as activity.
		bool ShowsActivity(const XINPUT_STATE& a_state)
		{
			const auto& pad = a_state.Gamepad;
			return pad.wButtons != 0 ||
				pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
				pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
				std::abs(static_cast<int>(pad.sThumbLX)) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
				std::abs(static_cast<int>(pad.sThumbLY)) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
				std::abs(static_cast<int>(pad.sThumbRX)) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE ||
				std::abs(static_cast<int>(pad.sThumbRY)) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
		}

		XInputPoller::State ToState(const XINPUT_STATE& a_state)
		{
			return {
				.connected = true,
				.buttons = a_state.Gamepad.wButtons,
				.lx = NormalizeThumb(a_state.Gamepad.sThumbLX),
				.ly = NormalizeThumb(a_state.Gamepad.sThumbLY),
				.rx = NormalizeThumb(a_state.Gamepad.sThumbRX),
				.ry = NormalizeThumb(a_state.Gamepad.sThumbRY),
			};
		}

		// The slot the player is actually using, latched for one capturing
		// interval (Runtime resets it alongside _directPadActive). Without the
		// latch-and-scan, the lowest CONNECTED slot always won — a charging
		// second pad, a wheel, or a Steam Input virtual device on slot 0 left
		// the player's real pad on slot 1 unread and overlay nav silently dead.
		constexpr DWORD kNoSlot = XUSER_MAX_COUNT;
		DWORD s_latchedSlot = kNoSlot;
	}

	XInputPoller::State XInputPoller::Poll()
	{
		static const auto getState = ResolveGetState();
		if (!getState) {
			return {};
		}

		// A latched slot keeps winning until it disconnects or the interval
		// ends: mid-interval the player's pad is authoritative even while idle
		// (a held-neutral frame must not hand nav to another device's noise).
		if (s_latchedSlot != kNoSlot) {
			XINPUT_STATE state{};
			if (getState(s_latchedSlot, &state) == ERROR_SUCCESS) {
				return ToState(state);
			}
			s_latchedSlot = kNoSlot;  // unplugged; fall through and rescan
		}

		// No latch yet: scan all four. The first slot showing real input wins
		// and latches; with everything neutral, report the first connected
		// slot's (neutral) state so `connected` stays truthful, but do not
		// latch — the decision waits for the player's first actual press.
		XInputPoller::State firstConnected{};
		for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
			XINPUT_STATE state{};
			if (getState(user, &state) != ERROR_SUCCESS) {
				continue;
			}
			if (ShowsActivity(state)) {
				s_latchedSlot = user;
				return ToState(state);
			}
			if (!firstConnected.connected) {
				firstConnected = ToState(state);
			}
		}
		return firstConnected;
	}

	void XInputPoller::ResetSlotLatch()
	{
		s_latchedSlot = kNoSlot;
	}
}
