#pragma once

#include "render/IWebRenderer.h"

namespace SWUI
{
	// Consumes CPU frames from a renderer and (eventually) presents them over
	// the game image. Submit() must finish using the FrameBufferView before
	// returning — the pixels are only valid for the duration of the call.
	class ICompositor
	{
	public:
		virtual ~ICompositor() = default;

		virtual bool Initialize() = 0;
		virtual void Shutdown() = 0;
		virtual void Submit(const FrameBufferView& a_frame) = 0;

		// Overlay visibility. A present-time compositor keeps drawing the last
		// frame every present, so it needs an explicit hide signal (Submit
		// simply stops being called when hidden, which is not observable from
		// the present hook). Default no-op for compositors that draw nothing.
		virtual void SetVisible(bool /*a_visible*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
