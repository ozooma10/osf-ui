#include "core/MainThreadLatch.h"

#include "RE/B/BSService.h"

namespace OSFUI
{
	void MainThreadLatch::Request(bool a_desired, ApplyFn a_apply)
	{
		_desired.store(a_desired, std::memory_order_release);

		// Steady state: nothing to do. Cheap, allocation-free — this is the path
		// taken on almost every tick.
		if (a_desired == _applied.load(std::memory_order_acquire)) {
			return;
		}
		// Coalesce: one task in flight at a time. It reads the latest _desired when
		// it drains, so rapid flips collapse to a single main-thread apply.
		if (_pending.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		auto* queue = RE::BSService::TaskQueue::GetSingleton();
		if (!queue) {
			// Engine not up yet (early boot). Drop the guard and retry next tick.
			_pending.store(false, std::memory_order_release);
			return;
		}

		// Runs on the game main thread during the next per-frame drain (or inline
		// here if this very thread is the drainer — still the main thread).
		queue->AddTask([this, a_apply]() {
			_pending.store(false, std::memory_order_release);
			const bool want = _desired.load(std::memory_order_acquire);
			if (want == _applied.load(std::memory_order_relaxed)) {
				return;  // a later Request already superseded this one
			}
			if (a_apply(want)) {
				_applied.store(want, std::memory_order_release);
			}
			// else: not committed (singleton not ready) — leave _applied so the
			// next tick's Request re-posts.
		});
	}
}
