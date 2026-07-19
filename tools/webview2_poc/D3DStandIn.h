#pragma once

#include <cstdint>
#include <span>

#include <d3d11.h>
#include <wrl/client.h>

class D3DStandIn
{
public:
	bool Initialize(HWND a_window, std::uint32_t a_width, std::uint32_t a_height);
	void Resize(std::uint32_t a_width, std::uint32_t a_height);
	void UploadFrame(std::span<const std::uint8_t> a_pixels, std::uint32_t a_width,
		std::uint32_t a_height, std::uint32_t a_stride);
	void Render(double a_seconds);
	void SetOverlayVisible(bool a_visible) { _overlayVisible = a_visible; }

	[[nodiscard]] ID3D11Device* Device() const { return _device.Get(); }
	[[nodiscard]] ID3D11DeviceContext* Context() const { return _context.Get(); }

private:
	bool CreateBackBuffer();
	bool CreatePipeline();
	void ReleaseBackBuffer();

	Microsoft::WRL::ComPtr<ID3D11Device>           _device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext>    _context;
	Microsoft::WRL::ComPtr<IDXGISwapChain>         _swapChain;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _renderTarget;
	Microsoft::WRL::ComPtr<ID3D11VertexShader>     _vertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader>      _pixelShader;
	Microsoft::WRL::ComPtr<ID3D11SamplerState>     _sampler;
	Microsoft::WRL::ComPtr<ID3D11BlendState>       _premultipliedBlend;
	Microsoft::WRL::ComPtr<ID3D11Texture2D>        _overlayTexture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _overlayView;
	std::uint32_t _width{ 0 };
	std::uint32_t _height{ 0 };
	std::uint32_t _overlayWidth{ 0 };
	std::uint32_t _overlayHeight{ 0 };
	bool _overlayVisible{ true };
};
