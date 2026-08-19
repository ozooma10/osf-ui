
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
	assert(recovery.CanAcceptResponse());

	assert(recovery.RequestManualRetry(200.0));
	assert(recovery.BeginDueAttempt(200.0));
	assert(recovery.Attempts() == 1);
	recovery.Reset();
	assert(recovery.PhaseValue() == Phase::Idle);
	assert(recovery.Attempts() == 0);
	assert(!recovery.CanAcceptResponse());

	// Non-retryable renderer/security failures cannot be overridden by an open.
	recovery.Disable();
	recovery.OnRetryableFailure(300.0);
	assert(recovery.PhaseValue() == Phase::Disabled);
	assert(!recovery.RequestManualRetry(300.0));
	assert(!recovery.BeginDueAttempt(1000.0));

	std::cout << "browser_host_recovery_tests: ok\n";
	return 0;
}
