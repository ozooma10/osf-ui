#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
	// Does nothing, successfully. Used when renderer="null" or as the fallback
	// when a requested backend cannot be created. Logs calls in dev mode.
	class NullWebRenderer final : public IWebRenderer
	{
	public:
		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void Resize(std::uint32_t a_width, std::uint32_t a_height) override;
		void Update(double a_deltaSeconds) override;
		std::optional<FrameBufferView> Render() override;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) override;

		[[nodiscard]] std::string_view Name() const override { return "null"; }

	private:
		bool _devMode{ false };
	};
}
