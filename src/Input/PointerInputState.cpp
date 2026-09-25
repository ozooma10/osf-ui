#include "Input/PointerInputState.h"

#include "Input/AbsoluteMouseMapping.h"
#include "Views/ViewManifest.h"

namespace OSFUI
{
	PointerInputState::PointerInputState() :
		_captureSize(PackViewSize({ kDefaultViewWidth, kDefaultViewHeight })),
		_viewSize(PackViewSize({ kDefaultViewWidth, kDefaultViewHeight }))
	{}

	void PointerInputState::Initialize(ViewSize a_size)
	{
		_captureSize.store(PackViewSize(a_size));
		_viewSize.store(PackViewSize(a_size));
		_cursorX = a_size.width * 0.5f;
		_cursorY = a_size.height * 0.5f;
	}

	void PointerInputState::PublishGeometry(ViewSize a_capture, ViewSize a_view)
	{
		_pendingMouseMove.store(kNoPendingMouseMove, std::memory_order_release);
		_insideView.store(false, std::memory_order_release);
		_captureSize.store(PackViewSize(a_capture), std::memory_order_release);
		_viewSize.store(PackViewSize(a_view), std::memory_order_release);
	}

	bool PointerInputState::UpdateFixedScaleformGeometry(bool a_fixed)
	{
		const bool changed = a_fixed != _fixedScaleformGeometry;
		_fixedScaleformGeometry = a_fixed;
		return changed;
	}

	void PointerInputState::ObserveGameClientSize()
	{
		_gameClientSizeObserved.store(true, std::memory_order_release);
	}

	bool PointerInputState::GameClientSizeObserved() const
	{
		return _gameClientSizeObserved.load(std::memory_order_acquire);
	}

	ViewSize PointerInputState::CaptureSize() const
	{
		return UnpackViewSize(_captureSize.load(std::memory_order_acquire));
	}

	ViewSize PointerInputState::ViewportSize() const
	{
		return UnpackViewSize(_viewSize.load(std::memory_order_acquire));
	}

	void PointerInputState::SuspendGeometry()
	{
		_geometryReady.store(false, std::memory_order_release);
	}

	void PointerInputState::ResumeGeometry()
	{
		_geometryReady.store(true, std::memory_order_release);
	}

	bool PointerInputState::GeometryReady() const
	{
		return _geometryReady.load(std::memory_order_acquire);
	}

	bool PointerInputState::CanSendPointer() const
	{
		return GeometryReady() && _insideView.load(std::memory_order_relaxed);
	}

	void PointerInputState::CenterCursor()
	{
		const auto view = ViewportSize();
		_cursorX = view.width * 0.5f;
		_cursorY = view.height * 0.5f;
		_insideView.store(true, std::memory_order_release);
	}

	void PointerInputState::UpdateAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (a_clientW <= 0 || a_clientH <= 0) return;
		const auto view = ViewportSize();
		const auto mapped = MapAbsoluteMouseToView(
			a_clientX, a_clientY, a_clientW, a_clientH, view.width, view.height);
		_cursorX.store(mapped.x, std::memory_order_relaxed);
		_cursorY.store(mapped.y, std::memory_order_relaxed);
		_insideView.store(mapped.inside, std::memory_order_relaxed);
		if (GeometryReady()) QueueMouseMove();
	}

	PointerInputState::Position PointerInputState::CursorPosition() const
	{
		return {
			static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(_cursorY.load(std::memory_order_relaxed)),
		};
	}

	void PointerInputState::QueueMouseMove()
	{
		const auto position = CursorPosition();
		const auto x = static_cast<std::uint32_t>(position.x);
		const auto y = static_cast<std::uint32_t>(position.y);
		_pendingMouseMove.store((static_cast<std::uint64_t>(x) << 32) | y);
	}

	std::optional<PointerInputState::Position> PointerInputState::TakeMouseMove()
	{
		const auto packed = _pendingMouseMove.exchange(kNoPendingMouseMove);
		if (packed == kNoPendingMouseMove) return std::nullopt;
		return Position{ static_cast<int>(packed >> 32), static_cast<int>(packed & 0xFFFF'FFFFull) };
	}

	void PointerInputState::DiscardMouseMove()
	{
		_pendingMouseMove.store(kNoPendingMouseMove);
	}
}
