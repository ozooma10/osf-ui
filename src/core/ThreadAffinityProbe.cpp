#include "core/ThreadAffinityProbe.h"

#include "core/Log.h"

#include "RE/B/BSService.h"

// Keep <Windows.h> confined to this file. NOGDI stops wingdi's ERROR macro from
// clobbering REX::ERROR. We only need GetCurrentThreadId / RtlCaptureStackBackTrace
// / GetModuleHandleW.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace OSFUI::ThreadProbe
{
	namespace
	{
		std::atomic<std::uint32_t> g_mainLoopTid{ 0 };
		std::atomic<std::uint32_t> g_sfseTaskTid{ 0 };

		// Sample budgets: enough to show stability/variance across frames, not
		// enough to spam the log at menu-uncapped frame rates.
		std::atomic<int> g_sfseSamples{ 0 };
		std::atomic<int> g_enginePosts{ 0 };
		constexpr int    kSfseSampleBudget = 3;
		constexpr int    kEnginePostBudget = 3;

		// One module-relative backtrace per source, so the render-graph vs
		// main-loop frames are captured deterministically — the trainwreck stack,
		// made non-fatal. Compare the printed offsets against the addresses in
		// MainThreadMenuPump.h (main loop 0x141890c60 -> module+0x1890c60).
		void LogBacktrace(const char* a_tag)
		{
			void*     frames[32];
			const auto n = ::RtlCaptureStackBackTrace(0, 32, frames, nullptr);
			const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
			REX::INFO("ThreadProbe[{}]: stack ({} frames, Starfield.exe-relative):", a_tag, n);
			for (unsigned short i = 0; i < n; ++i) {
				const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
				REX::INFO("    #{:02} Starfield.exe+{:#x}", i, addr - base);
			}
		}
	}

	void NoteMainLoop()
	{
		if (!Log::DevMode()) {
			return;
		}
		// Fast path once recorded: the main loop is a single thread, so a change
		// would itself be a finding — but we don't need GetCurrentThreadId every
		// frame to learn that.
		if (g_mainLoopTid.load(std::memory_order_relaxed) != 0) {
			return;
		}
		const auto    tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
		std::uint32_t expected = 0;
		if (g_mainLoopTid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
			REX::INFO("ThreadProbe[main-loop]: UI_AdvanceActiveMenus runs on tid={} (Scaleform/AS3 owner)", tid);
		}
	}

	void NoteSfseTask()
	{
		if (!Log::DevMode()) {
			return;
		}
		const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
		g_sfseTaskTid.store(tid, std::memory_order_relaxed);

		// Don't spend the sample budget until the main-loop anchor exists, or the
		// same= comparison is meaningless (mainLoopTid=0). The pump arms and starts
		// publishing the anchor shortly after load, so this defers sampling to
		// steady state.
		if (g_mainLoopTid.load(std::memory_order_acquire) == 0) {
			return;
		}
		if (g_sfseSamples.fetch_add(1, std::memory_order_relaxed) >= kSfseSampleBudget) {
			return;
		}
		const auto mainTid = g_mainLoopTid.load(std::memory_order_acquire);
		REX::INFO("ThreadProbe[sfse-task]: FrameTick runs on tid={} | mainLoopTid={} | same={}",
			tid, mainTid, (mainTid != 0 && tid == mainTid) ? "YES" : "no");
		LogBacktrace("sfse-task");
	}

	void ProbeEngineQueue()
	{
		if (!Log::DevMode()) {
			return;
		}
		// Wait for the main-loop anchor before posting, so the drain's same=
		// comparison against the main thread is meaningful (and steady-state, not
		// early-boot before the queue's drainer is even running).
		if (g_mainLoopTid.load(std::memory_order_acquire) == 0) {
			return;
		}
		const int n = g_enginePosts.fetch_add(1, std::memory_order_relaxed);
		if (n >= kEnginePostBudget) {
			return;
		}
		auto* queue = RE::BSService::TaskQueue::GetSingleton();
		if (!queue) {
			REX::INFO("ThreadProbe[engine-queue #{}]: TaskQueue singleton null (early boot?) — skipped", n);
			return;
		}
		const auto postTid = static_cast<std::uint32_t>(::GetCurrentThreadId());
		// Fire-and-forget: AddTask owns the delegate and runs it on the next
		// budgeted drain (or inline right here if this very thread is the drainer
		// / queueing is disabled — which the inline=YES line below reveals).
		queue->AddTask([postTid, n]() {
			const auto drainTid = static_cast<std::uint32_t>(::GetCurrentThreadId());
			const auto mainTid = g_mainLoopTid.load(std::memory_order_acquire);
			const auto sfseTid = g_sfseTaskTid.load(std::memory_order_relaxed);
			REX::INFO("ThreadProbe[engine-queue #{}]: BSService drain tid={} | mainLoopTid={} same={} | "
					  "postTid={} inline={} | sfseTaskTid={} same={}",
				n, drainTid,
				mainTid, (mainTid != 0 && drainTid == mainTid) ? "YES" : "no",
				postTid, (drainTid == postTid) ? "YES" : "no",
				sfseTid, (sfseTid != 0 && drainTid == sfseTid) ? "YES" : "no");
			LogBacktrace("engine-queue");
		});
	}
}
