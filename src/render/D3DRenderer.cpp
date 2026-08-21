#include "render/D3DRenderer.h"

#include <array>
#include <cmath>
#include <sstream>

#include "core/Logger.h"

namespace lwe::render {

D3DRenderer::~D3DRenderer() {
    Shutdown();
}

bool D3DRenderer::Initialize(const HWND targetWindow) {
    RECT client{};
    if (!GetClientRect(targetWindow, &client)) {
        core::LogError(L"Unable to read the wallpaper window size.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        core::LogError(L"Wallpaper window is empty; D3D initialization was cancelled.");
        return false;
    }

    return CreateDevice() && CreateSwapChain(targetWindow, width, height) && CreateRenderTarget();
}

bool D3DRenderer::CreateDevice() {
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL selectedLevel{};
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevels.data(),
        static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, &device_, &selectedLevel,
        &context_);

    if (FAILED(result)) {
        core::LogWarning(L"Hardware D3D11 device creation failed; trying WARP software rendering.");
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, featureLevels.data(),
            static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, &device_, &selectedLevel,
            &context_);
    }

    if (FAILED(result)) {
        core::LogError(L"D3D11 device creation failed.", result);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device_.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description))) {
            core::LogInfo(L"D3D11 adapter: " + std::wstring(description.Description));
        }
    }

    std::wostringstream message;
    message << L"D3D11 feature level selected: 0x" << std::hex
            << static_cast<unsigned int>(selectedLevel) << L'.';
    core::LogInfo(message.str());
    return true;
}

bool D3DRenderer::CreateSwapChain(const HWND targetWindow, const UINT width, const UINT height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;

    HRESULT result = device_.As(&dxgiDevice);
    if (SUCCEEDED(result)) {
        result = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result)) {
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the DXGI factory.", result);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = width;
    description.BufferDesc.Height = height;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = targetWindow;
    description.Windowed = TRUE;

    // Windows 11 raised desktop requires a WS_EX_LAYERED child. A normal HWND
    // flip-model swap chain does not honor SetLayeredWindowAttributes reliably,
    // so this compatibility spike intentionally uses the D3D11 BitBlt model.
    // The production optimization path is DirectComposition plus a composition
    // flip-model swap chain, which will be evaluated after embedding is stable.
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    result = factory->CreateSwapChain(device_.Get(), &description, &swapChain_);
    if (FAILED(result)) {
        core::LogError(L"DXGI swap chain creation failed.", result);
        return false;
    }

    factory->MakeWindowAssociation(targetWindow,
                                   DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    core::LogInfo(L"DXGI BitBlt swap chain created for layered desktop compatibility.");
    return true;
}

bool D3DRenderer::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(result)) {
        core::LogError(L"Unable to obtain the swap chain back buffer.", result);
        return false;
    }

    const HRESULT viewResult =
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(viewResult)) {
        core::LogError(L"Unable to create the D3D11 render target.", viewResult);
        return false;
    }
    return true;
}

bool D3DRenderer::Resize(const UINT width, const UINT height) {
    if (!IsInitialized() || width == 0 || height == 0) {
        return true;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    context_->Flush();

    const HRESULT result =
        swapChain_->ResizeBuffers(1, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(result)) {
        core::LogError(L"DXGI ResizeBuffers failed.", result);
        return false;
    }
    return CreateRenderTarget();
}

bool D3DRenderer::Render(const std::chrono::steady_clock::duration elapsed) {
    if (!IsInitialized()) {
        return false;
    }

    const float seconds = std::chrono::duration<float>(elapsed).count();
    // The spike used to change so slowly that a successful launch looked inert.
    // A brighter five-second blue/teal pulse makes rendering visibly testable.
    const float slowWave = (std::sin(seconds * 1.25F) + 1.0F) * 0.5F;
    const float secondWave = (std::sin(seconds * 0.85F + 1.7F) + 1.0F) * 0.5F;
    const float clearColor[4]{
        0.020F + slowWave * 0.040F,
        0.080F + secondWave * 0.220F,
        0.150F + slowWave * 0.300F,
        1.0F,
    };

    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);

    const HRESULT result = swapChain_->Present(1, 0);
    if (result == DXGI_STATUS_OCCLUDED) {
        Sleep(100);
        return true;
    }
    if (FAILED(result)) {
        core::LogError(L"DXGI Present failed.", result);
        return false;
    }
    return true;
}

bool D3DRenderer::PresentStaticImage(const std::span<const std::uint8_t> bgraPixels,
                                     const UINT width, const UINT height,
                                     const UINT stride) {
    const std::uint64_t requiredStride = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t requiredBytes = static_cast<std::uint64_t>(stride) * height;
    if (!IsInitialized() || width == 0 || height == 0 || stride < requiredStride ||
        requiredBytes > bgraPixels.size()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the static wallpaper back buffer.", result);
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (description.Width != width || description.Height != height ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        core::LogError(L"Static wallpaper dimensions do not match the swap chain.");
        return false;
    }

    context_->UpdateSubresource(backBuffer.Get(), 0, nullptr, bgraPixels.data(), stride, 0);
    result = swapChain_->Present(1, 0);
    if (FAILED(result)) {
        core::LogError(L"Static wallpaper Present failed.", result);
        return false;
    }

    return true;
}

void D3DRenderer::Shutdown() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    renderTarget_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

bool D3DRenderer::IsInitialized() const noexcept {
    return device_ && context_ && swapChain_ && renderTarget_;
}

}  // namespace lwe::render
