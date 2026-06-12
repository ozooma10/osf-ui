#include "composite/NullCompositor.h"

namespace SWUI
{
	bool NullCompositor::Initialize()
	{
		REX::INFO("NullCompositor: initialized (frames will be logged and dropped)");
		return true;
	}

	void NullCompositor::Shutdown()
	{
		REX::INFO("NullCompositor: shutdown after {} submitted frame(s)", _submitted);
	}

	void NullCompositor::Submit(const FrameBufferView& a_frame)
	{
		++_submitted;
		// First frame at INFO, then sampled at DEBUG so a visible tick loop
		// doesn't flood the log.
		if (_submitted == 1) {
			REX::INFO("NullCompositor: first frame submitted ({}x{}, stride {}, frame #{})",
				a_frame.width, a_frame.height, a_frame.strideBytes, a_frame.frameIndex);
		} else if (_submitted % 300 == 0) {
			REX::DEBUG("NullCompositor: {} frames submitted (latest #{}, {}x{})",
				_submitted, a_frame.frameIndex, a_frame.width, a_frame.height);
		}
	}
}
