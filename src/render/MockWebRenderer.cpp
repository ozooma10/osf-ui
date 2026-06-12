#include "render/MockWebRenderer.h"

namespace SWUI
{
	bool MockWebRenderer::Initialize(const RendererConfig& a_config)
	{
		_devMode = a_config.devMode;
		_width = a_config.width;
		_height = a_config.height;
		_buffer.assign(static_cast<std::size_t>(_width) * _height * 4, 0);
		REX::INFO("MockWebRenderer: initialized ({}x{}, RGBA8 CPU buffer)", _width, _height);
		return true;
	}

	void MockWebRenderer::Shutdown()
	{
		_buffer.clear();
		_buffer.shrink_to_fit();
	}

	void MockWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		_loadedViewId = a_manifest.id;
		Resize(a_manifest.width, a_manifest.height);
		REX::INFO("MockWebRenderer: 'loaded' view '{}' from {} (metadata only; HTML is not rendered by the mock backend)",
			a_manifest.id, a_manifest.EntryPath().string());
	}

	void MockWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == _width && a_height == _height) {
			return;
		}
		_width = a_width;
		_height = a_height;
		_buffer.assign(static_cast<std::size_t>(_width) * _height * 4, 0);
		if (_devMode) {
			REX::DEBUG("MockWebRenderer: resized to {}x{}", _width, _height);
		}
	}

	void MockWebRenderer::Update(double a_deltaSeconds)
	{
		_elapsed += a_deltaSeconds;
	}

	std::optional<FrameBufferView> MockWebRenderer::Render()
	{
		if (_buffer.empty()) {
			return std::nullopt;
		}

		FillTestPattern();
		++_frameIndex;

		return FrameBufferView{
			.pixels = _buffer,
			.width = _width,
			.height = _height,
			.strideBytes = _width * 4,
			.format = PixelFormat::kRGBA8,
			.frameIndex = _frameIndex,
		};
	}

	void MockWebRenderer::SendMessageToWeb(std::string_view a_json)
	{
		// No JS engine to deliver to; visible in dev mode so bridge plumbing
		// can be traced end to end.
		if (_devMode) {
			REX::DEBUG("MockWebRenderer: native->web message (dropped, no JS engine): {}", a_json);
		}
	}

	void MockWebRenderer::FillTestPattern()
	{
		// Animated diagonal gradient: enough structure to verify orientation,
		// stride, and channel order once a real compositor exists.
		const auto phase = static_cast<std::uint32_t>(_elapsed * 60.0);
		for (std::uint32_t y = 0; y < _height; ++y) {
			auto* row = _buffer.data() + static_cast<std::size_t>(y) * _width * 4;
			for (std::uint32_t x = 0; x < _width; ++x) {
				auto* px = row + static_cast<std::size_t>(x) * 4;
				px[0] = static_cast<std::uint8_t>((x + phase) & 0xFF);  // R
				px[1] = static_cast<std::uint8_t>((y + phase) & 0xFF);  // G
				px[2] = static_cast<std::uint8_t>((x ^ y) & 0xFF);      // B
				px[3] = 0xC0;                                           // A: semi-transparent
			}
		}
	}
}
