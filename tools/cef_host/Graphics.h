#pragma once
#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <functional>
#include <span>
#include <stdexcept>
#include "Wv2Messages.h"

namespace osfui::cefhost
{
    using Microsoft::WRL::ComPtr;
    struct GraphicsError : std::runtime_error
    {
        HRESULT code;
        bool borrowedCopyPending;
        GraphicsError(HRESULT hr, const char* operation, bool pending = false) :
            std::runtime_error(operation), code(hr), borrowedCopyPending(pending) {}
    };
    struct Surface
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> resource;
        UINT width{}, height{};
        void Clear() { texture.Reset(); resource.Reset(); width = height = 0; }
    };
    struct Layer
    {
        const Surface* surface{};
        float x{}, y{}, width{}, height{};
    };
    // All methods run on CEF's UI thread. No borrowed CEF texture escapes CopyPaint.
    class Graphics
    {
    public:
        using Send = std::function<bool(const nlohmann::json&)>;
        Graphics() = default;
        ~Graphics();
        void Initialize(LUID adapter, HANDLE game, Send send);
        void CopyPaint(HANDLE borrowed, const RECT& visible, Surface& destination);
        bool Publish(std::span<const Layer> layers, UINT width, UINT height, std::uint64_t epoch);
        void Acknowledge(UINT slot, std::uint64_t serial);

    private:
        void EnsureSurface(Surface& surface, UINT width, UINT height);
        void EnsureRing(UINT width, UINT height);
        void ReleaseRing();
        HANDLE Duplicate(HANDLE handle);
        struct Slot
        {
            Surface surface;
            ComPtr<ID3D11RenderTargetView> target;
            HANDLE handle{};
            std::uint64_t serial{}, acknowledged{};
        };
        std::array<Slot, wv2::kRingSlots> slots_;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11Device1> device1_;
        ComPtr<ID3D11Device5> device5_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11DeviceContext4> context4_;
        ComPtr<ID3D11Fence> produceFence_, copyFence_;
        ComPtr<ID3D11VertexShader> vertex_;
        ComPtr<ID3D11PixelShader> pixel_;
        ComPtr<ID3D11SamplerState> sampler_;
        ComPtr<ID3D11BlendState> blend_;
        ComPtr<ID3D11RasterizerState> raster_;
        HANDLE copyEvent_{}, game_{};
        LUID adapter_{};
        Send send_;
        UINT width_{}, height_{}, nextSlot_{};
        std::uint64_t serial_{}, copySerial_{};
    };
}
