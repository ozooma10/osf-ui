#include "Graphics.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <limits>

namespace osfui::cefhost
{
    namespace
    {
        void Check(HRESULT hr, const char* operation)
        {
            if (FAILED(hr)) throw GraphicsError(hr, operation);
        }
        constexpr char kShader[] = R"(
            struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
            Vertex VS(uint id : SV_VertexID) {
                Vertex v; v.uv = float2((id << 1) & 2, id & 2);
                v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1); return v;
            }
            Texture2D image : register(t0); SamplerState linearSampler : register(s0);
            float4 PS(Vertex v) : SV_TARGET { return image.Sample(linearSampler, v.uv); }
        )";
    }
    Graphics::~Graphics()
    {
        ReleaseRing();
        if (copyEvent_) CloseHandle(copyEvent_);
    }
    void Graphics::Initialize(LUID adapter, HANDLE game, Send send)
    {
        game_ = game;
        adapter_ = adapter;
        send_ = std::move(send);
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter> dxgiAdapter;
        Check(factory->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&dxgiAdapter)), "Find Starfield graphics adapter");
        constexpr D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        Check(D3D11CreateDevice(dxgiAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
            &device_, nullptr, &context_), "Create CEF composition device");
        Check(device_.As(&device1_), "Query ID3D11Device1");
        Check(device_.As(&device5_), "Query ID3D11Device5");
        Check(context_.As(&context4_), "Query ID3D11DeviceContext4");
        Check(device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&produceFence_)), "Create produce fence");
        Check(device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_)), "Create copy fence");
        copyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!copyEvent_) throw GraphicsError(HRESULT_FROM_WIN32(GetLastError()), "Create copy completion event");
        ComPtr<ID3DBlob> vertex, pixel, errors;
        Check(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vertex, &errors), "Compile composition vertex shader");
        Check(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pixel, &errors), "Compile composition pixel shader");
        Check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertex_), "Create vertex shader");
        Check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &pixel_), "Create pixel shader");
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sample, &sampler_), "Create sampler");
        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device_->CreateBlendState(&blend, &blend_), "Create premultiplied alpha blend");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        Check(device_->CreateRasterizerState(&raster, &raster_), "Create rasterizer");
    }
    void Graphics::EnsureSurface(Surface& surface, UINT width, UINT height)
    {
        if (surface.texture && surface.width == width && surface.height == height) return;
        if (!width || !height || width > 16384 || height > 16384)
            throw GraphicsError(E_INVALIDARG, "Invalid CEF texture dimensions");
        surface.Clear();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Check(device_->CreateTexture2D(&desc, nullptr, &surface.texture), "Create owned CEF surface");
        Check(device_->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.resource), "Create surface SRV");
        surface.width = width; surface.height = height;
    }
    void Graphics::CopyPaint(HANDLE borrowed, const RECT& visible, Surface& destination)
    {
        ComPtr<ID3D11Texture2D> source;
        Check(device1_->OpenSharedResource1(borrowed, IID_PPV_ARGS(&source)), "Open CEF paint texture on game adapter");
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1)
            throw GraphicsError(E_INVALIDARG, "Unsupported CEF paint texture format");
        // CEF's allocation may include padding outside the visible frame.
        if (visible.left < 0 || visible.top < 0 || visible.right <= visible.left || visible.bottom <= visible.top ||
            static_cast<UINT>(visible.right) > desc.Width || static_cast<UINT>(visible.bottom) > desc.Height)
            throw GraphicsError(E_INVALIDARG, "Invalid CEF visible texture rectangle");
        EnsureSurface(destination, visible.right - visible.left, visible.bottom - visible.top);
        const D3D11_BOX box{ static_cast<UINT>(visible.left), static_cast<UINT>(visible.top), 0,
            static_cast<UINT>(visible.right), static_cast<UINT>(visible.bottom), 1 };
        context_->CopySubresourceRegion(destination.texture.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
        // CEF may reuse the source after this callback. Submitting a copy is not completion.
        const auto serial = ++copySerial_;
        const auto signalled = context4_->Signal(copyFence_.Get(), serial);
        if (FAILED(signalled)) throw GraphicsError(signalled, "Signal borrowed texture copy", true);
        context_->Flush();
        if (copyFence_->GetCompletedValue() < serial) {
            const auto armed = copyFence_->SetEventOnCompletion(serial, copyEvent_);
            if (FAILED(armed)) throw GraphicsError(armed, "Arm borrowed texture completion", true);
            if (WaitForSingleObject(copyEvent_, 2000) != WAIT_OBJECT_0)
                throw GraphicsError(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "CEF paint copy did not complete", true);
        }
        if (copyFence_->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())
            throw GraphicsError(DXGI_ERROR_DEVICE_REMOVED, "CEF copy device removed", true);
    }
    HANDLE Graphics::Duplicate(HANDLE handle)
    {
        HANDLE remote{};
        if (!DuplicateHandle(GetCurrentProcess(), handle, game_, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
            throw GraphicsError(HRESULT_FROM_WIN32(GetLastError()), "Duplicate CEF texture handle into game");
        return remote;
    }
    void Graphics::ReleaseRing()
    {
        for (auto& slot : slots_) {
            if (slot.handle) CloseHandle(slot.handle);
            slot = {};
        }
        width_ = height_ = 0;
    }
    void Graphics::EnsureRing(UINT width, UINT height)
    {
        if (width_ == width && height_ == height) return;
        if (!width || !height || width > 16384 || height > 16384)
            throw GraphicsError(E_INVALIDARG, "Invalid output dimensions");
        ReleaseRing();
        std::vector<HANDLE> remote;
        HANDLE localFence{};
        try {
            wv2::msg::Textures message{ .width = width, .height = height,
                .adapterLuidLow = adapter_.LowPart, .adapterLuidHigh = static_cast<UINT>(adapter_.HighPart) };
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            for (auto& slot : slots_) {
                Check(device_->CreateTexture2D(&desc, nullptr, &slot.surface.texture), "Create shared CEF ring texture");
                Check(device_->CreateRenderTargetView(slot.surface.texture.Get(), nullptr, &slot.target), "Create ring RTV");
                ComPtr<IDXGIResource1> resource;
                Check(slot.surface.texture.As(&resource), "Query shared resource");
                Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr, &slot.handle), "Create ring handle");
                const auto duplicated = Duplicate(slot.handle);
                remote.push_back(duplicated);
                message.slots.push_back(reinterpret_cast<UINT64>(duplicated));
            }
            Check(produceFence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &localFence), "Share produce fence");
            const auto duplicated = Duplicate(localFence);
            CloseHandle(localFence); localFence = nullptr;
            remote.push_back(duplicated);
            message.produceFence = reinterpret_cast<UINT64>(duplicated);
            if (!send_(wv2::msg::ToJson(message))) throw GraphicsError(E_FAIL, "Send CEF texture ring");
            remote.clear(); // ownership transferred to the game
            width_ = width; height_ = height;
        } catch (...) {
            if (localFence) CloseHandle(localFence);
            for (HANDLE handle : remote) {
                HANDLE local{};
                if (DuplicateHandle(game_, handle, GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) CloseHandle(local);
            }
            ReleaseRing();
            throw;
        }
    }
    bool Graphics::Publish(std::span<const Layer> layers, UINT width, UINT height, std::uint64_t epoch)
    {
        EnsureRing(width, height);
        UINT index = wv2::kRingSlots;
        for (UINT i = 0; i < slots_.size(); ++i) {
            const auto candidate = (nextSlot_ + i) % static_cast<UINT>(slots_.size());
            const auto& slot = slots_[candidate];
            if (!slot.serial || slot.acknowledged >= slot.serial) { index = candidate; break; }
        }
        if (index == wv2::kRingSlots) return false; // normal backpressure; retain owned view surfaces
        auto& slot = slots_[index];
        constexpr float transparent[4]{};
        context_->ClearRenderTargetView(slot.target.Get(), transparent);
        context_->OMSetRenderTargets(1, slot.target.GetAddressOf(), nullptr);
        context_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
        context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_.Get(), nullptr, 0);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        for (const auto& layer : layers) {
            if (!layer.surface || !layer.surface->resource || layer.width <= 0 || layer.height <= 0) continue;
            D3D11_VIEWPORT viewport{ layer.x, layer.y, layer.width, layer.height, 0, 1 };
            context_->RSSetViewports(1, &viewport);
            context_->PSSetShaderResources(0, 1, layer.surface->resource.GetAddressOf());
            context_->Draw(3, 0);
        }
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        const auto serial = ++serial_;
        Check(context4_->Signal(produceFence_.Get(), serial), "Signal CEF frame fence");
        context_->Flush();
        slot.serial = serial;
        nextSlot_ = (index + 1) % static_cast<UINT>(slots_.size());
        if (!send_(wv2::msg::ToJson(wv2::msg::Frame{ .slot = index, .serial = serial,
            .width = width, .height = height, .presentationEpoch = epoch })))
            throw GraphicsError(E_FAIL, "Send CEF frame");
        return true;
    }
    void Graphics::Acknowledge(UINT slot, std::uint64_t serial)
    {
        if (slot < slots_.size() && serial <= serial_)
            slots_[slot].acknowledged = (std::max)(slots_[slot].acknowledged, serial);
    }
}
