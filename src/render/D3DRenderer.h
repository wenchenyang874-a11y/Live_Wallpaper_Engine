#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

namespace lwe::render {

struct ImageRegion final {
    std::span<const std::uint8_t> bgraPixels;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    LONG left = 0;
    LONG top = 0;
};

class D3DRenderer final {
public:
    D3DRenderer() = default;
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    bool Initialize(HWND targetWindow);
    bool Resize(UINT width, UINT height);
    bool Render(std::chrono::steady_clock::duration elapsed);
    bool PresentStaticImage(std::span<const std::uint8_t> bgraPixels, UINT width,
                            UINT height, UINT stride);
    bool PresentImageRegions(std::span<const ImageRegion> regions);
    HRESULT AcquireBackBuffer(
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& backBuffer) const;
    HRESULT AcquireVideoTransferSurface(UINT sourceWidth, UINT sourceHeight,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& transferSurface);
    bool CommitVideoTransferSurface(std::span<const RECT> destinations,
                                    UINT sourceWidth, UINT sourceHeight);
    bool PresentCurrentFrame(bool synchronize = true);
    [[nodiscard]] std::optional<std::uint64_t> VideoMemoryUsage() const;
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] ID3D11Device* Device() const noexcept;

private:
    bool CreateDevice();
    bool CreateSwapChain(HWND targetWindow, UINT width, UINT height);
    bool CreateRenderTarget();
    bool CreateVideoCompositionPipeline();
    bool EnsureVideoCompositeSurface(UINT width, UINT height);
    bool UpdateVideoVertices(std::span<const RECT> destinations,
                             UINT sourceWidth, UINT sourceHeight,
                             UINT targetWidth, UINT targetHeight);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> videoTransferSurface_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> videoCompositeSurface_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> videoCompositeTarget_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> videoVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> videoPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> videoInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> videoVertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> videoSampler_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> videoSourceView_;
    UINT videoVertexCount_ = 0;
};

}  // namespace lwe::render
