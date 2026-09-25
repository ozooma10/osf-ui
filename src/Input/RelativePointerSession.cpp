#include "Input/RelativePointerSession.h"

#include "API/BridgeApi.h"

namespace OSFUI
{
	bool RelativePointerSession::Begin(std::string_view a_viewId)
	{
		if (_active.load(std::memory_order_acquire) && _view == a_viewId) {
			return true;
		}
		if (!_view.empty()) {
			Finish(API::RelativePointerPhase::kCancel);
		}
		_dx.store(0.0f, std::memory_order_relaxed);
		_dy.store(0.0f, std::memory_order_relaxed);
		_wheel.store(0.0f, std::memory_order_relaxed);
		_stop.store(Stop::kNone, std::memory_order_release);
		_view = a_viewId;
		_active.store(true, std::memory_order_release);
		if (!API::BridgeApi::Get().DispatchRelativePointer(_view, API::RelativePointerPhase::kBegin)) {
			_active.store(false, std::memory_order_release);
			_view.clear();
			return false;
		}
		return true;
	}

	void RelativePointerSession::End(std::string_view a_viewId)
	{
		if (!_view.empty() && _view == a_viewId) {
			Finish(API::RelativePointerPhase::kEnd);
		}
	}

	void RelativePointerSession::Cancel(std::string_view a_viewId)
	{
		if (_view.empty() || (!a_viewId.empty() && _view != a_viewId)) {
			return;
		}
		Finish(API::RelativePointerPhase::kCancel);
	}

	void RelativePointerSession::Finish(API::RelativePointerPhase a_phase)
	{
		if (_view.empty()) {
			return;
		}
		_active.store(false, std::memory_order_release);
		_stop.store(Stop::kNone, std::memory_order_release);
		const float dx = _dx.exchange(0.0f, std::memory_order_acq_rel);
		const float dy = _dy.exchange(0.0f, std::memory_order_acq_rel);
		const float wheel = _wheel.exchange(0.0f, std::memory_order_acq_rel);
		if (dx != 0.0f || dy != 0.0f || wheel != 0.0f) {
			API::BridgeApi::Get().DispatchRelativePointer(_view, API::RelativePointerPhase::kUpdate, dx, dy, wheel);
		}
		API::BridgeApi::Get().DispatchRelativePointer(_view, a_phase);
		_view.clear();
		// A WndProc packet that observed the old active edge can finish its atomic
		// add after the exchanges above. Never let that tail leak into a later owner.
		_dx.store(0.0f, std::memory_order_relaxed);
		_dy.store(0.0f, std::memory_order_relaxed);
		_wheel.store(0.0f, std::memory_order_relaxed);
	}

	void RelativePointerSession::Drain()
	{
		if (_view.empty()) {
			_dx.store(0.0f, std::memory_order_relaxed);
			_dy.store(0.0f, std::memory_order_relaxed);
			_wheel.store(0.0f, std::memory_order_relaxed);
			_stop.store(Stop::kNone, std::memory_order_release);
			return;
		}
		if (!API::BridgeApi::Get().HasRelativePointer(_view)) {
			_active.store(false, std::memory_order_release);
			_view.clear();
			_dx.store(0.0f, std::memory_order_relaxed);
			_dy.store(0.0f, std::memory_order_relaxed);
			_wheel.store(0.0f, std::memory_order_relaxed);
			_stop.store(Stop::kNone, std::memory_order_release);
			return;
		}
		const float dx = _dx.exchange(0.0f, std::memory_order_acq_rel);
		const float dy = _dy.exchange(0.0f, std::memory_order_acq_rel);
		const float wheel = _wheel.exchange(0.0f, std::memory_order_acq_rel);
		if (dx != 0.0f || dy != 0.0f || wheel != 0.0f) {
			if (!API::BridgeApi::Get().DispatchRelativePointer(_view, API::RelativePointerPhase::kUpdate, dx, dy, wheel)) {
				_active.store(false, std::memory_order_release);
				_view.clear();
				return;
			}
		}
		const auto stop = _stop.exchange(Stop::kNone, std::memory_order_acq_rel);
		if (stop == Stop::kEnd) {
			Finish(API::RelativePointerPhase::kEnd);
		} else if (stop == Stop::kCancel) {
			Finish(API::RelativePointerPhase::kCancel);
		}
	}

	bool RelativePointerSession::AccumulateMotion(int a_dx, int a_dy)
	{
		if (!_active.load(std::memory_order_acquire)) {
			return false;
		}
		if (a_dx != 0) {
			_dx.fetch_add(static_cast<float>(a_dx), std::memory_order_relaxed);
		}
		if (a_dy != 0) {
			_dy.fetch_add(static_cast<float>(a_dy), std::memory_order_relaxed);
		}
		return true;
	}

	bool RelativePointerSession::AccumulateWheel(int a_wheelDelta)
	{
		if (!_active.load(std::memory_order_acquire)) return false;
		// Match DOM WheelEvent.deltaY: positive scrolls down, opposite Win32.
		_wheel.fetch_add(-static_cast<float>(a_wheelDelta) / 120.0f, std::memory_order_relaxed);
		return true;
	}

	void RelativePointerSession::RequestStop(Stop a_stop)
	{
		if (_active.exchange(false, std::memory_order_acq_rel)) {
			_stop.store(a_stop, std::memory_order_release);
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
		if (_view != a_allowedView) Cancel();
	}
}
