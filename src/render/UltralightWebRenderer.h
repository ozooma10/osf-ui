#pragma once

// Optional Ultralight backend. Only compiled when the xmake option
// `with_ultralight` is enabled (defines SWUI_WITH_ULTRALIGHT=1 and requires
// the ULTRALIGHT_SDK_DIR environment variable). The SDK is proprietary and is
// never vendored into this repository.

#if defined(SWUI_WITH_ULTRALIGHT)

	#include "render/IWebRenderer.h"

namespace SWUI
{
	// STUB. Skeleton for an offscreen Ultralight-based HTML renderer.
	// Intentionally does not initialize the SDK yet — see the TODO list in the
	// .cpp and docs/renderer-plan.md (Phase 1) before filling this in.
	class UltralightWebRenderer final : public IWebRenderer
	{
	public:
		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void Resize(std::uint32_t a_width, std::uint32_t a_height) override;
		void Update(double a_deltaSeconds) override;
		std::optional<FrameBufferView> Render() override;
		void SendMessageToWeb(std::string_view a_json) override;

		[[nodiscard]] std::string_view Name() const override { return "ultralight"; }

	private:
		RendererConfig _config;
	};
}

#endif  // SWUI_WITH_ULTRALIGHT
