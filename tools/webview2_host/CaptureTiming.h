#pragma once

#include <Windows.h>
#include <mmsystem.h>

namespace osfui::webview2
{
	// WGC runs in an invisible host even when its output is visible in Starfield.
	// Windows 11 may otherwise ignore this process's timer-resolution requests.
	// Hold the request during startup and while a view is visible; restore the previous policy
	// when hidden and on teardown. This does not change the game's timer policy.
	class CaptureTiming
	{
	public:
		struct Result
		{
			MMRESULT timer{};
			DWORD policyError{};
		};

		CaptureTiming() = default;
		CaptureTiming(const CaptureTiming&) = delete;
		CaptureTiming& operator=(const CaptureTiming&) = delete;
		~CaptureTiming() { Disable(); }

		Result Enable() noexcept
		{
			if (enabled_) return result_;
			enabled_ = true;
			result_ = {};
			if (!policyChanged_) previous_ = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0 };
			if (policyChanged_ || ::GetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling,
				&previous_, sizeof(previous_))) {
				auto policy = previous_;
				policy.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
				policy.StateMask &= ~PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
				if (::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling,
					&policy, sizeof(policy))) {
					policyChanged_ = true;
				} else {
					result_.policyError = ::GetLastError();
				}
			} else {
				result_.policyError = ::GetLastError();
			}
			result_.timer = ::timeBeginPeriod(1);
			timerRequested_ = result_.timer == TIMERR_NOERROR;
			return result_;
		}

		DWORD Disable() noexcept
		{
			enabled_ = false;
			if (timerRequested_) {
				::timeEndPeriod(1);
				timerRequested_ = false;
			}
			if (policyChanged_) {
				if (!::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling,
					&previous_, sizeof(previous_))) return ::GetLastError();
				policyChanged_ = false;
			}
			return ERROR_SUCCESS;
		}

	private:
		bool enabled_{}, timerRequested_{}, policyChanged_{};
		PROCESS_POWER_THROTTLING_STATE previous_{};
		Result result_{};
	};
}
