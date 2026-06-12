#include "composite/D3D12Compositor.h"

#include "core/Log.h"

namespace SWUI
{
	bool D3D12Compositor::Initialize()
	{
		// Refuse to pretend: initializing "successfully" here would let the
		// runtime believe frames are being presented. Fail so the runtime
		// falls back to NullCompositor and the log says why.
		REX::ERROR("D3D12Compositor: not implemented — no device/queue/present access has been "
				   "reverse engineered yet (see docs/reverse-engineering-notes.md). "
				   "Falling back to the null compositor.");
		return false;
	}

	void D3D12Compositor::Shutdown() {}

	void D3D12Compositor::Submit(const FrameBufferView&)
	{
		static std::once_flag once;
		Log::WarnOnce(once, "D3D12Compositor: Submit called on unimplemented stub; frames are dropped");
	}
}
