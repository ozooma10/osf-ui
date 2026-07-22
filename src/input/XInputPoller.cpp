#include "input/XInputPoller.h"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>

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
	}

	XInputPoller::State XInputPoller::Poll()
	{
		static const auto getState = ResolveGetState();
		if (!getState) {
			return {};
		}

		for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
			XINPUT_STATE state{};
			if (getState(user, &state) != ERROR_SUCCESS) {
				continue;
			}
			return {
				.connected = true,
				.buttons = state.Gamepad.wButtons,
				.lx = NormalizeThumb(state.Gamepad.sThumbLX),
				.ly = NormalizeThumb(state.Gamepad.sThumbLY),
				.rx = NormalizeThumb(state.Gamepad.sThumbRX),
				.ry = NormalizeThumb(state.Gamepad.sThumbRY),
			};
		}
		return {};
	}
}
