
#include "Render/BrowserHostRecovery.h"

#include <cassert>
#include <iostream>

using OSFUI::BrowserHostRecovery;

int main()
{
	using Phase = BrowserHostRecovery::Phase;

	BrowserHostRecovery recovery;
	assert(recovery.PhaseValue() == Phase::Idle);
	assert(recovery.IsAvailable());
	assert(!recovery.BeginDueAttempt(100.0));

	// Initial loss waits one second, then starts attempt 1.
	assert(recovery.OnFailure(100.0, true));
	assert(recovery.PhaseValue() == Phase::Waiting);
	assert(!recovery.IsAvailable());
	// Duplicate notifications neither postpone the attempt nor change its classification.
	assert(!recovery.OnFailure(100.5, true));
	assert(!recovery.OnFailure(100.5, false));
	recovery.OnResponse(100.5);
	assert(!recovery.IsAvailable());
	assert(!recovery.BeginDueAttempt(100.99));
	assert(recovery.BeginDueAttempt(101.0));
	assert(recovery.Attempts() == 1);
	assert(recovery.PhaseValue() == Phase::AwaitingResponse);
	assert(!recovery.IsAvailable());

	// A silent replacement times out, then observes the second backoff.
	assert(!recovery.ExpireResponseWait(130.99));
	assert(recovery.ExpireResponseWait(131.0));
	assert(recovery.PhaseValue() == Phase::Waiting);
	recovery.OnResponse(132.0);  // a late response cannot restore availability
	assert(!recovery.IsAvailable());
	assert(!recovery.BeginDueAttempt(133.99));
	assert(recovery.BeginDueAttempt(134.0));
	assert(recovery.Attempts() == 2);

	// A replacement's failure is handled even though UI use is still blocked.
	assert(recovery.OnFailure(134.5, true));
	assert(!recovery.OnFailure(135.0, true));
	assert(!recovery.BeginDueAttempt(144.49));
	assert(recovery.BeginDueAttempt(144.5));
	assert(recovery.Attempts() == 3);
	assert(recovery.OnFailure(145.0, true));
	assert(recovery.PhaseValue() == Phase::Exhausted);
	assert(!recovery.CanAcceptResponse());
	assert(!recovery.IsAvailable());

	assert(recovery.RequestManualRetry(200.0));
	assert(!recovery.IsAvailable());
	assert(!recovery.OnFailure(200.0, false));  // still the old host until the attempt begins
	assert(recovery.BeginDueAttempt(200.0));
	assert(recovery.Attempts() == 1);
	assert(!recovery.IsAvailable());
	assert(recovery.OnFailure(200.5, true));
	recovery.Reset();
	assert(recovery.PhaseValue() == Phase::Idle);
	assert(recovery.IsAvailable());
	assert(recovery.Attempts() == 0);
	assert(!recovery.CanAcceptResponse());

	// Brief load/crash loops retain the budget; only a stable minute resets it.
	for (unsigned attempt = 1; attempt <= BrowserHostRecovery::kMaxAttempts; ++attempt) {
		assert(recovery.OnFailure(300.0 + attempt * 100, true));
		assert(!recovery.IsAvailable());
		assert(recovery.BeginDueAttempt(320.0 + attempt * 100));
		recovery.OnResponse(321.0 + attempt * 100);
		assert(recovery.IsAvailable());
		recovery.ObserveHealth(322.0 + attempt * 100);
		assert(recovery.Attempts() == attempt);
	}
	assert(recovery.OnFailure(700.0, true));
	assert(recovery.PhaseValue() == Phase::Exhausted);
	assert(!recovery.IsAvailable());
	assert(recovery.RequestManualRetry(800.0));
	assert(recovery.BeginDueAttempt(800.0));
	recovery.OnResponse(801.0);
	recovery.ObserveHealth(860.9);
	assert(recovery.Attempts() == 1);
	recovery.ObserveHealth(861.0);
	assert(recovery.Attempts() == 0 && recovery.PhaseValue() == Phase::Idle);
	assert(recovery.IsAvailable());

	// Non-retryable renderer/security failures cannot be overridden by an open.
	assert(recovery.OnFailure(900.0, false));
	assert(!recovery.OnFailure(900.5, true));
	assert(!recovery.OnFailure(900.5, false));
	assert(recovery.PhaseValue() == Phase::Disabled);
	assert(!recovery.IsAvailable());
	assert(!recovery.RequestManualRetry(901.0));
	recovery.OnResponse(902.0);
	assert(!recovery.IsAvailable());
	assert(!recovery.BeginDueAttempt(1000.0));

	// A timeout schedules a retry but does not consume the current host's first failure.
	recovery.Reset();
	assert(recovery.OnFailure(1000.0, true));
	assert(recovery.BeginDueAttempt(1001.0));
	assert(recovery.ExpireResponseWait(1031.0));
	assert(recovery.OnFailure(1032.0, false));
	assert(recovery.PhaseValue() == Phase::Disabled);
	assert(!recovery.IsAvailable());
	assert(!recovery.BeginDueAttempt(1040.0));

	std::cout << "browser_host_recovery_tests: ok\n";
	return 0;
}
