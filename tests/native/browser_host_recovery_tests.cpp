
#include "Render/BrowserHostRecovery.h"

#include <cassert>
#include <iostream>

using OSFUI::BrowserHostRecovery;

int main()
{
	using Phase = BrowserHostRecovery::Phase;

	BrowserHostRecovery recovery;
	assert(recovery.PhaseValue() == Phase::Idle);
	assert(!recovery.BeginDueAttempt(100.0));

	// Initial loss waits one second, then starts attempt 1.
	recovery.OnRetryableFailure(100.0);
	assert(recovery.PhaseValue() == Phase::Waiting);
	assert(!recovery.BeginDueAttempt(100.99));
	assert(recovery.BeginDueAttempt(101.0));
	assert(recovery.Attempts() == 1);
	assert(recovery.PhaseValue() == Phase::AwaitingResponse);

	// A silent replacement times out, then observes the second backoff.
	assert(!recovery.ExpireResponseWait(130.99));
	assert(recovery.ExpireResponseWait(131.0));
	assert(recovery.PhaseValue() == Phase::Waiting);
	assert(!recovery.BeginDueAttempt(133.99));
	assert(recovery.BeginDueAttempt(134.0));
	assert(recovery.Attempts() == 2);

	recovery.OnRetryableFailure(134.5);
	assert(!recovery.BeginDueAttempt(144.49));
	assert(recovery.BeginDueAttempt(144.5));
	assert(recovery.Attempts() == 3);
	recovery.OnRetryableFailure(145.0);
	assert(recovery.PhaseValue() == Phase::Exhausted);
	assert(!recovery.CanAcceptResponse());

	assert(recovery.RequestManualRetry(200.0));
	assert(recovery.BeginDueAttempt(200.0));
	assert(recovery.Attempts() == 1);
	recovery.Reset();
	assert(recovery.PhaseValue() == Phase::Idle);
	assert(recovery.Attempts() == 0);
	assert(!recovery.CanAcceptResponse());

	// Brief load/crash loops retain the budget; only a stable minute resets it.
	for (unsigned attempt = 1; attempt <= BrowserHostRecovery::kMaxAttempts; ++attempt) {
		recovery.OnRetryableFailure(300.0 + attempt * 100);
		assert(recovery.BeginDueAttempt(320.0 + attempt * 100));
		recovery.OnResponse(321.0 + attempt * 100);
		recovery.ObserveHealth(322.0 + attempt * 100);
		assert(recovery.Attempts() == attempt);
	}
	recovery.OnRetryableFailure(700.0);
	assert(recovery.PhaseValue() == Phase::Exhausted);
	assert(recovery.RequestManualRetry(800.0));
	assert(recovery.BeginDueAttempt(800.0));
	recovery.OnResponse(801.0);
	recovery.ObserveHealth(860.9);
	assert(recovery.Attempts() == 1);
	recovery.ObserveHealth(861.0);
	assert(recovery.Attempts() == 0 && recovery.PhaseValue() == Phase::Idle);

	// Non-retryable renderer/security failures cannot be overridden by an open.
	recovery.Disable();
	recovery.OnRetryableFailure(300.0);
	assert(recovery.PhaseValue() == Phase::Disabled);
	assert(!recovery.RequestManualRetry(300.0));
	assert(!recovery.BeginDueAttempt(1000.0));

	std::cout << "browser_host_recovery_tests: ok\n";
	return 0;
}
