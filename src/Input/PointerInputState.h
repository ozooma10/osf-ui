#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "Runtime/AdaptiveViewGeometry.h"

namespace OSFUI
{
	// WndProc publishes cursor samples; Runtime publishes geometry and drains moves.
	class PointerInputState
	{
	public:
		PointerInputState();

		struct Position
		{
			int x;
			int y;
		};

		// Runtime.
		void Initialize(ViewSize a_size);
		void PublishGeometry(ViewSize a_capture, ViewSize a_view);
		ViewSize CaptureSize() const;
		ViewSize ViewportSize() const;
		void CenterCursor();

		// WndProc.
		void UpdateAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		Position CursorPosition() const;
		bool CanSendPointer() const;

		// WndProc and Runtime can queue; Runtime drains once per update or discards on recovery.
		void QueueMouseMove();
		std::optional<Position> TakeMouseMove();
		void DiscardMouseMove();

	private:
		// Runtime only.
		ViewSize m_captureSize;

		// Shared with WndProc.
		std::atomic<std::uint64_t> m_viewSize;
		std::atomic<float> m_cursorX{ 0.0f };
		std::atomic<float> m_cursorY{ 0.0f };
		std::atomic_bool m_insideView{ true };
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t> m_pendingMouseMove{ kNoPendingMouseMove };
	};
}
