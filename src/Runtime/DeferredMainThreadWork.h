#pragma once

#include <atomic>

namespace OSFUI
{
	// Coalesced cross-thread work consumed only at the main-thread checkpoint.
	class DeferredMainThreadWork
	{
	public:
		void Request() noexcept
		{
			_pending.store(true, std::memory_order_release);
		}

		[[nodiscard]] bool Take() noexcept
		{
			return _pending.exchange(false, std::memory_order_acq_rel);
		}

	private:
		std::atomic_bool _pending{ false };
	};
}
