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
			m_phase = Phase::Idle;
			m_attempts = 0;
			m_retryAt = 0.0;
			m_responseDeadline = 0.0;
		}

		// A load proves responsiveness, not stability. Keep the retry budget until the host stays healthy.
		void OnResponse(double a_now)
		{
			if (!CanAcceptResponse()) return;
			m_phase = Phase::Healthy;
			m_healthyUntil = a_now + 60.0;
		}

		void ObserveHealth(double a_now)
		{
			if (m_phase == Phase::Healthy && a_now >= m_healthyUntil) Reset();
		}

		void Disable()
		{
			m_phase = Phase::Disabled;
			m_retryAt = 0.0;
			m_responseDeadline = 0.0;
		}

		void OnRetryableFailure(double a_now)
		{
			if (m_phase == Phase::Disabled) {
				return;
			}
			Schedule(a_now);
		}

		[[nodiscard]] bool BeginDueAttempt(double a_now)
		{
			if (m_phase != Phase::Waiting || a_now < m_retryAt) {
				return false;
			}
			++m_attempts;
			m_phase = Phase::AwaitingResponse;
			m_responseDeadline = a_now + kResponseTimeoutSeconds;
			return true;
		}

		[[nodiscard]] bool ExpireResponseWait(double a_now)
		{
			if (m_phase != Phase::AwaitingResponse || a_now < m_responseDeadline) {
				return false;
			}
			Schedule(a_now);
			return true;
		}

		// Manual retry resets the exhausted budget but does not reopen the overlay.
		[[nodiscard]] bool RequestManualRetry(double a_now)
		{
			if (m_phase != Phase::Exhausted) {
				return false;
			}
			m_attempts = 0;
			m_phase = Phase::Waiting;
			m_retryAt = a_now;
			m_responseDeadline = 0.0;
			return true;
		}

		[[nodiscard]] bool CanAcceptResponse() const
		{
			return m_phase == Phase::AwaitingResponse;
		}

		[[nodiscard]] Phase PhaseValue() const { return m_phase; }
		[[nodiscard]] std::uint32_t Attempts() const { return m_attempts; }

	private:
		void Schedule(double a_now)
		{
			m_responseDeadline = 0.0;
			if (m_attempts >= kMaxAttempts) {
				m_phase = Phase::Exhausted;
				m_retryAt = 0.0;
				return;
			}
			m_phase = Phase::Waiting;
			m_retryAt = a_now + kBackoffSeconds[m_attempts];
		}

		static constexpr std::array<double, kMaxAttempts> kBackoffSeconds{
			1.0, 3.0, 10.0
		};

		Phase         m_phase{ Phase::Idle };
		std::uint32_t m_attempts{ 0 };
		double        m_retryAt{ 0.0 };
		double        m_responseDeadline{ 0.0 };
		double        m_healthyUntil{ 0.0 };
	};
}
