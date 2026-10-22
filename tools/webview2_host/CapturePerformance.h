#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace osfui::webview2
{
	// Host-STA only. Allocated only for an enabled developer HUD. Times are steady
	// clock milliseconds (QPC on Windows), never the coarse GetTickCount64 clock.
	class CapturePerformance
	{
		using json = nlohmann::json;
		struct Sample { double at{}, gap{}; };
		struct Channel
		{
			std::array<Sample, 2048> samples{};
			std::size_t head{}, count{};
			std::uint64_t total{};
			double worst{};
			std::optional<double> previous;
			void ClearWindow() { head = count = 0; previous.reset(); }
			void Add(double now)
			{
				const double gap = previous ? now - *previous : 0;
				samples[head] = { now, gap };
				head = (head + 1) % samples.size();
				count = (std::min)(count + 1, samples.size());
				previous = now;
				++total;
				worst = (std::max)(worst, gap);
			}
			json Snapshot(double now, double start, bool active) const
			{
				std::vector<double> gaps;
				std::size_t frames = 0;
				for (std::size_t i = 0; i < count; ++i) {
					const auto& sample = samples[(head + samples.size() - count + i) % samples.size()];
					if (sample.at <= now - 5000) continue;
					++frames;
					if (sample.gap > 0) gaps.push_back(sample.gap);
				}
				std::sort(gaps.begin(), gaps.end());
				const double elapsed = (std::min)(5000.0, now - start);
				return { { "fps", active && elapsed > 0 ? json(frames * 1000.0 / elapsed) : json(nullptr) },
					{ "frames", frames }, { "totalFrames", total }, { "sessionWorstGapMs", worst },
					{ "p95GapMs", gaps.empty() ? json(nullptr) : json(gaps[static_cast<std::size_t>(std::ceil(gaps.size() * .95)) - 1]) },
					{ "worstGapMs", gaps.empty() ? json(nullptr) : json(gaps.back()) },
					{ "sinceLastMs", active ? json(now - previous.value_or(start)) : json(nullptr) } };
			}
		};
	public:
		explicit CapturePerformance(std::string session, double now) : session_(std::move(session)), started_(now) {}
		static double Now()
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		const std::string& Session() const { return session_; }
		bool Active() const { return active_; }
		void SetActive(bool active, double now)
		{
			if (active == active_) return;
			if (active_) elapsed_ += now - resumed_;
			active_ = active;
			resumed_ = now;
			capture_.ClearWindow(); publish_.ClearWindow();
			scrollStart_.reset(); scrollUntil_ = 0;
			pendingCount_ = 0;
		}
		void Scrolling(bool scrolling, double now, double scrollAgeMs = 0)
		{
			// Confirm a gap only after the page reports scrolling through its end.
			// Otherwise the first damage-driven idle gap after a wheel gesture looks
			// like a hitch. Confirmation arrives with the next bounded HUD poll.
			const double lastScroll = now - scrollAgeMs;
			std::size_t kept = 0;
			for (std::size_t i = 0; i < pendingCount_; ++i) {
				const auto& candidate = pending_[i];
				const double end = started_ + candidate.at;
				if (now - end > 600) continue;
				if (lastScroll >= end) {
					hitches_[hitchHead_] = candidate;
					hitchHead_ = (hitchHead_ + 1) % hitches_.size();
					hitchCount_ = (std::min)(hitchCount_ + 1, hitches_.size());
					++totalHitches_;
				} else if (scrolling) {
					pending_[kept++] = candidate;
				}
			}
			pendingCount_ = kept;
			if (!active_ || !scrolling) { scrollStart_.reset(); scrollUntil_ = 0; return; }
			if (!scrollStart_ || now > scrollUntil_) scrollStart_ = now;
			scrollUntil_ = now + 600; // Four HUD polls/sec; never retain stale scrolling state indefinitely.
		}
		void Capture(double now) { Record(capture_, "capture", now); }
		void Published(double now) { Record(publish_, "publication", now); }
		void Deferred() { if (active_) ++deferrals_; }
		json Snapshot(double now) const
		{
			json hitches = json::array();
			for (std::size_t i = 0; i < hitchCount_; ++i) {
				const auto& h = hitches_[(hitchHead_ + hitches_.size() - hitchCount_ + i) % hitches_.size()];
				hitches.push_back({ { "stage", h.stage }, { "atMs", h.at }, { "gapMs", h.gap } });
			}
			return { { "source", "WebView2 WGC / shared-texture publication (host-wide, not game FPS)" },
				{ "session", session_ }, { "sampling", active_ }, { "windowMs", 5000 },
				{ "activeMs", elapsed_ + (active_ ? now - resumed_ : 0) },
				{ "capture", capture_.Snapshot(now, resumed_, active_) },
				{ "publication", publish_.Snapshot(now, resumed_, active_) },
				{ "queueDeferrals", deferrals_ }, { "scrollHitches", totalHitches_ },
				{ "unconfirmedScrollGaps", pendingCount_ },
				{ "hitchesOmitted", totalHitches_ - hitchCount_ }, { "hitches", std::move(hitches) },
				{ "scrolling", active_ && scrollStart_ && now <= scrollUntil_ } };
		}
	private:
		void Record(Channel& channel, const char* stage, double now)
		{
			if (!active_) return;
			if (channel.previous && scrollStart_ && *channel.previous >= *scrollStart_ && now <= scrollUntil_) {
				const auto gap = now - *channel.previous;
				if (gap > 50 && pendingCount_ < pending_.size()) {
					pending_[pendingCount_++] = { stage, now - started_, gap };
				}
			}
			channel.Add(now);
		}
		struct Hitch { const char* stage{}; double at{}, gap{}; };
		std::string session_;
		Channel capture_, publish_;
		std::array<Hitch, 64> hitches_{};
		std::array<Hitch, 64> pending_{};
		std::size_t pendingCount_{};
		std::size_t hitchHead_{}, hitchCount_{};
		std::uint64_t totalHitches_{}, deferrals_{};
		bool active_{};
		double started_{}, resumed_{}, elapsed_{}, scrollUntil_{};
		std::optional<double> scrollStart_;
	};
}
