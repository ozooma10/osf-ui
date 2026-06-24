#pragma once

#include "render/IWebRenderer.h"

namespace PrismaSF
{
	// Produces a generated RGBA test pattern on the CPU. Exists purely to prove
	// the renderer -> compositor pipeline compiles and moves plausible data; it
	// does not interpret any HTML/CSS/JS.
	class MockWebRenderer final : public IWebRenderer
	{
	public:
		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void Resize(std::uint32_t a_width, std::uint32_t a_height) override;
		void Update(double a_deltaSeconds) override;
		std::optional<FrameBufferView> Render() override;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) override;

		[[nodiscard]] std::string_view Name() const override { return "mock"; }

	private:
		void FillTestPattern();

		std::vector<std::uint8_t> _buffer;
		std::uint32_t             _width{ 0 };
		std::uint32_t             _height{ 0 };
		std::uint64_t             _frameIndex{ 0 };
		double                    _elapsed{ 0.0 };
		bool                      _devMode{ false };
		std::string               _loadedViewId;
	};
}
