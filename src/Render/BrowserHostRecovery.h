#pragma once

#include <array>
#include <cstdint>

namespace OSFUI
{
	// Main-thread-only retry policy for a terminal browser-host connection
	// failure. Transport teardown/restart stays in the web renderer; this class owns
	// only bounded timing and the manual retry escape hatch.
	class BrowserHostRecovery
	{
	public:
		enum class Phase
		{
			Idle,
			Waiting,
			AwaitingResponse,
			Exhausted,
			Disabled,
		};

		static constexpr std::uint32_t kMaxAttempts = 3;
		static constexpr double        kResponseTimeoutSeconds = 30.0;

		void Reset()
		{
			_phase = Phase::Idle;
			_attempts = 0;
			_retryAt = 0.0;
			_responseDeadline = 0.0;
		}

		void Disable()
		{
			_phase = Phase::Disabled;
			_retryAt = 0.0;
			_responseDeadline = 0.0;
		}

		void OnRetryableFailure(double a_now)
		{
			if (_phase == Phase::Disabled) {
				return;
			}
			Schedule(a_now);
		}

		[[nodiscard]] bool BeginDueAttempt(double a_now)
		{
			if (_phase != Phase::Waiting || a_now < _retryAt) {
				return false;
			}
			++_attempts;
			_phase = Phase::AwaitingResponse;
			_responseDeadline = a_now + kResponseTimeoutSeconds;
			return true;
		}

		void OnAttemptSetupFailed(double a_now)
		{
			if (_phase == Phase::AwaitingResponse) {
				Schedule(a_now);
			}
		}

		[[nodiscard]] bool ExpireResponseWait(double a_now)
		{
			if (_phase != Phase::AwaitingResponse || a_now < _responseDeadline) {
				return false;
			}
			Schedule(a_now);
			return true;
		}

		// An explicit open after the automatic budget is spent starts a fresh
		// cycle immediately. It does not itself reopen the overlay.
		[[nodiscard]] bool RequestManualRetry(double a_now)
		{
			if (_phase != Phase::Exhausted) {
				return false;
			}
			_attempts = 0;
			_phase = Phase::Waiting;
			_retryAt = a_now;
			_responseDeadline = 0.0;
			return true;
		}

		[[nodiscard]] bool CanAcceptResponse() const
		{
			return _phase == Phase::Waiting ||
			       _phase == Phase::AwaitingResponse ||
			       _phase == Phase::Exhausted;
		}

		[[nodiscard]] Phase PhaseValue() const { return _phase; }
		[[nodiscard]] std::uint32_t Attempts() const { return _attempts; }

	private:
		void Schedule(double a_now)
		{
			_responseDeadline = 0.0;
			if (_attempts >= kMaxAttempts) {
				_phase = Phase::Exhausted;
				_retryAt = 0.0;
				return;
			}
			_phase = Phase::Waiting;
			_retryAt = a_now + kBackoffSeconds[_attempts];
		}

		static constexpr std::array<double, kMaxAttempts> kBackoffSeconds{
			1.0, 3.0, 10.0
		};

		Phase         _phase{ Phase::Idle };
		std::uint32_t _attempts{ 0 };
		double        _retryAt{ 0.0 };
		double        _responseDeadline{ 0.0 };
	};
}
