#pragma once

#include "Render/IWebRenderer.h"

namespace OSFUI
{
	struct CompositorStatus
	{
		bool seamActive{ false };
		bool frameGeneration{ false };
	};

	// Consumes shared-texture frames and composites them over the game image.
	class ICompositor
	{
	public:
		virtual ~ICompositor() = default;

		virtual bool Initialize() = 0;
		virtual void Submit(const FrameBufferView& a_frame) = 0;

		virtual void SetVisible(bool /*a_visible*/) {}

		virtual void SetOutputResizeCallback(std::function<void(std::uint32_t a_width, std::uint32_t a_height)> /*a_callback*/) {}

		virtual bool IsOutputSizeKnown() const { return true; }

		virtual void SetSharedRing(const SharedRingDesc& /*a_desc*/) {}

		virtual void SetSeamDrawMode(bool /*a_enabled*/) {}
		virtual CompositorStatus GetStatus() const { return {}; }
		virtual std::string_view Name() const = 0;
	};
}
