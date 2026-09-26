#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "Runtime/AdaptiveViewGeometry.h"

namespace OSFUI
{
	// WndProc publishes cursor samples; Runtime publishes geometry and drains moves.
	// Runtime geometry bookkeeping relies on ordered, non-overlapping updates.
	// Fields shared with WndProc retain their atomic publication boundaries.
	class PointerInputState
	{
	public:
		PointerInputState();

		struct Position
		{
			int x;
			int y;
		};

		// Runtime geometry lifecycle.
		void Initialize(ViewSize a_size);
		void PublishGeometry(ViewSize a_capture, ViewSize a_view);
		bool UpdateFixedScaleformGeometry(bool a_fixed);
		ViewSize CaptureSize() const;
		ViewSize ViewportSize() const;
		void SuspendGeometry();
		void ResumeGeometry();
		void CenterCursor();

		// WndProc input sampling and queries.
		void UpdateAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		Position CursorPosition() const;
		bool GeometryReady() const;
		bool CanSendPointer() const;

		// WndProc and Runtime can queue; Runtime drains once per update or discards on recovery.
		void QueueMouseMove();
		std::optional<Position> TakeMouseMove();
		void DiscardMouseMove();

	private:
		// Runtime-owned geometry bookkeeping; no window-thread readers.
		ViewSize m_captureSize;
		bool m_fixedScaleformGeometry{ false };

		// Runtime and WndProc share the viewport, cursor and pending motion.
		std::atomic<std::uint64_t> m_viewSize;
		std::atomic<float> m_cursorX{ 0.0f };
		std::atomic<float> m_cursorY{ 0.0f };
		std::atomic_bool m_insideView{ true };
		std::atomic_bool m_geometryReady{ true };
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t> m_pendingMouseMove{ kNoPendingMouseMove };
	};
}
