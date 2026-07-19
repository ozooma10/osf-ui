#include "D3DStandIn.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi.h>

using Microsoft::WRL::ComPtr;

namespace
{
	void DebugHr(const char* a_where, HRESULT a_hr)
	{
		char text[256]{};
		sprintf_s(text, "osfui-webview2-poc: %s failed (0x%08X)\n", a_where, static_cast<unsigned>(a_hr));
		::OutputDebugStringA(text);
	}

	ComPtr<ID3DBlob> CompileShader(const char* a_source, const char* a_entry, const char* a_profile)
	{
		ComPtr<ID3DBlob> bytecode;
		ComPtr<ID3DBlob> errors;
		const auto hr = ::D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr,
			a_entry, a_profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
		if (FAILED(hr)) {
			if (errors) {
				::OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
			}
			DebugHr("D3DCompile", hr);
			return {};
		}
		return bytecode;
	}
}

bool D3DStandIn::Initialize(HWND a_window, std::uint32_t a_width, std::uint32_t a_height)
{
	_width = (std::max)(1u, a_width);
	_height = (std::max)(1u, a_height);

	DXGI_SWAP_CHAIN_DESC swap{};
	swap.BufferDesc.Width = _width;
	swap.BufferDesc.Height = _height;
	swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap.BufferDesc.RefreshRate = { 60, 1 };
	swap.SampleDesc = { 1, 0 };
	swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap.BufferCount = 2;
	swap.OutputWindow = a_window;
	swap.Windowed = TRUE;
	swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL requested[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
	};
	D3D_FEATURE_LEVEL actual{};
	const auto hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, static_cast<UINT>(std::size(requested)),
		D3D11_SDK_VERSION, &swap, &_swapChain, &_device, &actual, &_context);
	if (FAILED(hr)) {
		DebugHr("D3D11CreateDeviceAndSwapChain", hr);
		return false;
	}

	ComPtr<ID3D10Multithread> multithread;
	if (SUCCEEDED(_context.As(&multithread))) {
		multithread->SetMultithreadProtected(TRUE);
	}

	return CreateBackBuffer() && CreatePipeline();
}

bool D3DStandIn::CreateBackBuffer()
{
	ComPtr<ID3D11Texture2D> buffer;
	auto hr = _swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
	if (FAILED(hr)) {
		DebugHr("IDXGISwapChain::GetBuffer", hr);
		return false;
	}
	hr = _device->CreateRenderTargetView(buffer.Get(), nullptr, &_renderTarget);
	if (FAILED(hr)) {
		DebugHr("CreateRenderTargetView", hr);
		return false;
	}
	return true;
}

void D3DStandIn::ReleaseBackBuffer()
{
	_renderTarget.Reset();
}

bool D3DStandIn::CreatePipeline()
{
	static constexpr char kVertexShader[] = R"(
		struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
		Output main(uint id : SV_VertexID) {
			float2 positions[3] = {
				float2(-1.0, 1.0), float2(-1.0, -3.0), float2(3.0, 1.0)
			};
			float2 uvs[3] = {
				float2(0.0, 0.0), float2(0.0, 2.0), float2(2.0, 0.0)
			};
			Output o;
			o.position = float4(positions[id], 0.0, 1.0);
			o.uv = uvs[id];
			return o;
		})";
	static constexpr char kPixelShader[] = R"(
		Texture2D overlay : register(t0);
		SamplerState linearSampler : register(s0);
		float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
			return overlay.Sample(linearSampler, uv);
		})";

	const auto vs = CompileShader(kVertexShader, "main", "vs_5_0");
	const auto ps = CompileShader(kPixelShader, "main", "ps_5_0");
	if (!vs || !ps) {
		return false;
	}
	auto hr = _device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &_vertexShader);
	if (FAILED(hr)) {
		DebugHr("CreateVertexShader", hr);
		return false;
	}
	hr = _device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &_pixelShader);
	if (FAILED(hr)) {
		DebugHr("CreatePixelShader", hr);
		return false;
	}

	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	hr = _device->CreateSamplerState(&sampler, &_sampler);
	if (FAILED(hr)) {
		DebugHr("CreateSamplerState", hr);
		return false;
	}

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = _device->CreateBlendState(&blend, &_premultipliedBlend);
	if (FAILED(hr)) {
		DebugHr("CreateBlendState", hr);
		return false;
	}
	return true;
}

