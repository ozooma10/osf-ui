#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace OSFUI
{
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

		// Retry only known temporary helper failures. Raw HRESULTs avoid a COM dependency in tests.
		[[nodiscard]] static constexpr bool IsRetryable(std::string_view a_stage, std::uint32_t a_code)
		{
			if (a_stage == "host-connection") return true;
			if (a_stage != "capture-ring" && a_stage != "capture-frame" &&
				a_stage != "capture-signal" && a_stage != "capture-recreate" &&
				a_stage != "capture-resize" && a_stage != "capture-setup" &&
				a_stage != "cef-graphics" && a_stage != "graphics-init") return false;
			switch (a_code) {
			case 0x8007000Eu: // E_OUTOFMEMORY
			case 0x887A0005u: // DXGI_ERROR_DEVICE_REMOVED (helper device only)
			case 0x887A0007u: // DXGI_ERROR_DEVICE_RESET
			case 0x887A0020u: // DXGI_ERROR_DRIVER_INTERNAL_ERROR
				return true;
			default:
				return false;
			}
		}

		void Reset()
		{
			m_phase = Phase::Idle;
			m_attempts = 0;
			m_retryAt = 0.0;
			m_responseDeadline = 0.0;
			m_healthyUntil = 0.0;
			m_failureHandled = false;
		}

		// A successful load does not reset retries; wait until the host stays healthy.
		void OnResponse(double a_now)
		{
			if (!CanAcceptResponse()) return;
			m_phase = Phase::Healthy;
			m_healthyUntil = a_now + 60.0;
			m_failureHandled = false;
		}

		void ObserveHealth(double a_now)
		{
			if (m_phase == Phase::Healthy && a_now >= m_healthyUntil) Reset();
		}

		// Ignore duplicates. A permanent error overrides a disconnect, regardless of arrival order.
		[[nodiscard]] bool OnFailure(double a_now, bool a_retryable)
		{
			if (m_failureHandled && (a_retryable || m_phase == Phase::Disabled)) {
				return false;
			}
			m_failureHandled = true;
			if (a_retryable) {
				Schedule(a_now);
			} else {
				m_phase = Phase::Disabled;
				m_retryAt = 0.0;
				m_responseDeadline = 0.0;
			}
			return true;
		}

		[[nodiscard]] bool BeginDueAttempt(double a_now)
		{
			if (m_phase != Phase::Waiting || a_now < m_retryAt) {
				return false;
			}
			++m_attempts;
			m_phase = Phase::AwaitingResponse;
			m_responseDeadline = a_now + kResponseTimeoutSeconds;
			m_failureHandled = false;  // accept failures from the replacement while UI remains blocked
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

		// A retiring helper cannot produce a replacement response. Keep the
		// attempt budget intact and give startup its full timeout after retirement.
		void DeferResponseWait(double a_now)
		{
			if (m_phase == Phase::AwaitingResponse)
				m_responseDeadline = a_now + kResponseTimeoutSeconds;
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

		// Recovery permits UI use before a failure and after a replacement responds.
		[[nodiscard]] bool IsAvailable() const
		{
			return m_phase == Phase::Idle || m_phase == Phase::Healthy;
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
		bool          m_failureHandled{ false };
		std::uint32_t m_attempts{ 0 };
		double        m_retryAt{ 0.0 };
		double        m_responseDeadline{ 0.0 };
		double        m_healthyUntil{ 0.0 };
	};
}
