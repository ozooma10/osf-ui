#include "core/BenchStats.h"

#include <array>

namespace OSFUI::bench
{
	namespace
	{
		// 0.04 ms buckets to ~40.9 ms; slower samples land in the last bucket
		// (percentile then reads "≥ 40.9"), true max is tracked exactly.
		constexpr std::size_t kBuckets = 1024;
		constexpr double      kMsPerBucket = 0.04;

		constexpr std::string_view kChannelNames[]{ "frame", "tick", "present", "produce" };

		struct Accum
		{
			std::mutex                         mutex;
			std::uint64_t                      count{ 0 };
			double                             sum{ 0.0 };
			double                             max{ 0.0 };
			std::array<std::uint32_t, kBuckets> hist{};

			void Add(double a_ms)
			{
				const auto bucket = std::min<std::size_t>(
					kBuckets - 1, static_cast<std::size_t>(std::max(a_ms, 0.0) / kMsPerBucket));
				std::lock_guard lk(mutex);
				++count;
				sum += a_ms;
				max = std::max(max, a_ms);
				++hist[bucket];
			}
		};

		struct Snapshot
		{
			std::uint64_t                       count{ 0 };
			double                              sum{ 0.0 };
			double                              max{ 0.0 };
			std::array<std::uint32_t, kBuckets> hist{};

			[[nodiscard]] double Percentile(double a_p) const
			{
				if (count == 0) {
					return 0.0;
				}
				const auto target = static_cast<std::uint64_t>(a_p * static_cast<double>(count - 1)) + 1;
				std::uint64_t seen = 0;
				for (std::size_t i = 0; i < kBuckets; ++i) {
					seen += hist[i];
					if (seen >= target) {
						return (static_cast<double>(i) + 0.5) * kMsPerBucket;
					}
				}
				return max;
			}
		};

		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_visible{ false };
		std::atomic<bool> g_flushNow{ false };

		Accum g_accums[static_cast<std::size_t>(Channel::kCount)];

		std::atomic<std::uint64_t> g_produced{ 0 };
		std::atomic<std::uint64_t> g_uploaded{ 0 };

		Snapshot Take(Accum& a_accum)
		{
			Snapshot s;
			std::lock_guard lk(a_accum.mutex);
			s.count = a_accum.count;
			s.sum = a_accum.sum;
			s.max = a_accum.max;
			s.hist = a_accum.hist;
			a_accum.count = 0;
			a_accum.sum = 0.0;
			a_accum.max = 0.0;
			a_accum.hist.fill(0);
			return s;
		}

		void Flush(bool a_visible, double a_windowSeconds)
		{
			for (std::size_t i = 0; i < static_cast<std::size_t>(Channel::kCount); ++i) {
				const auto s = Take(g_accums[i]);
				if (s.count == 0) {
					continue;
				}
				REX::INFO("Bench: ch={} vis={} win={:.2f}s n={} avg={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f}",
					kChannelNames[i], a_visible ? 1 : 0, a_windowSeconds, s.count,
					s.sum / static_cast<double>(s.count),
					s.Percentile(0.50), s.Percentile(0.95), s.Percentile(0.99), s.max);
			}
			const auto produced = g_produced.exchange(0);
			const auto uploaded = g_uploaded.exchange(0);
			if ((produced || uploaded) && a_windowSeconds > 0.0) {
				REX::INFO("Bench: rates vis={} win={:.2f}s produced={} ({:.1f}/s) uploaded={} ({:.1f}/s)",
					a_visible ? 1 : 0, a_windowSeconds, produced,
					static_cast<double>(produced) / a_windowSeconds, uploaded,
					static_cast<double>(uploaded) / a_windowSeconds);
			}
		}
	}

	void SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled, std::memory_order_relaxed);
	}

	bool Enabled()
	{
		return g_enabled.load(std::memory_order_relaxed);
	}

	void SetVisible(bool a_visible)
	{
		if (g_visible.exchange(a_visible, std::memory_order_relaxed) != a_visible) {
			// Edge: ask the main thread to flush the ending window immediately
			// (labelled with the OLD state) so open/closed samples never mix.
			g_flushNow.store(true, std::memory_order_relaxed);
		}
	}

	void Add(Channel a_channel, double a_ms)
	{
		if (!Enabled()) {
			return;
		}
		g_accums[static_cast<std::size_t>(a_channel)].Add(a_ms);
	}

	void CountProduced()
	{
		if (Enabled()) {
			g_produced.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void CountUploaded()
	{
		if (Enabled()) {
			g_uploaded.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void FlushIfDue()
	{
		if (!Enabled()) {
			return;
		}
		static std::chrono::steady_clock::time_point s_windowStart = std::chrono::steady_clock::now();
		static bool                                  s_windowVisible = g_visible.load(std::memory_order_relaxed);

		const auto now = std::chrono::steady_clock::now();
		const auto windowSeconds = std::chrono::duration<double>(now - s_windowStart).count();
		const bool edge = g_flushNow.exchange(false, std::memory_order_relaxed);
		if (!edge && windowSeconds < 5.0) {
			return;
		}
		// On an edge the ending window belongs to the PREVIOUS visibility.
		Flush(s_windowVisible, windowSeconds);
		s_windowStart = now;
		s_windowVisible = g_visible.load(std::memory_order_relaxed);
	}
}
