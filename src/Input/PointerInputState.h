#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "Runtime/AdaptiveViewGeometry.h"

namespace OSFUI
{
	// WndProc publishes cursor samples; main publishes geometry and drains moves.
	// Shared fields retain their atomic publication boundaries. Main alone owns
	// the Scaleform geometry mode and decides when the rendered view is ready.
	class PointerInputState
	{
	public:
		PointerInputState();

		struct Position
		{
			int x;
			int y;
		};

		// Main-thread geometry lifecycle.
		void Initialize(ViewSize a_size);
		void PublishGeometry(ViewSize a_capture, ViewSize a_view);
		bool UpdateFixedScaleformGeometry(bool a_fixed);
		void ObserveGameClientSize();
		bool GameClientSizeObserved() const;
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

		// Both threads can queue; main drains once per frame or discards on recovery.
		void QueueMouseMove();
		std::optional<Position> TakeMouseMove();
		void DiscardMouseMove();

	private:
		std::atomic<float> _cursorX{ 0.0f };
		std::atomic<float> _cursorY{ 0.0f };
		std::atomic_bool _insideView{ true };
		std::atomic_bool _geometryReady{ true };
		std::atomic<std::uint64_t> _captureSize;
		std::atomic<std::uint64_t> _viewSize;
		std::atomic_bool _gameClientSizeObserved{ false };
		bool _fixedScaleformGeometry{ false };
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t> _pendingMouseMove{ kNoPendingMouseMove };
	};
}
