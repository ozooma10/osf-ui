#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "OSFUI.h"

namespace OSFUI
{
	// Main owns the session and dispatches native callbacks. WndProc only adds
	// deltas or requests a stop through the atomic producer methods below.
	class RelativePointerSession
	{
	public:
		// Main-thread lifecycle.
		bool Begin(std::string_view a_viewId);
		void End(std::string_view a_viewId);
		void Cancel(std::string_view a_viewId = {});
		void ReconcileOwner(std::string_view a_allowedView);
		void Drain();

		// Window-message thread; these never invoke native callbacks.
		bool AccumulateMotion(int a_dx, int a_dy);
		bool AccumulateWheel(int a_wheelDelta);
		void RequestEnd();
		void RequestCancel();

	private:
		enum class Stop : std::uint32_t
		{
			kNone = 0,
			kEnd = 1,
			kCancel = 2,
		};
		void RequestStop(Stop a_stop);
		void Finish(API::RelativePointerPhase a_phase);

		std::atomic_bool m_active{ false };
		std::atomic<float> m_dx{ 0.0f };
		std::atomic<float> m_dy{ 0.0f };
		std::atomic<float> m_wheel{ 0.0f };
		std::atomic<Stop> m_stop{ Stop::kNone };
		std::string m_view;  // main-thread only
	};
}
