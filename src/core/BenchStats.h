#pragma once

// Renderer-benchmark instrumentation (docs/renderer-benchmark.md). Off unless
// config benchStats is true; every probe early-outs on one relaxed atomic
// load, so shipping builds pay a branch per call site and nothing else.
//
// Four latency channels, one per cost pool the backends must be compared on
// (they run on different threads by design, which is why this is not a single
// profiler scope):
//   frame   — main-thread frame delta (game frame time), fed from FrameTickTask
//   tick    — main-thread overlay cost per tick (renderer Update + Submit)
//   present — present-hook overlay cost per drawn present (upload + composite
//             record/submit, including fence waits)
//   produce — in-process CPU cost to obtain one web frame (used by the
//             diagnostic WebView2 WGC readback path)
// plus two rate counters: frames PRODUCED by the web renderer and frames
// UPLOADED by the compositor (the effective on-screen UI rate).
//
// The main thread flushes every ~5 s and on every overlay visibility edge:
// one "Bench:" line per non-empty channel, vis-labelled, then reset — so a
// log window never mixes overlay-open and overlay-closed samples.

namespace OSFUI::bench
{
	enum class Channel
	{
		kFrame,
		kTick,
		kPresent,
		kProduce,

		kCount
	};

	void               SetEnabled(bool a_enabled);
	[[nodiscard]] bool Enabled();

	// Overlay visibility, mirrored from Runtime (same edge as the
	// "overlay visibility ->" log line). Forces a flush so windows stay pure.
	void SetVisible(bool a_visible);

	void Add(Channel a_channel, double a_ms);
	void CountProduced();
	void CountUploaded();

	// Main thread only. Emits + resets all channels every ~5 s.
	void FlushIfDue();

	// RAII probe: adds elapsed ms to the channel unless disabled.
	class Scope
	{
	public:
		explicit Scope(Channel a_channel) :
			_channel(a_channel), _armed(Enabled())
		{
			if (_armed) {
				_start = std::chrono::steady_clock::now();
			}
		}

		~Scope()
		{
			if (_armed) {
				Add(_channel, std::chrono::duration<double, std::milli>(
								  std::chrono::steady_clock::now() - _start)
								  .count());
			}
		}

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		std::chrono::steady_clock::time_point _start;
		Channel                               _channel;
		bool                                  _armed;
	};
}
