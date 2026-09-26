#include "Input/PointerInputState.h"

#include "Input/AbsoluteMouseMapping.h"
#include "Views/ViewManifest.h"

namespace OSFUI
{
	PointerInputState::PointerInputState() :
		m_captureSize{ kDefaultViewWidth, kDefaultViewHeight },
		m_viewSize(PackViewSize({ kDefaultViewWidth, kDefaultViewHeight }))
	{}

	void PointerInputState::Initialize(ViewSize a_size)
	{
		m_captureSize = a_size;
		m_viewSize.store(PackViewSize(a_size));
		m_cursorX = a_size.width * 0.5f;
		m_cursorY = a_size.height * 0.5f;
	}

	void PointerInputState::PublishGeometry(ViewSize a_capture, ViewSize a_view)
	{
		m_pendingMouseMove.store(kNoPendingMouseMove, std::memory_order_release);
		m_insideView.store(false, std::memory_order_release);
		m_captureSize = a_capture;
		m_viewSize.store(PackViewSize(a_view), std::memory_order_release);
	}

	bool PointerInputState::UpdateFixedScaleformGeometry(bool a_fixed)
	{
		const bool changed = a_fixed != m_fixedScaleformGeometry;
		m_fixedScaleformGeometry = a_fixed;
		return changed;
	}

	void PointerInputState::ObserveGameClientSize()
	{
		m_gameClientSizeObserved = true;
	}

	bool PointerInputState::GameClientSizeObserved() const
	{
		return m_gameClientSizeObserved;
	}

	ViewSize PointerInputState::CaptureSize() const
	{
		return m_captureSize;
	}

	ViewSize PointerInputState::ViewportSize() const
	{
		return UnpackViewSize(m_viewSize.load(std::memory_order_acquire));
	}

	void PointerInputState::SuspendGeometry()
	{
		m_geometryReady.store(false, std::memory_order_release);
	}

	void PointerInputState::ResumeGeometry()
	{
		m_geometryReady.store(true, std::memory_order_release);
	}

	bool PointerInputState::GeometryReady() const
	{
		return m_geometryReady.load(std::memory_order_acquire);
	}

	bool PointerInputState::CanSendPointer() const
	{
		return GeometryReady() && m_insideView.load(std::memory_order_relaxed);
	}

	void PointerInputState::CenterCursor()
	{
		const auto view = ViewportSize();
		m_cursorX = view.width * 0.5f;
		m_cursorY = view.height * 0.5f;
		m_insideView.store(true, std::memory_order_release);
	}

	void PointerInputState::UpdateAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (a_clientW <= 0 || a_clientH <= 0) return;
		const auto view = ViewportSize();
		const auto mapped = MapAbsoluteMouseToView(
			a_clientX, a_clientY, a_clientW, a_clientH, view.width, view.height);
		m_cursorX.store(mapped.x, std::memory_order_relaxed);
		m_cursorY.store(mapped.y, std::memory_order_relaxed);
		m_insideView.store(mapped.inside, std::memory_order_relaxed);
		if (GeometryReady()) QueueMouseMove();
	}

	PointerInputState::Position PointerInputState::CursorPosition() const
	{
		return {
			static_cast<int>(m_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(m_cursorY.load(std::memory_order_relaxed)),
		};
	}

	void PointerInputState::QueueMouseMove()
	{
		const auto position = CursorPosition();
		const auto x = static_cast<std::uint32_t>(position.x);
		const auto y = static_cast<std::uint32_t>(position.y);
		m_pendingMouseMove.store((static_cast<std::uint64_t>(x) << 32) | y);
	}

	std::optional<PointerInputState::Position> PointerInputState::TakeMouseMove()
	{
		const auto packed = m_pendingMouseMove.exchange(kNoPendingMouseMove);
		if (packed == kNoPendingMouseMove) return std::nullopt;
		return Position{ static_cast<int>(packed >> 32), static_cast<int>(packed & 0xFFFF'FFFFull) };
	}

	void PointerInputState::DiscardMouseMove()
	{
		m_pendingMouseMove.store(kNoPendingMouseMove);
	}
}
