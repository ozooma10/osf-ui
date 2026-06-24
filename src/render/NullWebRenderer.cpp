#include "render/NullWebRenderer.h"

#include "core/Log.h"

namespace PrismaSF
{
	bool NullWebRenderer::Initialize(const RendererConfig& a_config)
	{
		_devMode = a_config.devMode;
		REX::INFO("NullWebRenderer: initialized ({}x{})", a_config.width, a_config.height);
		return true;
	}

	void NullWebRenderer::Shutdown()
	{
		if (_devMode) {
			REX::DEBUG("NullWebRenderer: Shutdown");
		}
	}

	void NullWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		if (_devMode) {
			REX::DEBUG("NullWebRenderer: LoadView '{}' (no-op)", a_manifest.id);
		}
	}

	void NullWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (_devMode) {
			REX::DEBUG("NullWebRenderer: Resize {}x{} (no-op)", a_width, a_height);
		}
	}

	void NullWebRenderer::Update(double)
	{
		// intentionally silent: called per tick
	}

	std::optional<FrameBufferView> NullWebRenderer::Render()
	{
		return std::nullopt;
	}

	void NullWebRenderer::SendMessageToWeb(std::string_view a_viewId, std::string_view a_json)
	{
		if (_devMode) {
			REX::DEBUG("NullWebRenderer: dropping native->web message to '{}': {}", a_viewId, a_json);
		}
	}
}
