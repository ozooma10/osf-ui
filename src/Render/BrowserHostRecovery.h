#pragma once

#include <array>
#include <cstdint>

namespace OSFUI
{
	// Main-thread retry timing for terminal browser-host failures; transport ownership stays in the renderer.
	class BrowserHostRecovery
	{
	public:
		enum class Phase
		{
			Idle,
			Waiting,
			AwaitingResponse,
			Healthy,
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

		// A load proves responsiveness, not stability. Keep the retry budget until the host stays healthy.
		void OnResponse(double a_now)
		{
			if (!CanAcceptResponse()) return;
			_phase = Phase::Healthy;
			_healthyUntil = a_now + 60.0;
		}

		void ObserveHealth(double a_now)
		{
			if (_phase == Phase::Healthy && a_now >= _healthyUntil) Reset();
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

		[[nodiscard]] bool ExpireResponseWait(double a_now)
		{
			if (_phase != Phase::AwaitingResponse || a_now < _responseDeadline) {
				return false;
			}
			Schedule(a_now);
			return true;
		}

		// Manual retry resets the exhausted budget but does not reopen the overlay.
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
			return _phase == Phase::AwaitingResponse;
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
		double        _healthyUntil{ 0.0 };
	};
}
