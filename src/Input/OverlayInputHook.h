#pragma once

#include <cstdint>
#include <optional>

namespace OSFUI
{
	// Subclass the game HWND to route and consume raw input while the overlay owns input.
	namespace OverlayInputHook
	{
		struct ClientSize
		{
			std::uint32_t width{ 0 };
			std::uint32_t height{ 0 };
		};

		// Renderer-worker request to restore focus on Starfield's window thread.
		inline constexpr std::uint32_t kRestoreGameFocusMessage = 0x8049;
		// Wake WndProc to apply cursor state immediately after main-thread policy changes.
		inline constexpr std::uint32_t kRefreshInputStateMessage = 0x804A;
		// Install once on the first main-thread tick; never un-subclass another overlay's chain.
		bool Install();
		void RequestStateRefresh();
		// Subclassed game window and authority for window-thread platform facts.
		[[nodiscard]] void* GameWindowHandle();
		[[nodiscard]] std::optional<ClientSize> GameWindowClientSize();

		namespace detail
		{
			[[nodiscard]] constexpr bool OriginalMovedAboveUs(const std::uintptr_t a_current, const std::uintptr_t a_ours, const std::uintptr_t a_original)
			{
				return a_current != a_ours && a_current == a_original;
			}
		}
	}
}
