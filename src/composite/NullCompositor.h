#pragma once

#include "composite/ICompositor.h"

namespace SWUI
{
	// Logs frame submissions and draws nothing. The default compositor until a
	// real presentation path exists.
	class NullCompositor final : public ICompositor
	{
	public:
		bool Initialize() override;
		void Shutdown() override;
		void Submit(const FrameBufferView& a_frame) override;

		[[nodiscard]] std::string_view Name() const override { return "null"; }

	private:
		std::uint64_t _submitted{ 0 };
	};
}
