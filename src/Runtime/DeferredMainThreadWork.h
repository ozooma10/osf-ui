#pragma once

#include <atomic>

namespace OSFUI
{
	// Cross-thread notification for work that must be consumed at a proven
	// main-thread checkpoint. Multiple requests coalesce; a request racing with
	// Take() is either consumed by that call or remains pending for the next one.
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