void D3DStandIn::Resize(std::uint32_t a_width, std::uint32_t a_height)
{
	if (!_swapChain || a_width == 0 || a_height == 0 ||
		(a_width == _width && a_height == _height)) {
		return;
	}
	_width = a_width;
	_height = a_height;
	ReleaseBackBuffer();
	_context->ClearState();
	const auto hr = _swapChain->ResizeBuffers(0, _width, _height, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) {
		DebugHr("ResizeBuffers", hr);
		return;
	}
	CreateBackBuffer();
}

void D3DStandIn::UploadFrame(std::span<const std::uint8_t> a_pixels,
	std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_stride)
{
	if (a_pixels.empty() || a_width == 0 || a_height == 0) {
		return;
	}
	if (!_overlayTexture || _overlayWidth != a_width || _overlayHeight != a_height) {
		_overlayView.Reset();
		_overlayTexture.Reset();

		D3D11_TEXTURE2D_DESC texture{};
		texture.Width = a_width;
		texture.Height = a_height;
		texture.MipLevels = 1;
		texture.ArraySize = 1;
		texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		texture.SampleDesc.Count = 1;
		texture.Usage = D3D11_USAGE_DEFAULT;
		texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		auto hr = _device->CreateTexture2D(&texture, nullptr, &_overlayTexture);
		if (FAILED(hr)) {
			DebugHr("CreateTexture2D(overlay)", hr);
			return;
		}
		hr = _device->CreateShaderResourceView(_overlayTexture.Get(), nullptr, &_overlayView);
		if (FAILED(hr)) {
			DebugHr("CreateShaderResourceView(overlay)", hr);
			_overlayTexture.Reset();
			return;
		}
		_overlayWidth = a_width;
		_overlayHeight = a_height;
	}
	_context->UpdateSubresource(_overlayTexture.Get(), 0, nullptr, a_pixels.data(), a_stride, 0);
}

void D3DStandIn::Render(double a_seconds)
{
	if (!_renderTarget) {
		return;
	}
	const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a_seconds * 1.7));
	const float drift = 0.5f + 0.5f * std::sin(static_cast<float>(a_seconds * 0.63 + 1.2));
	const float clear[] = {
		0.025f + 0.22f * pulse,
		0.04f + 0.15f * drift,
		0.12f + 0.20f * (1.0f - pulse),
		1.0f
	};
	_context->OMSetRenderTargets(1, _renderTarget.GetAddressOf(), nullptr);
	_context->ClearRenderTargetView(_renderTarget.Get(), clear);

	if (_overlayView && _overlayVisible) {
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(_width);
		viewport.Height = static_cast<float>(_height);
		viewport.MaxDepth = 1.0f;
		_context->RSSetViewports(1, &viewport);
		_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		_context->VSSetShader(_vertexShader.Get(), nullptr, 0);
		_context->PSSetShader(_pixelShader.Get(), nullptr, 0);
		_context->PSSetShaderResources(0, 1, _overlayView.GetAddressOf());
		_context->PSSetSamplers(0, 1, _sampler.GetAddressOf());
		const float factors[4]{};
		_context->OMSetBlendState(_premultipliedBlend.Get(), factors, 0xFFFFFFFFu);
		_context->Draw(3, 0);
		ID3D11ShaderResourceView* nullView = nullptr;
		_context->PSSetShaderResources(0, 1, &nullView);
	}

	_swapChain->Present(1, 0);
}
