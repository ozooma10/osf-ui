
#include "Render/BrowserHostRecovery.h"

#include "check.h"
#include <iostream>

using OSFUI::BrowserHostRecovery;

int main()
{
	using Phase = BrowserHostRecovery::Phase;
	using namespace std::literals;

	// Retry only explicit graphics failures that replacing either helper can repair.
	CHECK(BrowserHostRecovery::IsRetryable("host-connection", 0));
	for (const auto stage : { "capture-ring"sv, "capture-frame"sv, "capture-signal"sv,
		"capture-recreate"sv, "capture-resize"sv, "capture-setup"sv,
		"cef-graphics"sv, "graphics-init"sv }) {
		for (const auto code : { 0x8007000Eu, 0x887A0005u, 0x887A0007u, 0x887A0020u }) {
			CHECK(BrowserHostRecovery::IsRetryable(stage, code));
		}
		for (const auto code : { 0u, 0x80004005u, 0x80070005u, 0x80070057u, 0x80004002u }) {
			CHECK(!BrowserHostRecovery::IsRetryable(stage, code));
		}
	}
	for (const auto stage : { "environment"sv, "composition-controller"sv, "browser-security"sv,
		"game-device"sv, "host-handshake"sv, "cef-bridge"sv, "unknown"sv }) {
		CHECK(!BrowserHostRecovery::IsRetryable(stage, 0x887A0005u));
	}

	// Repeated capture faults and their disconnect notifications share one budget.
	BrowserHostRecovery capture;
	CHECK(capture.OnFailure(0.0, BrowserHostRecovery::IsRetryable("capture-frame", 0x887A0005u)));
	for (unsigned attempt = 1; attempt <= BrowserHostRecovery::kMaxAttempts; ++attempt) {
		CHECK(!capture.OnFailure(attempt * 100.0, BrowserHostRecovery::IsRetryable("host-connection", 0)));
		CHECK(capture.BeginDueAttempt(attempt * 100.0));
		CHECK(capture.Attempts() == attempt);
		capture.OnResponse(attempt * 100.0 + 1.0);
		CHECK(capture.OnFailure(attempt * 100.0 + 2.0, BrowserHostRecovery::IsRetryable("capture-ring", 0x8007000Eu)));
	}
	CHECK(capture.PhaseValue() == Phase::Exhausted);
	CHECK(!capture.IsAvailable());

	// A pipe-writer failure may arrive before the helper's permanent error.
	BrowserHostRecovery permanent;
	CHECK(permanent.OnFailure(0.0, BrowserHostRecovery::IsRetryable("host-connection", 0)));
	CHECK(permanent.OnFailure(0.1, BrowserHostRecovery::IsRetryable("browser-security", 0x80070005u)));
	CHECK(permanent.PhaseValue() == Phase::Disabled);
	CHECK(!permanent.OnFailure(0.2, true));
	CHECK(!permanent.BeginDueAttempt(10.0));
	CHECK(capture.RequestManualRetry(400.0));
	CHECK(capture.BeginDueAttempt(400.0));
	// The game device can fail while a restart is waiting; the attempt must stop.
	CHECK(capture.OnFailure(400.0, BrowserHostRecovery::IsRetryable("game-device", 0x887A0005u)));
	CHECK(capture.PhaseValue() == Phase::Disabled);
	CHECK(!capture.RequestManualRetry(401.0));
	capture.OnResponse(402.0);
	CHECK(!capture.IsAvailable());

	BrowserHostRecovery recovery;
	CHECK(recovery.PhaseValue() == Phase::Idle);
	CHECK(recovery.IsAvailable());
	CHECK(!recovery.BeginDueAttempt(100.0));

	// Initial loss waits one second, then starts attempt 1.
	CHECK(recovery.OnFailure(100.0, true));
	CHECK(recovery.PhaseValue() == Phase::Waiting);
	CHECK(!recovery.IsAvailable());
	// Duplicate retryable notifications do not postpone the attempt.
	CHECK(!recovery.OnFailure(100.5, true));
	recovery.OnResponse(100.5);
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.BeginDueAttempt(100.99));
	CHECK(recovery.BeginDueAttempt(101.0));
	CHECK(recovery.Attempts() == 1);
	CHECK(recovery.PhaseValue() == Phase::AwaitingResponse);
	CHECK(!recovery.IsAvailable());

	// A silent replacement times out, then observes the second backoff.
	CHECK(!recovery.ExpireResponseWait(130.99));
	CHECK(recovery.ExpireResponseWait(131.0));
	CHECK(recovery.PhaseValue() == Phase::Waiting);
	recovery.OnResponse(132.0);  // a late response cannot restore availability
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.BeginDueAttempt(133.99));
	CHECK(recovery.BeginDueAttempt(134.0));
	CHECK(recovery.Attempts() == 2);

	// A replacement's failure is handled even though UI use is still blocked.
	CHECK(recovery.OnFailure(134.5, true));
	CHECK(!recovery.OnFailure(135.0, true));
	CHECK(!recovery.BeginDueAttempt(144.49));
	CHECK(recovery.BeginDueAttempt(144.5));
	CHECK(recovery.Attempts() == 3);
	CHECK(recovery.OnFailure(145.0, true));
	CHECK(recovery.PhaseValue() == Phase::Exhausted);
	CHECK(!recovery.CanAcceptResponse());
	CHECK(!recovery.IsAvailable());

	CHECK(recovery.RequestManualRetry(200.0));
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.OnFailure(200.0, true));  // still the old host until the attempt begins
	CHECK(recovery.BeginDueAttempt(200.0));
	CHECK(recovery.Attempts() == 1);
	CHECK(!recovery.IsAvailable());
	CHECK(recovery.OnFailure(200.5, true));
	recovery.Reset();
	CHECK(recovery.PhaseValue() == Phase::Idle);
	CHECK(recovery.IsAvailable());
	CHECK(recovery.Attempts() == 0);
	CHECK(!recovery.CanAcceptResponse());

	// Brief load/crash loops retain the budget; only a stable minute resets it.
	for (unsigned attempt = 1; attempt <= BrowserHostRecovery::kMaxAttempts; ++attempt) {
		CHECK(recovery.OnFailure(300.0 + attempt * 100, true));
		CHECK(!recovery.IsAvailable());
		CHECK(recovery.BeginDueAttempt(320.0 + attempt * 100));
		recovery.OnResponse(321.0 + attempt * 100);
		CHECK(recovery.IsAvailable());
		recovery.ObserveHealth(322.0 + attempt * 100);
		CHECK(recovery.Attempts() == attempt);
	}
	CHECK(recovery.OnFailure(700.0, true));
	CHECK(recovery.PhaseValue() == Phase::Exhausted);
	CHECK(!recovery.IsAvailable());
	CHECK(recovery.RequestManualRetry(800.0));
	CHECK(recovery.BeginDueAttempt(800.0));
	recovery.OnResponse(801.0);
	recovery.ObserveHealth(860.9);
	CHECK(recovery.Attempts() == 1);
	recovery.ObserveHealth(861.0);
	CHECK(recovery.Attempts() == 0 && recovery.PhaseValue() == Phase::Idle);
	CHECK(recovery.IsAvailable());

	// Non-retryable renderer/security failures cannot be overridden by an open.
	CHECK(recovery.OnFailure(900.0, false));
	CHECK(!recovery.OnFailure(900.5, true));
	CHECK(!recovery.OnFailure(900.5, false));
	CHECK(recovery.PhaseValue() == Phase::Disabled);
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.RequestManualRetry(901.0));
	recovery.OnResponse(902.0);
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.BeginDueAttempt(1000.0));

	// A timeout schedules a retry but does not consume the current host's first failure.
	recovery.Reset();
	CHECK(recovery.OnFailure(1000.0, true));
	CHECK(recovery.BeginDueAttempt(1001.0));
	CHECK(recovery.ExpireResponseWait(1031.0));
	CHECK(recovery.OnFailure(1032.0, false));
	CHECK(recovery.PhaseValue() == Phase::Disabled);
	CHECK(!recovery.IsAvailable());
	CHECK(!recovery.BeginDueAttempt(1040.0));

	// Slow retirement (for example, waiting for view-file preparation) does not
	// consume retries before a replacement has even started.
	recovery.Reset();
	CHECK(recovery.OnFailure(0.0, true));
	CHECK(recovery.BeginDueAttempt(1.0));
	for (double now : {31.0, 100.0, 300.0}) {
		recovery.DeferResponseWait(now);
		CHECK(!recovery.ExpireResponseWait(now));
		CHECK(!recovery.BeginDueAttempt(now));
		CHECK(recovery.Attempts() == 1 && recovery.PhaseValue() == Phase::AwaitingResponse);
	}
	CHECK(!recovery.ExpireResponseWait(329.99));
	CHECK(recovery.ExpireResponseWait(330.0)); // Full response budget after retirement.
	CHECK(recovery.Attempts() == 1 && recovery.PhaseValue() == Phase::Waiting);
	recovery.DeferResponseWait(400.0); // Deferral cannot alter retry backoff or failures.
	CHECK(recovery.BeginDueAttempt(400.0) && recovery.Attempts() == 2);
	CHECK(recovery.OnFailure(401.0, false));
	recovery.DeferResponseWait(500.0);
	CHECK(recovery.PhaseValue() == Phase::Disabled && !recovery.ExpireResponseWait(600.0));

	std::cout << "browser_host_recovery_tests: " << g_checks << " checks, " << g_failures << " failures\n";
	return g_failures;
}
