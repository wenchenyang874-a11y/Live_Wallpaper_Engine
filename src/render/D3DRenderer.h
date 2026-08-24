#pragma once

#include <chrono>
#include <cstdint>
#include <span>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

namespace lwe::render {

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
    HRESULT AcquireBackBuffer(
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& backBuffer) const;
    bool PresentCurrentFrame(bool synchronize = true);
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] ID3D11Device* Device() const noexcept;

private:
    bool CreateDevice();
    bool CreateSwapChain(HWND targetWindow, UINT width, UINT height);
    bool CreateRenderTarget();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
};

}  // namespace lwe::render
