#include "CaptureTiming.h"
#include "check.h"
#include <cstdio>

namespace
{
	PROCESS_POWER_THROTTLING_STATE ReadPolicy()
	{
		PROCESS_POWER_THROTTLING_STATE policy{ PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0 };
		CHECK(::GetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &policy, sizeof(policy)));
		return policy;
	}

	void CheckPolicy(const PROCESS_POWER_THROTTLING_STATE& expected)
	{
		const auto actual = ReadPolicy();
		CHECK(actual.ControlMask == expected.ControlMask);
		CHECK(actual.StateMask == expected.StateMask);
	}
}

int main()
{
	const auto original = ReadPolicy();
	// Emulate an occluded host's coarse-timer policy in this test process only.
	// Also set an unrelated policy bit to check that it survives both transitions.
	auto throttled = original;
	throttled.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION | PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	throttled.StateMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	throttled.StateMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	CHECK(::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &throttled, sizeof(throttled)));
	auto active = throttled;
	active.StateMask &= ~PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	{
		osfui::webview2::CaptureTiming timing;
		CHECK(timing.Disable() == ERROR_SUCCESS);
		CheckPolicy(throttled);
		for (int i = 0; i < 3; ++i) {
			const auto result = timing.Enable();
			CHECK(result.timer == TIMERR_NOERROR);
			CHECK(result.policyError == ERROR_SUCCESS);
			CHECK(timing.Enable().policyError == ERROR_SUCCESS);
			CheckPolicy(active);
			CHECK(timing.Disable() == ERROR_SUCCESS);
			CHECK(timing.Disable() == ERROR_SUCCESS);
			CheckPolicy(throttled);
		}
		CHECK(timing.Enable().policyError == ERROR_SUCCESS);
		CheckPolicy(active);
	} // Early host teardown must also restore the original process policy.
	CheckPolicy(throttled);
	auto restore = original;
	CHECK(::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &restore, sizeof(restore)));
	{
		osfui::webview2::CaptureTiming timing;
		CHECK(timing.Enable().policyError == ERROR_SUCCESS);
	}
	CheckPolicy(original);
	std::printf("Capture timing: %d checks, %d failures (visibility cycles, idempotence, policy preservation, teardown)\n",
		g_checks, g_failures);
	return g_failures ? 1 : 0;
}
