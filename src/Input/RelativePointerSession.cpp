#include "Input/RelativePointerSession.h"

#include "API/BridgeApi.h"

namespace OSFUI
{
	bool RelativePointerSession::Begin(std::string_view a_viewId)
	{
		if (m_active.load(std::memory_order_acquire) && m_view == a_viewId) {
			return true;
		}
		if (!m_view.empty()) {
			Finish(API::RelativePointerPhase::kCancel);
		}
		m_dx.store(0.0f, std::memory_order_relaxed);
		m_dy.store(0.0f, std::memory_order_relaxed);
		m_wheel.store(0.0f, std::memory_order_relaxed);
		m_stop.store(Stop::kNone, std::memory_order_release);
		m_view = a_viewId;
		m_active.store(true, std::memory_order_release);
		if (!API::BridgeApi::Get().DispatchRelativePointer(m_view, API::RelativePointerPhase::kBegin)) {
			m_active.store(false, std::memory_order_release);
			m_view.clear();
			return false;
		}
		return true;
	}

	void RelativePointerSession::End(std::string_view a_viewId)
	{
		if (!m_view.empty() && m_view == a_viewId) {
			Finish(API::RelativePointerPhase::kEnd);
		}
	}

	void RelativePointerSession::Cancel(std::string_view a_viewId)
	{
		if (m_view.empty() || (!a_viewId.empty() && m_view != a_viewId)) {
			return;
		}
		Finish(API::RelativePointerPhase::kCancel);
	}

	void RelativePointerSession::Finish(API::RelativePointerPhase a_phase)
	{
		if (m_view.empty()) {
			return;
		}
		m_active.store(false, std::memory_order_release);
		m_stop.store(Stop::kNone, std::memory_order_release);
		const float dx = m_dx.exchange(0.0f, std::memory_order_acq_rel);
		const float dy = m_dy.exchange(0.0f, std::memory_order_acq_rel);
		const float wheel = m_wheel.exchange(0.0f, std::memory_order_acq_rel);
		if (dx != 0.0f || dy != 0.0f || wheel != 0.0f) {
			API::BridgeApi::Get().DispatchRelativePointer(m_view, API::RelativePointerPhase::kUpdate, dx, dy, wheel);
		}
		API::BridgeApi::Get().DispatchRelativePointer(m_view, a_phase);
		m_view.clear();
		// A WndProc packet that observed the old active edge can finish its atomic
		// add after the exchanges above. Never let that tail leak into a later owner.
		m_dx.store(0.0f, std::memory_order_relaxed);
		m_dy.store(0.0f, std::memory_order_relaxed);
		m_wheel.store(0.0f, std::memory_order_relaxed);
	}

	void RelativePointerSession::Drain()
	{
		if (m_view.empty()) {
			m_dx.store(0.0f, std::memory_order_relaxed);
			m_dy.store(0.0f, std::memory_order_relaxed);
			m_wheel.store(0.0f, std::memory_order_relaxed);
			m_stop.store(Stop::kNone, std::memory_order_release);
			return;
		}
		if (!API::BridgeApi::Get().HasRelativePointer(m_view)) {
			m_active.store(false, std::memory_order_release);
			m_view.clear();
			m_dx.store(0.0f, std::memory_order_relaxed);
			m_dy.store(0.0f, std::memory_order_relaxed);
			m_wheel.store(0.0f, std::memory_order_relaxed);
			m_stop.store(Stop::kNone, std::memory_order_release);
			return;
		}
		const float dx = m_dx.exchange(0.0f, std::memory_order_acq_rel);
		const float dy = m_dy.exchange(0.0f, std::memory_order_acq_rel);
		const float wheel = m_wheel.exchange(0.0f, std::memory_order_acq_rel);
		if (dx != 0.0f || dy != 0.0f || wheel != 0.0f) {
			if (!API::BridgeApi::Get().DispatchRelativePointer(m_view, API::RelativePointerPhase::kUpdate, dx, dy, wheel)) {
				m_active.store(false, std::memory_order_release);
				m_view.clear();
				return;
			}
		}
		const auto stop = m_stop.exchange(Stop::kNone, std::memory_order_acq_rel);
		if (stop == Stop::kEnd) {
			Finish(API::RelativePointerPhase::kEnd);
		} else if (stop == Stop::kCancel) {
			Finish(API::RelativePointerPhase::kCancel);
		}
	}

	bool RelativePointerSession::AccumulateMotion(int a_dx, int a_dy)
	{
		if (!m_active.load(std::memory_order_acquire)) {
			return false;
		}
		if (a_dx != 0) {
			m_dx.fetch_add(static_cast<float>(a_dx), std::memory_order_relaxed);
		}
		if (a_dy != 0) {
			m_dy.fetch_add(static_cast<float>(a_dy), std::memory_order_relaxed);
		}
		return true;
	}

	bool RelativePointerSession::AccumulateWheel(int a_wheelDelta)
	{
		if (!m_active.load(std::memory_order_acquire)) return false;
		// Match DOM WheelEvent.deltaY: positive scrolls down, opposite Win32.
		m_wheel.fetch_add(-static_cast<float>(a_wheelDelta) / 120.0f, std::memory_order_relaxed);
		return true;
	}

	void RelativePointerSession::RequestStop(Stop a_stop)
	{
		if (m_active.exchange(false, std::memory_order_acq_rel)) {
			m_stop.store(a_stop, std::memory_order_release);
		}
	}

	void RelativePointerSession::RequestEnd()
	{
		RequestStop(Stop::kEnd);
	}

	void RelativePointerSession::RequestCancel()
	{
		RequestStop(Stop::kCancel);
	}

	void RelativePointerSession::ReconcileOwner(std::string_view a_allowedView)
	{
		if (m_view != a_allowedView) Cancel();
	}
}
